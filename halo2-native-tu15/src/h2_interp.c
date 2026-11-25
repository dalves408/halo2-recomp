/* Interpolation: smooth movement between Halo 2's 30 Hz game updates.
 *
 * Halo 2 runs its world at 30 updates a second. At 60 fps every update is
 * drawn twice: the view turns on every frame (the game reads look input each
 * frame), but the player's position -- walking, strafing, riding -- and
 * everything in the world only move on updates, so movement steps at 30 Hz
 * (measured 2026-11-23 with RECOMP_FLIP_HASH: walking, the in-between frame
 * changed 0.05 against 4.3 on an update frame).
 *
 * Where it hooks. The main loop sub_0012DF60 (1.0 0x12b450) runs the due
 * updates (sub_0013AA50), then the per-frame update (sub_0013A9B0) that places
 * each player's camera from their body, then draws (sub_00229C50). The camera,
 * the first-person arms and gun are all derived from the body during that
 * stretch, so the blend goes into the body itself: after the updates
 * (h2_interp_world_updated) each player's body position is replaced by a blend
 * of its last two updates, and after the draw (h2_interp_render_end) the
 * game's own position is put back, so the next update, AI and physics never
 * see it. Moving the camera alone does not work: the first-person gun is
 * placed from the unblended body and slides against the view.
 *
 * Found 2026-11-23: players data array (header pointer at 0x4F54F4, 0x21c-byte
 * records, the body's object handle at +0x2C); object data array (header
 * pointer at 0x4ECBD0, 12-byte entries: salt at +0, data pointer at +8); an
 * object's position at +0x64. Raising the body's position by 0.5 after the
 * updates raised the drawn camera by 0.5 in 119 of 119 frames.
 *
 * The blend runs one frame behind the newest update: the frame an update
 * lands shows the previous position moved one frame's worth toward the new
 * one, and the last frame before the next update shows the new one. At 60 fps
 * that is the midpoint and then the new position: even steps, 16.7 ms of
 * extra delay on position only (turning stays immediate). A jump of more than 5 units (respawn, teleport,
 * cutscene cut) is not blended.
 *
 * Measured on a test PC (Uprising, walking, 60 Hz): the in-between frame changed
 * 0.0-0.1 before, 0.6-1.4 with the blend (update frames 1.0-2.0); the gun's
 * screen position held within 1 px frame to frame, as without the blend.
 *
 * Stage 2: every object. Objects are drawn from their node matrices (the
 * world transform of each bone, real_matrix4x3: scale, forward, left, up,
 * position; 52 bytes), built on updates. Each object's header has the node
 * block's offset and size at +0x114 (u16 size, u16 offset; the player's body:
 * 55 nodes at +0x4AC). Each object's position and nodes are blended the same
 * way as the body was in stage 1 (the axes re-normalised after the blend).
 * Off unless RECOMP_HALO2_INTERP=1.
 *
 * Cutscene cameras step with the updates too and are not objects; they are
 * blended where the draw copies them (see "the camera, for cutscenes").
 *
 * Test: RECOMP_HALO2_INTERP_LIFT=<units> raises every object's nodes by that
 * much for drawing instead of blending (does the draw use these matrices?). */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "gen/recomp_types.h"
#include "kernel/recomp_clip.h"   /* g_rclip_note: one line per frame for test clips */

#define GAME_TICK_COUNTER 0x4F2C5Cu
#define OBJECT_ARRAY_PTR  0x4ECBD0u   /* -> the object data array's header */
#define ARRAY_MAX         0x20u       /* header: u16 capacity */
#define ARRAY_DATA        0x44u       /* header: data pointer */
#define OBJECT_POSITION   0x64u
#define OBJECT_NODE_BLOCK 0x114u      /* u16 size, u16 offset of the node matrices */
#define NODE_FLOATS       13
#define NODE_BYTES        52u
#define MAX_NODES         160
#define MAX_OBJECTS       2048
#define SNAP_DISTANCE     5.0f
#define GUEST_HIGH_END    0x84000000u
#define DATA_ARRAY_SIG    0x64407440u   /* 'd@t@' at header +0x2C */
#define RENDER_CAMERA     0x4C65C0u   /* position first; copied from a view per render pass */

/* One tracked object: its position (3 floats) then its nodes (13 floats each). */
typedef struct {
    uint32_t handle;            /* 0: not tracked */
    uint32_t data;              /* guest address of the object */
    uint32_t nodes_off, n;      /* node block offset; floats per snapshot (3 + 13 * nodes) */
    double tick_time;           /* host seconds when this update was first seen */
    float *buf;                 /* one allocation: prev, cur, shown (swapped) */
    float *prev, *cur, *shown;  /* n floats each */
    int moved;                  /* shown was written this frame */
    float step, last_step;      /* distance moved by the last update, and by the one before */
    float node_step;            /* the most any node moved in recent updates (fading) */
    uint32_t shown_frame;       /* the frame shown was last written */
    int snapped;                /* the last update was a teleport: show it as it is */
    uint32_t *node_seen;        /* per node: the frame it last changed */
    uint8_t *node_run;          /* per node: changes on consecutive frames in a row (2+: the game moves it every frame) */
    uint8_t *node_kept;         /* per node: updates left that the game rebuilt it during the frame (left alone meanwhile) */
} Obj;

/* A node the game moves every frame itself (a cutscene character's head and
 * arms between updates, measured 2026-11-24 in the Arbiter intro: 17 of 48
 * nodes): its change is no update, and the blend leaves it as the game has it. */
static int s_no_live;   /* test: RECOMP_HALO2_INTERP_NOLIVE=1 treats every node as updated with the world */
static int s_fp_rule;        /* first-person blend; RECOMP_HALO2_INTERP_FP=0 turns it off (test) */
static int s_cam_cut_rule;   /* the held-still / big-step camera cut rule; RECOMP_HALO2_INTERP_CAMCUT=0 turns it off (test) */
/* Also a node the game rebuilds during the frame, after the blend (a talking
 * Brute's jaw, 2026-11-24): blended, it showed the game's pose on update frames
 * and the half-step-behind blend between them, and the jaw flapped. */
#define NODE_LIVE(o, j) (!s_no_live && ((o)->node_run[j] >= 2 || (o)->node_kept[j]))

static Obj s_obj[MAX_OBJECTS];
static int s_on = -1;
static double s_qpc_scale;
/* The frame interval (host seconds, smoothed). The frame an update is first
 * seen shows it one frame's worth along from the previous one, so the last
 * frame before the next update reaches it: even steps at any refresh rate
 * (60 Hz: halfway, then all the way). */
static double s_frame_dt = 1.0 / 60.0, s_last_frame_time;
static uint32_t s_frame;
static unsigned s_dbg_everyframe, s_dbg_live_only;   /* every-frame nodes left alone; changes only in those */
static uint32_t s_objlog_until;   /* test: RECOMP_HALO2_INTERP_OBJLOG traces objects until this frame */
static unsigned s_dbg_changed, s_dbg_late, s_dbg_rej[4];   /* rejected: 0 position, 1 scale, 2 axes, 3 node far from the object */   /* changes seen at the update hook; changes the game made after it (seen at the draw's end) */
static unsigned s_dbg_frames, s_dbg_blended, s_dbg_snap, s_dbg_objects, s_dbg_restored, s_dbg_kept, s_dbg_new;
static float s_lift, s_lift_on;   /* lift test: amount; amount while its flag file exists */
static int s_lift_mode = 1;       /* the flag file's first digit: 1 nodes, 2 position, 4 bounding centres, +8 odd frames only */
static float s_centre_z[MAX_OBJECTS][3];

static int interp_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_HALO2_INTERP");
        LARGE_INTEGER f;
        s_on = e && e[0] == '1';
        s_no_live = getenv("RECOMP_HALO2_INTERP_NOLIVE") != NULL;
        s_fp_rule = !(getenv("RECOMP_HALO2_INTERP_FP") && getenv("RECOMP_HALO2_INTERP_FP")[0] == '0');
        s_cam_cut_rule = !(getenv("RECOMP_HALO2_INTERP_CAMCUT") && getenv("RECOMP_HALO2_INTERP_CAMCUT")[0] == '0');
        QueryPerformanceFrequency(&f);
        s_qpc_scale = 1.0 / (double)f.QuadPart;
        if (s_on && getenv("RECOMP_HALO2_INTERP_LIFT"))
            s_lift = (float)atof(getenv("RECOMP_HALO2_INTERP_LIFT"));
        if (s_on)
            fprintf(stderr, "[INTERP] object interpolation on (test)%s\n", s_lift ? ", lift test" : "");
    }
    return s_on;
}

static float getf(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }
static void putf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

static double now_seconds(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * s_qpc_scale;
}

/* ---- the probe (test): RECOMP_HALO2_INTERP_PROBE=1 with RECOMP_HALO2_INTERP_PROBE_VA="hex,..."
 * raises each candidate position's z by 0.5 after the updates, a different one every 120 frames,
 * and counts the frames whose drawn camera rose with it. RECOMP_HALO2_INTERP_PROBE_FLAG=<file>
 * starts it when the file appears (else at frame RECOMP_HALO2_INTERP_PROBE_AT, 4200). */
static uint32_t s_probe_va[8];
static int s_probe_n;
static int s_probe = -1, s_probe_k = -1, s_probe_active;
static uint32_t s_probe_at = 4200;
static float s_probe_saved, s_probe_base_z;
static unsigned s_probe_hits[8], s_probe_tries[8];

static void probe_init(void)
{
    const char *e = getenv("RECOMP_HALO2_INTERP_PROBE"), *l = getenv("RECOMP_HALO2_INTERP_PROBE_VA");
    s_probe = e && e[0] == '1' && l && *l;
    if (!s_probe)
        return;
    if (getenv("RECOMP_HALO2_INTERP_PROBE_AT"))
        s_probe_at = (uint32_t)strtoul(getenv("RECOMP_HALO2_INTERP_PROBE_AT"), NULL, 10);
    if (getenv("RECOMP_HALO2_INTERP_PROBE_FLAG"))
        s_probe_at = 0xFFFFFF00u;
    while (*l && s_probe_n < 8) {
        char *end;
        s_probe_va[s_probe_n++] = (uint32_t)strtoul(l, &end, 16);
        if (end == l) break;
        l = *end == ',' ? end + 1 : end;
    }
}

static void probe_restore(void)
{
    if (s_probe_active) {
        if (getf(s_probe_va[s_probe_k] + 8) == s_probe_saved + 0.5f)
            putf(s_probe_va[s_probe_k] + 8, s_probe_saved);
        s_probe_active = 0;
    }
}

static void probe_frame(void)
{
    uint32_t f = s_frame + 1;
    int k;
    if (s_probe_at == 0xFFFFFF00u && f % 30 == 0 &&
        GetFileAttributesA(getenv("RECOMP_HALO2_INTERP_PROBE_FLAG")) != INVALID_FILE_ATTRIBUTES) {
        s_probe_at = f + 2;
        fprintf(stderr, "[INTERP-PROBE] start at frame %u\n", s_probe_at);
    }
    if (f >= s_probe_at && f < s_probe_at + (uint32_t)s_probe_n * 120) {
        s_probe_k = (int)((f - s_probe_at) / 120);
        s_probe_saved = getf(s_probe_va[s_probe_k] + 8);
        putf(s_probe_va[s_probe_k] + 8, s_probe_saved + 0.5f);
        s_probe_active = 1;
    }
    if (s_probe_at != 0xFFFFFF00u && f == s_probe_at + (uint32_t)s_probe_n * 120)
        for (k = 0; k < s_probe_n; k++)
            fprintf(stderr, "[INTERP-PROBE] %08X: drawn camera rose in %u of %u frames\n",
                    s_probe_va[k], s_probe_hits[k], s_probe_tries[k]);
}

/* ---- the blend */

static float *guest_floats(uint32_t va) { return (float *)XBOX_PTR(va); }

static void obj_forget(Obj *o)
{
    free(o->buf);
    free(o->node_seen);
    o->buf = o->prev = o->cur = o->shown = NULL;
    o->node_seen = NULL;
    o->node_run = NULL;
    o->node_kept = NULL;
    o->handle = 0;
    o->moved = 0;
}

/* Copy the object's position and nodes into dst (n floats). */
static void obj_read(const Obj *o, float *dst)
{
    memcpy(dst, guest_floats(o->data + OBJECT_POSITION), 12);
    memcpy(dst + 3, guest_floats(o->data + o->nodes_off), (o->n - 3) * 4u);
}

static float dist2(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static void normalize3(float *v)
{
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 1e-12f) {
        l = 1.0f / sqrtf(l);
        v[0] *= l; v[1] *= l; v[2] *= l;
    }
}

/* Only real objects get written: during a level load the table is torn down and
 * rebuilt, and an entry can point at memory that is no longer an object. The
 * position must be finite and every node a matrix (scale near 1, unit axes)
 * near the first node. Not near the object's position: a cutscene animation
 * carries the whole body far from it (measured 2026-11-24: every cutscene
 * character failed a 20-unit check and was never blended). */
static int looks_like_object(uint32_t data, uint32_t off, uint32_t nodes)
{
    const float *pos = guest_floats(data + OBJECT_POSITION), *m = guest_floats(data + off), *root = m + 10;
    uint32_t j, a;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]) ||
        fabsf(pos[0]) > 1e5f || fabsf(pos[1]) > 1e5f || fabsf(pos[2]) > 1e5f)
        return s_dbg_rej[0]++, 0;
    if (nodes && (!isfinite(root[0]) || !isfinite(root[1]) || !isfinite(root[2]) ||
                  fabsf(root[0]) > 1e5f || fabsf(root[1]) > 1e5f || fabsf(root[2]) > 1e5f))
        return s_dbg_rej[0]++, 0;
    for (j = 0; j < nodes; j++, m += NODE_FLOATS) {
        if (!(m[0] > 0.01f && m[0] < 100.0f))
            return s_dbg_rej[1]++, 0;
        for (a = 1; a < 10; a += 3) {
            float l = m[a] * m[a] + m[a + 1] * m[a + 1] + m[a + 2] * m[a + 2];
            if (!(l > 0.9f && l < 1.1f))
                return s_dbg_rej[2]++, 0;
        }
        if (!(dist2(m + 10, root) < 10000.0f))   /* within 100 units of the first node */
            return s_dbg_rej[3]++, 0;
    }
    return 1;
}

static float s_raw[3 + MAX_NODES * NODE_FLOATS];

static void blend_object(uint32_t index, uint32_t handle, uint32_t data, double now)
{
    Obj *o = &s_obj[index];
    uint32_t block = MEM32(data + OBJECT_NODE_BLOCK), size = block & 0xFFFFu, off = block >> 16;
    uint32_t nodes = size / NODE_BYTES, n = 3 + nodes * NODE_FLOATS, k, j;
    float alpha;
    if (size % NODE_BYTES || nodes > MAX_NODES || off < 0x100u || off > 0x4000u ||
        data + off + size > GUEST_HIGH_END || !looks_like_object(data, off, nodes)) {
        if (o->handle) obj_forget(o);
        return;
    }
    if (o->handle != handle || o->data != data || o->nodes_off != off || o->n != n) {
        obj_forget(o);                              /* a new object: no history yet */
        o->buf = o->prev = (float *)malloc(n * 3u * sizeof(float));
        o->node_seen = (uint32_t *)calloc(nodes + 1, sizeof(uint32_t) + 2);
        if (!o->buf || !o->node_seen) {
            obj_forget(o);
            return;
        }
        o->node_run = (uint8_t *)(o->node_seen + nodes + 1);
        o->node_kept = o->node_run + nodes + 1;
        o->cur = o->prev + n;
        o->shown = o->cur + n;
        o->handle = handle;
        o->data = data;
        o->nodes_off = off;
        o->n = n;
        obj_read(o, o->cur);
        memcpy(o->prev, o->cur, n * 4u);
        o->tick_time = now;
        o->step = o->last_step = 0.0f;
        o->node_step = 0.0f;
        o->snapped = 0;
        o->shown_frame = 0;
        s_dbg_new++;
        return;
    }
    s_dbg_objects++;
    obj_read(o, s_raw);
    for (k = 0; k < n; k++)
        if (!isfinite(s_raw[k]))
            return;
    /* A change is a new update. The tick counter is no help: it changes a
     * frame before the bodies move (measured 2026-11-23). Changes only in nodes
     * the game moves every frame are not. */
    if (memcmp(s_raw, o->cur, n * 4u)) {
        int update = memcmp(s_raw, o->cur, 12) != 0;
        for (j = 0; j < nodes; j++) {
            const float *a = s_raw + 3 + j * NODE_FLOATS, *b = o->cur + 3 + j * NODE_FLOATS;
            if (!memcmp(a, b, NODE_BYTES))
                continue;
            o->node_run[j] = o->node_seen[j] == s_frame - 1 ? (uint8_t)(o->node_run[j] < 255 ? o->node_run[j] + 1 : 255) : 0;
            o->node_seen[j] = s_frame;
            if (!NODE_LIVE(o, j))
                update = 1;
        }
        if (getenv("RECOMP_HALO2_INTERP_LIVELOG") && s_frame % 600 == 1) {   /* test: objects with every-frame nodes */
            uint32_t live = 0;
            for (j = 0; j < nodes; j++) live += NODE_LIVE(o, j);
            if (live)
                fprintf(stderr, "[LIVE] frame %u obj %u tag %08X nodes %u live %u update %d run0 %u\n", s_frame, index,
                        MEM32(data), nodes, live, update, o->node_run[0]);
        }
        if (update)
            for (j = 0; j < nodes; j++)
                if (o->node_kept[j]) o->node_kept[j]--;
        if (!update) {
            memcpy(o->cur, s_raw, n * 4u);          /* the game's own every-frame nodes: keep the update's timing */
            s_dbg_live_only++;
        } else {
            float *t = o->prev;
            o->prev = o->cur;
            o->cur = t;
            memcpy(o->cur, s_raw, n * 4u);
            o->tick_time = now;
            s_dbg_changed++;
            /* A teleport (respawn, scripted move, cutscene cut) is a jump over 5 units
             * that is also far bigger than the step before it; a fast mover (a ship,
             * a vehicle) moves that far every update and is blended. */
            o->last_step = o->step;
            o->step = sqrtf(dist2(o->prev, o->cur));
            if (nodes) {
                float d = sqrtf(dist2(o->prev + 3 + 10, o->cur + 3 + 10));
                if (d > o->step) o->step = d;
            }
            o->snapped = o->step > SNAP_DISTANCE && o->step > 3.0f * o->last_step + 1.0f;
            {   /* A pose or place jump at a cutscene cut, under the teleport distance
                 * (2026-11-24, Heretic intro: a Brute's head down for one frame, and a
                 * camera riding a Prophet swinging for one): some node moved far more
                 * than any did the update before. */
                float most = 0.0f, d;
                for (j = 0; j < nodes; j++) {
                    d = dist2(o->prev + 3 + j * NODE_FLOATS + 10, o->cur + 3 + j * NODE_FLOATS + 10);
                    if (d > most) most = d;
                }
                most = sqrtf(most);
                if (nodes >= 4 && most > 0.4f && most > 4.0f * o->node_step + 0.2f) {   /* skeletal objects: a projectile's first move is no cut */
                    o->snapped = 1;
                    if (getenv("RECOMP_HALO2_INTERP_SNAPLOG")) {   /* test: what the bone-jump rule caught */
                        static int logged;
                        if (logged++ < 300)
                            fprintf(stderr, "[SNAP] frame %u obj %u tag %08X nodes %u most %.3f ref %.3f step %.3f last %.3f\n",
                                    s_frame, index, MEM32(data), nodes, most, o->node_step, o->step, o->last_step);
                    }
                }
                /* the largest recent step, fading: an object the game moves every
                 * other update (distant AI) has steps of 0 between its moves */
                o->node_step = most > o->node_step * 0.8f ? most : o->node_step * 0.8f;
            }
            /* An update that comes before the last one was fully shown (the frame and
             * update clocks drift apart when the screen is not 60 Hz) starts from what
             * the last frame showed, not from the last update: no jolt. */
            if (o->shown_frame == s_frame - 1 && !o->snapped)
                memcpy(o->prev, o->shown, n * 4u);
        }
    }
    if (s_lift_on && s_lift_mode == 0)
        return;                                     /* test: flag file "0" turns the blend off */
    if (s_lift_on && (!(s_lift_mode & 8) || (s_frame & 1))) {   /* test: raise what the mode says (8: odd frames) */
        memcpy(o->shown, o->cur, n * 4u);
        if (s_lift_mode & 1)
            for (j = 0; j < nodes; j++)
                o->shown[3 + j * NODE_FLOATS + 12] += s_lift_on;
        if (s_lift_mode & 2)
            o->shown[2] += s_lift_on;
        if (s_lift_mode & 4)
            for (j = 0; j < 3; j++) {
                s_centre_z[index][j] = getf(data + 0x38 + j * 16) + s_lift_on;
                putf(data + 0x38 + j * 16, s_centre_z[index][j]);
            }
    } else {
        if (o->snapped) {
            s_dbg_snap++;
            o->shown_frame = 0;                     /* the next blend starts from the game's pose */
            return;
        }
        alpha = (float)((now - o->tick_time + s_frame_dt) * 30.0);
        if (alpha >= 1.0f)
            return;                                 /* the current state: nothing to change */
        if (getenv("RECOMP_HALO2_INTERP_LIVEWHOLE"))   /* test: an object with game-moved nodes is not blended at all */
            for (j = 0; j < nodes; j++)
                if (NODE_LIVE(o, j))
                    return;
        if (!memcmp(o->prev, o->cur, n * 4u))
            return;                                 /* not moving */
        if (alpha < 0.0f)
            alpha = 0.0f;
        for (k = 0; k < n; k++)
            o->shown[k] = o->prev[k] + (o->cur[k] - o->prev[k]) * alpha;
        for (j = 0; j < nodes; j++) {
            float *m = o->shown + 3 + j * NODE_FLOATS;
            if (NODE_LIVE(o, j)) {                  /* the game's every-frame node: as the game has it */
                memcpy(m, o->cur + 3 + j * NODE_FLOATS, NODE_BYTES);
                s_dbg_everyframe++;
                continue;
            }
            normalize3(m + 1); normalize3(m + 4); normalize3(m + 7);   /* keep the axes unit length */
        }
        /* A node the game moves itself (a talking face) is built from the game's
         * pose of its parent, which the blend draws half a step behind: shown as
         * the game has it, a jaw slipped against its head every other frame
         * (2026-11-24). Carry it with the nearest blended node instead: the
         * blend's move of that node (its drawn matrix against the game's),
         * applied to the game's pose of this one. RECOMP_HALO2_INTERP_LIVECARRY=0
         * shows it as the game has it (test). */
        if (!(getenv("RECOMP_HALO2_INTERP_LIVECARRY") && getenv("RECOMP_HALO2_INTERP_LIVECARRY")[0] == '0'))
            for (j = 0; j < nodes; j++) {
                const float *c = o->cur + 3 + j * NODE_FLOATS, *cp, *sp;
                float *m = o->shown + 3 + j * NODE_FLOATS, best = 1e30f, R[9], t[3];
                uint32_t q, p = nodes;
                int a, b;
                if (!NODE_LIVE(o, j))
                    continue;
                for (q = 0; q < nodes; q++) {
                    float d;
                    if (q == j || NODE_LIVE(o, q))
                        continue;
                    d = dist2(c + 10, o->cur + 3 + q * NODE_FLOATS + 10);
                    if (d < best) { best = d; p = q; }
                }
                if (p == nodes)
                    continue;                       /* no blended node: as the game has it */
                cp = o->cur + 3 + p * NODE_FLOATS;
                sp = o->shown + 3 + p * NODE_FLOATS;
                /* R = Rshown * Rcur^T (axes as columns: forward 1-3, left 4-6, up 7-9) */
                for (a = 0; a < 3; a++)
                    for (b = 0; b < 3; b++)
                        R[a * 3 + b] = sp[1 + a] * cp[1 + b] + sp[4 + a] * cp[4 + b] + sp[7 + a] * cp[7 + b];
                for (a = 0; a < 3; a++)
                    t[a] = sp[10 + a] - (R[a * 3] * cp[10] + R[a * 3 + 1] * cp[11] + R[a * 3 + 2] * cp[12]);
                m[0] = c[0];
                for (b = 0; b < 4; b++) {           /* the three axes, then the position */
                    const float *v = c + 1 + b * 3;
                    for (a = 0; a < 3; a++)
                        m[1 + b * 3 + a] = R[a * 3] * v[0] + R[a * 3 + 1] * v[1] + R[a * 3 + 2] * v[2] + (b == 3 ? t[a] : 0.0f);
                }
                normalize3(m + 1); normalize3(m + 4); normalize3(m + 7);
            }
        if (getenv("RECOMP_HALO2_INTERP_OBJLOG") && nodes >= 20 && s_frame <= s_objlog_until) {   /* test: skeletal objects' blend for 14 frames after a camera cut */
            static int logged;
            if (logged++ < 3000) {
                /* the node whose drawn position moved most since the last frame drawn, and its state */
                static float last[MAX_OBJECTS][MAX_NODES][3];
                uint32_t best = 0;
                float bd = -1.0f;
                for (j = 0; j < nodes; j++) {
                    const float *p = o->shown + 3 + j * NODE_FLOATS + 10;
                    float dd = (p[0] - last[index][j][0]) * (p[0] - last[index][j][0]) + (p[1] - last[index][j][1]) * (p[1] - last[index][j][1]) +
                               (p[2] - last[index][j][2]) * (p[2] - last[index][j][2]);
                    if (dd > bd) { bd = dd; best = j; }
                }
                fprintf(stderr, "[OBJ] frame %u obj %u nodes %u alpha %.2f root %.3f %.3f | most-moved node %u by %.4f live %d run %u kept %u | node prev %.4f cur %.4f shown %.4f\n",
                        s_frame, index, nodes, alpha, o->shown[3 + 10], o->shown[3 + 11], best, sqrtf(bd), NODE_LIVE(o, best),
                        o->node_run[best], o->node_kept[best], o->prev[3 + best * NODE_FLOATS + 12],
                        o->cur[3 + best * NODE_FLOATS + 12], o->shown[3 + best * NODE_FLOATS + 12]);
                for (j = 0; j < nodes; j++)
                    memcpy(last[index][j], o->shown + 3 + j * NODE_FLOATS + 10, 12);
            }
        }
    }
    memcpy(guest_floats(data + OBJECT_POSITION), o->shown, 12);
    memcpy(guest_floats(data + off), o->shown + 3, (n - 3) * 4u);
    o->moved = 1;
    o->shown_frame = s_frame;
    s_dbg_blended++;
}

static int object_still(uint32_t index, const Obj *o)
{
    uint32_t hdr = MEM32(OBJECT_ARRAY_PTR), e;
    if (hdr < 0x80000000u || hdr >= GUEST_HIGH_END || MEM32(hdr + 0x2C) != DATA_ARRAY_SIG)
        return 0;
    e = MEM32(hdr + ARRAY_DATA) + index * 12u;
    return (MEM32(e) & 0xFFFFu) == (o->handle >> 16) && MEM32(e + 8) == o->data;
}

/* Put the game's own state back wherever the frame left the blend in place
 * (a node the game rebuilt during the frame keeps the game's value). */
static void restore_objects(void)
{
    uint32_t i, j;
    for (i = 0; i < MAX_OBJECTS; i++) {
        Obj *o = &s_obj[i];
        float *g;
        if (!o->moved)
            continue;
        o->moved = 0;
        if (!object_still(i, o))
            continue;
        if (s_lift_on && (s_lift_mode & 4))
            for (j = 0; j < 3; j++)
                if (getf(o->data + 0x38 + j * 16) == s_centre_z[i][j])
                    putf(o->data + 0x38 + j * 16, s_centre_z[i][j] - s_lift_on);
        g = guest_floats(o->data + OBJECT_POSITION);
        if (!memcmp(g, o->shown, 12))
            memcpy(g, o->cur, 12);
        else
            s_dbg_kept++;                           /* the game moved it during the frame */
        g = guest_floats(o->data + o->nodes_off);
        for (j = 0; j < (o->n - 3) / NODE_FLOATS; j++) {
            const float *sh = o->shown + 3 + j * NODE_FLOATS;
            if (!memcmp(g + j * NODE_FLOATS, sh, NODE_BYTES))
                memcpy(g + j * NODE_FLOATS, o->cur + 3 + j * NODE_FLOATS, NODE_BYTES);
            else {
                s_dbg_kept++;                       /* rebuilt by the game during the frame (a turret aimed) */
                if (o->node_kept)
                    o->node_kept[j] = 8;            /* left alone for the next 8 updates */
            }
        }
        s_dbg_restored++;
    }
}

/* ---- points: particles (and other arrays of positions)
 *
 * Particles are not objects: the "particles" data array (header pointer at
 * 0x52D714, 64-byte records) holds each particle's world position at +0x1C
 * and its velocity at +0x28, moved on updates (found 2026-11-23 in
 * Metropolis). They are blended like the objects' positions: a change is a
 * new update, the frame shows the in-between point, the game's value goes back
 * after the draw. A new particle has no history and shows where it is. */
typedef struct {
    uint32_t ptr;               /* guest address holding the array header's address */
    uint32_t field;             /* offset of the position (3 floats) in a record */
    uint32_t max;               /* tracked records at most */
    const char *name;
} PointArray;

static const PointArray s_point_arrays[] = {
    { 0x52D714u, 0x1Cu, 1024u, "particles" },
};
#define POINT_ARRAYS (sizeof s_point_arrays / sizeof s_point_arrays[0])

typedef struct {
    uint32_t handle;            /* salt << 16 | index; 0: not tracked */
    uint32_t va;                /* the position's guest address */
    float prev[3], cur[3], shown[3];
    double t;
    uint32_t shown_frame;
    float step;
    int moved;
} PointTrack;

static PointTrack *s_points[POINT_ARRAYS];
static float s_lift_points = -1.0f;   /* test: RECOMP_HALO2_INTERP_LIFT_POINTS */
static unsigned s_dbg_points, s_dbg_points_blended;

static void blend_points(double now)
{
    uint32_t a, i, k;
    if (s_lift_points < 0)
        s_lift_points = getenv("RECOMP_HALO2_INTERP_LIFT_POINTS") ? (float)atof(getenv("RECOMP_HALO2_INTERP_LIFT_POINTS")) : 0.0f;
    for (a = 0; a < POINT_ARRAYS; a++) {
        const PointArray *pa = &s_point_arrays[a];
        uint32_t hdr = MEM32(pa->ptr), max, size, data;
        PointTrack *tr;
        if (hdr < 0x80000000u || hdr >= GUEST_HIGH_END || MEM32(hdr + 0x2C) != DATA_ARRAY_SIG)
            continue;
        max = MEM32(hdr + ARRAY_MAX) & 0xFFFFu;
        size = MEM32(hdr + 0x24);
        data = MEM32(hdr + ARRAY_DATA);
        if (max > pa->max) max = pa->max;
        if (size < pa->field + 12u || size > 0x1000u || data < 0x80000000u || data + max * size > GUEST_HIGH_END)
            continue;
        if (!s_points[a] && !(s_points[a] = (PointTrack *)calloc(pa->max, sizeof(PointTrack))))
            continue;
        tr = s_points[a];
        for (i = 0; i < max; i++) {
            PointTrack *t = &tr[i];
            uint32_t rec = data + i * size, salt = MEM32(rec) & 0xFFFFu, handle = salt << 16 | i;
            const float *g;
            float raw[3], alpha, d;
            if (salt == 0) {                        /* free */
                t->handle = 0;
                continue;
            }
            t->va = rec + pa->field;
            g = guest_floats(t->va);
            memcpy(raw, g, sizeof raw);
            if (!isfinite(raw[0]) || !isfinite(raw[1]) || !isfinite(raw[2]))
                continue;
            if (t->handle != handle) {              /* a new particle: no history */
                t->handle = handle;
                memcpy(t->prev, raw, sizeof raw);
                memcpy(t->cur, raw, sizeof raw);
                t->t = now;
                t->step = 0;
                t->shown_frame = 0;
                continue;
            }
            s_dbg_points++;
            if (memcmp(raw, t->cur, sizeof raw)) {  /* moved: a new update */
                float last = t->step;
                memcpy(t->prev, t->shown_frame == s_frame - 1 ? t->shown : t->cur, sizeof raw);
                memcpy(t->cur, raw, sizeof raw);
                t->t = now;
                t->step = sqrtf(dist2(t->prev, t->cur));
                if (t->step > SNAP_DISTANCE && t->step > 3.0f * last + 1.0f)
                    memcpy(t->prev, t->cur, sizeof raw);    /* a jump: show it as it is */
            }
            if (s_lift_on && s_lift_mode == 0)
                continue;                           /* test: flag file "0" turns the blend off */
            if ((s_lift_on && (s_lift_mode & 1) && (!(s_lift_mode & 8) || (s_frame & 1))) ||
                (s_lift_points > 0 && (s_frame & 1))) {   /* RECOMP_HALO2_INTERP_LIFT_POINTS=<units>: odd frames */
                memcpy(t->shown, t->cur, sizeof raw);   /* test: raise them */
                t->shown[2] += s_lift_points > 0 ? s_lift_points : s_lift_on;
            } else {
                alpha = (float)((now - t->t + s_frame_dt) * 30.0);
                if (alpha >= 1.0f || !memcmp(t->prev, t->cur, sizeof raw))
                    continue;
                if (alpha < 0.0f) alpha = 0.0f;
                for (k = 0; k < 3; k++) {
                    d = t->cur[k] - t->prev[k];
                    t->shown[k] = t->prev[k] + d * alpha;
                }
            }
            memcpy(guest_floats(t->va), t->shown, sizeof raw);
            t->moved = 1;
            t->shown_frame = s_frame;
            s_dbg_points_blended++;
        }
    }
}

static void restore_points(void)
{
    uint32_t a, i;
    for (a = 0; a < POINT_ARRAYS; a++) {
        PointTrack *tr = s_points[a];
        if (!tr)
            continue;
        for (i = 0; i < s_point_arrays[a].max; i++) {
            PointTrack *t = &tr[i];
            if (!t->moved)
                continue;
            t->moved = 0;
            if (!memcmp(guest_floats(t->va), t->shown, sizeof t->shown))
                memcpy(guest_floats(t->va), t->cur, sizeof t->cur);
        }
    }
}

/* After the due updates (sub_0013AA50), before the per-frame update places the cameras. */
static double s_dbg_cost;       /* host seconds spent in the blend and restore, this 600 frames */

static void world_updated(void);

void h2_interp_world_updated(void)
{
    double t0;
    if (!interp_enabled())
        return;
    t0 = now_seconds();
    world_updated();
    s_dbg_cost += now_seconds() - t0;
}

/* ---- test: find node matrices outside the object table (RECOMP_HALO2_INTERP_SCAN=<flag file>)
 *
 * When the flag file appears, guest memory is copied at the update hook on 5
 * frames in a row; then every run of matrices (scale 1, unit axes) that is not
 * an object's node block is listed with how each of its 4 frame-to-frame
 * steps changed it ("1010": every other frame, the 30 Hz pattern). */
#define SCAN_FRAMES 5
typedef struct { uint32_t va, size; uint8_t *copy; uint8_t *changed[SCAN_FRAMES - 1]; } ScanRegion;
static ScanRegion s_scan[64];
static int s_scan_n, s_scan_step = -1;

static void scan_regions(void)
{
    static const uint32_t ranges[2][2] = { { 0x00010000u, 0x04000000u }, { 0x80000000u, GUEST_HIGH_END } };
    int r;
    s_scan_n = 0;
    for (r = 0; r < 2; r++) {
        uint32_t va = ranges[r][0];
        while (va < ranges[r][1] && s_scan_n < 64) {
            MEMORY_BASIC_INFORMATION mi;
            uint32_t len;
            if (!VirtualQuery(XBOX_PTR(va), &mi, sizeof mi))
                break;
            len = (uint32_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - (uint8_t *)XBOX_PTR(va));
            if (va + len > ranges[r][1] || va + len < va)
                len = ranges[r][1] - va;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && len >= 4096) {
                s_scan[s_scan_n].va = va;
                s_scan[s_scan_n].size = len;
                s_scan_n++;
            }
            va += len;
        }
    }
}

static int node_like(const float *m)
{
    int a;
    if (!(m[0] > 0.9f && m[0] < 1.1f))
        return 0;
    for (a = 1; a < 10; a += 3) {
        float l = m[a] * m[a] + m[a + 1] * m[a + 1] + m[a + 2] * m[a + 2];
        if (!(l > 0.98f && l < 1.02f))
            return 0;
    }
    return isfinite(m[10]) && isfinite(m[11]) && isfinite(m[12]) &&
           fabsf(m[10]) < 1e5f && fabsf(m[11]) < 1e5f && fabsf(m[12]) < 1e5f;
}

static int in_object_nodes(uint32_t va)
{
    uint32_t i;
    for (i = 0; i < MAX_OBJECTS; i++) {
        const Obj *o = &s_obj[i];
        if (o->handle && va >= o->data && va < o->data + o->nodes_off + (o->n - 3) * 4u)
            return 1;
    }
    return 0;
}

static void scan_report(void)
{
    int r, k;
    unsigned listed = 0;
    fprintf(stderr, "[SCAN] %d regions\n", s_scan_n);
    for (r = 0; r < s_scan_n; r++) {
        ScanRegion *g = &s_scan[r];
        uint32_t off = 0;
        while (off + NODE_BYTES <= g->size) {
            uint32_t va = g->va + off, run = 0, stride = 0, o2;
            char pat[SCAN_FRAMES];
            if (!node_like((const float *)XBOX_PTR(va))) { off += 4; continue; }
            /* a run: matrices at a fixed stride (52 or 64) */
            {
                static const uint32_t strides[2] = { NODE_BYTES, 64 };
                int si;
                for (si = 0; si < 2 && run < 2; si++) {
                    run = 0;
                    for (o2 = off; o2 + NODE_BYTES <= g->size && node_like((const float *)XBOX_PTR(g->va + o2)); o2 += strides[si])
                        run++;
                    stride = strides[si];
                }
            }
            if (run < 3 || in_object_nodes(va)) { off += run > 1 ? run * stride : 4; continue; }
            for (k = 0; k < SCAN_FRAMES - 1; k++) {   /* any dword of the run changed in step k */
                uint32_t b, any = 0;
                for (b = off / 4; b < (off + run * stride) / 4 && !any; b++)
                    any = g->changed[k][b >> 3] & (1u << (b & 7));
                pat[k] = any ? '1' : '0';
            }
            pat[SCAN_FRAMES - 1] = 0;
            if (strcmp(pat, "0000") && listed < 300) {
                const float *m = (const float *)XBOX_PTR(va);
                fprintf(stderr, "[SCAN] %08X run %u stride %u changes %s first at %.2f %.2f %.2f\n", va, run, stride, pat, m[10], m[11], m[12]);
                listed++;
            }
            off += run * stride;
        }
    }
    fprintf(stderr, "[SCAN] done: %u runs listed\n", listed);
    for (r = 0; r < s_scan_n; r++) {
        free(s_scan[r].copy);
        for (k = 0; k < SCAN_FRAMES - 1; k++) free(s_scan[r].changed[k]);
        memset(&s_scan[r], 0, sizeof s_scan[r]);
    }
}

/* Each update hook: start on the flag file, copy, compare, report after the last frame. */
static void scan_frame(void)
{
    static const char *flag = (const char *)-1;
    int r;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_HALO2_INTERP_SCAN");
    if (!flag)
        return;
    if (s_scan_step < 0) {
        if (s_frame % 15 || GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES)
            return;
        DeleteFileA(flag);
        scan_regions();
        for (r = 0; r < s_scan_n; r++) {
            int k;
            s_scan[r].copy = (uint8_t *)malloc(s_scan[r].size);
            for (k = 0; k < SCAN_FRAMES - 1; k++)
                s_scan[r].changed[k] = (uint8_t *)calloc(s_scan[r].size / 32 + 1, 1);
            if (s_scan[r].copy)
                memcpy(s_scan[r].copy, XBOX_PTR(s_scan[r].va), s_scan[r].size);
        }
        s_scan_step = 0;
        fprintf(stderr, "[SCAN] started at frame %u\n", s_frame);
        return;
    }
    for (r = 0; r < s_scan_n; r++) {
        ScanRegion *g = &s_scan[r];
        const uint32_t *now = (const uint32_t *)XBOX_PTR(g->va), *was = (const uint32_t *)g->copy;
        uint32_t b;
        if (!g->copy || !g->changed[s_scan_step])
            continue;
        for (b = 0; b < g->size / 4; b++)
            if (now[b] != was[b])
                g->changed[s_scan_step][b >> 3] |= (uint8_t)(1u << (b & 7));
        memcpy(g->copy, now, g->size);
    }
    if (++s_scan_step == SCAN_FRAMES - 1) {
        scan_report();
        s_scan_step = -1;
    }
}

/* test: RECOMP_HALO2_INTERP_TRACE=<flag file>: when it appears, print for 40
 * frames, at the update hook (H) and just before the draw (D), the position of
 * node 0 and node 4 of the two first-person matrix runs, and the render camera. */
static int s_trace_left;
static void trace_point(char where)
{
    static const char *flag = (const char *)-1;
    static const uint32_t va[3] = { 0x004CE404u, 0x004CF110u, RENDER_CAMERA };
    float p[3][3];
    int i;
    if (flag == (const char *)-1)
        flag = getenv("RECOMP_HALO2_INTERP_TRACE");
    if (!flag)
        return;
    static uint32_t armed_at;
    if (!node_like((const float *)XBOX_PTR(0x004CE404u)))
        armed_at = 0;
    else if (!armed_at)
        armed_at = s_frame;                     /* the arms exist: wait 30 s of play before tracing */
    if (!s_trace_left && where == 'H' && s_frame % 15 == 0 && armed_at &&
        GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(flag);
        s_trace_left = 80;
    }
    if (!s_trace_left)
        return;
    s_trace_left--;
    for (i = 0; i < 3; i++) {
        uint32_t a = i < 2 ? va[i] + 10 * 4 : va[i];
        p[i][0] = getf(a); p[i][1] = getf(a + 4); p[i][2] = getf(a + 8);
    }
    fprintf(stderr, "[TRACE] %c frame %u tick %u arms %.4f %.4f %.4f weapon %.4f %.4f %.4f cam %.4f %.4f %.4f arms4 %.4f %.4f %.4f\n",
            where, s_frame, MEM32(GAME_TICK_COUNTER), p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2],
            p[2][0], p[2][1], p[2][2], getf(0x004CE404u + 4 * 52 + 40), getf(0x004CE404u + 4 * 52 + 44), getf(0x004CE404u + 4 * 52 + 48));
}

static void world_updated(void)
{
    uint32_t hdr, entries, max, i;
    double now = now_seconds();
    if (s_last_frame_time > 0 && now - s_last_frame_time > 0 && now - s_last_frame_time < 0.1)
        s_frame_dt += (now - s_last_frame_time - s_frame_dt) * 0.05;
    s_last_frame_time = now;
    scan_frame();
    trace_point('H');
    if (s_probe < 0)
        probe_init();
    restore_objects();          /* in case a frame was not drawn */
    restore_points();
    probe_restore();
    if (s_probe > 0) {          /* no blending while probing */
        if (!getenv("RECOMP_HALO2_INTERP_PROBE_LATE"))
            probe_frame();
        return;
    }
    hdr = MEM32(OBJECT_ARRAY_PTR);
    if (hdr < 0x80000000u || hdr >= GUEST_HIGH_END || MEM32(hdr + 0x2C) != DATA_ARRAY_SIG)
        return;
    max = MEM32(hdr + ARRAY_MAX) & 0xFFFFu;
    entries = MEM32(hdr + ARRAY_DATA);
    if (max > MAX_OBJECTS)
        max = MAX_OBJECTS;
    now = now_seconds();
    if (s_lift && s_frame % 15 == 0) {   /* RECOMP_HALO2_INTERP_LIFT_FLAG=<file>: lift only while it exists */
        const char *f = getenv("RECOMP_HALO2_INTERP_LIFT_FLAG");
        FILE *fp = f ? fopen(f, "r") : NULL;
        int c = fp ? fgetc(fp) : EOF;
        if (fp) fclose(fp);
        if (c >= '0' && c <= '9') s_lift_mode = c - '0';
        s_lift_on = !f || fp ? s_lift : 0.0f;
    }
    for (i = 0; i < max; i++) {
        uint32_t e = entries + i * 12u, salt = MEM32(e) & 0xFFFFu, data = MEM32(e + 8);
        if (salt == 0 || data < 0x80000000u || data >= GUEST_HIGH_END) {      /* free slot */
            if (s_obj[i].handle) obj_forget(&s_obj[i]);
            continue;
        }
        blend_object(i, salt << 16 | i, data, now);
    }
    blend_points(now);
}

/* ---- the camera, for cutscenes
 *
 * The main view is rendered by sub_0002B9F0 from a camera it is handed
 * (position, forward, up, ...): it copies it into the render camera 0x4C65C0
 * and builds the view matrices from that and from a second copy. Moving only
 * the render camera moved the objects but not the level, so the hook at its
 * entry (patch 13) blends the camera it is handed. In play the camera follows the blended body and already moves every
 * frame. A cutscene camera does not: it steps with the updates (it changes
 * on one frame and holds on the next). So the copied camera is blended only
 * while it steps, and never when it sits at a player's eyes (the
 * first-person gun is placed from the body, not from this camera). */
#define PLAYERS_ARRAY_PTR 0x4F54F4u   /* -> the players data array's header */
#define PLAYER_SIZE       0x21Cu
#define PLAYER_UNIT       0x2Cu       /* the body's object handle */
#define CAM_FLOATS        10          /* position, forward, up, field of view */
#define MAX_VIEWS         4
#define CAMERA_CUT_DISTANCE 20.0f

/* One part of a camera (its position, or its direction and the rest), tracked on
 * its own: a cutscene can turn the camera every frame while its position steps
 * with the updates, or the other way round. */
typedef struct {
    float prev[CAM_FLOATS], cur[CAM_FLOATS];
    double t;                   /* host seconds when cur was first seen */
    float shown[CAM_FLOATS];    /* what the last blend showed of this part */
    uint32_t shown_frame;
    uint32_t changed_frame;     /* the last frame this part changed */
    int stepping;               /* the last change came after a frame of holding */
    int valid;
    float step, last_step;      /* largest recent change (fading), and the last one (units; for the direction 1 - cos) */
    int cut;                    /* the last change was a cut: show it as it is */
} CamTrack;

typedef struct {
    CamTrack pos, rot;          /* floats 0-2; floats 3-9 */
} CamState;

static CamState s_cam[MAX_VIEWS];
static uint32_t s_cam_frame;    /* frame of the copies counted in s_cam_index */
static int s_cam_index;
static int s_cam_moved;
static float s_cam_raw[CAM_FLOATS], s_cam_shown[CAM_FLOATS];
static unsigned s_dbg_cam_blends, s_dbg_cam_fp, s_dbg_cam_cuts;

/* Is this camera at a local player's eyes (within 1.5 units of a body)? */
static int camera_at_a_body(const float *pos)
{
    uint32_t hdr = MEM32(PLAYERS_ARRAY_PTR), max, data, objs, i;
    if (hdr < 0x80000000u || hdr >= GUEST_HIGH_END || MEM32(hdr + 0x2C) != DATA_ARRAY_SIG)
        return 0;
    objs = MEM32(OBJECT_ARRAY_PTR);
    if (objs < 0x80000000u || objs >= GUEST_HIGH_END || MEM32(objs + 0x2C) != DATA_ARRAY_SIG)
        return 0;
    max = MEM32(hdr + ARRAY_MAX) & 0xFFFFu;
    data = MEM32(hdr + ARRAY_DATA);
    for (i = 0; i < max && i < 16; i++) {
        uint32_t p = data + i * PLAYER_SIZE, unit, e, obj;
        if ((MEM32(p) & 0xFFFFu) == 0)
            continue;
        unit = MEM32(p + PLAYER_UNIT);
        if (unit == 0xFFFFFFFFu || (unit & 0xFFFFu) >= (MEM32(objs + ARRAY_MAX) & 0xFFFFu))
            continue;
        e = MEM32(objs + ARRAY_DATA) + (unit & 0xFFFFu) * 12u;
        if ((MEM32(e) & 0xFFFFu) != (unit >> 16))
            continue;
        obj = MEM32(e + 8);
        if (obj < 0x80000000u || obj >= GUEST_HIGH_END)
            continue;
        if (dist2(guest_floats(obj + OBJECT_POSITION), pos) < 2.25f)
            return 1;
    }
    return 0;
}

static void camera_probe_measure(float z)
{
    if (s_probe_active) {
        s_probe_tries[s_probe_k]++;
        if (z > s_probe_base_z + 0.4f && z < s_probe_base_z + 0.6f)
            s_probe_hits[s_probe_k]++;
    } else
        s_probe_base_z = z;
}

/* After a render pass copied a view into the render camera (patch 12): the probe's measurement. */
void h2_interp_camera_copied(void)
{
    float z;
    if (s_on <= 0 || s_probe <= 0)
        return;
    z = getf(RENDER_CAMERA + 8);
    if (getf(RENDER_CAMERA) == 0 && getf(RENDER_CAMERA + 4) == 0 && z == 0)
        return;                                     /* a 2D pass */
    camera_probe_measure(z);
}

static uint32_t s_cam_va, s_cam_va2;   /* where the blended camera was written (a second copy: va2) */

/* Track one part (n floats from first) of the camera; returns the blend factor for
 * it, or -1 to leave it as it is. */
static float track_part(CamTrack *t, const float *raw, int n, double now)
{
    float alpha;
    if (!t->valid) {
        memcpy(t->prev, raw, n * sizeof(float));
        memcpy(t->cur, raw, n * sizeof(float));
        t->t = now;
        t->changed_frame = s_frame;
        t->valid = 1;
        return -1.0f;
    }
    if (memcmp(raw, t->cur, n * sizeof(float))) {
        uint32_t held = s_frame - t->changed_frame;
        float step;
        if (n == 3)
            step = sqrtf(dist2(raw, t->cur));
        else
            step = 1.0f - (raw[0] * t->cur[0] + raw[1] * t->cur[1] + raw[2] * t->cur[2]);
        /* A cut between two cutscene shots (seen 2026-11-24 in the Arbiter intro:
         * a held shot, then half a unit and a big turn in one update, and the frame
         * between showed a view from neither shot): a change after the camera held
         * still for more than two updates, or far bigger than its last step. A
         * real move only loses the blend of its first step. */
        if (held > 4)
            t->step = 0.0f;                         /* held still: no recent steps */
        t->cut = s_cam_cut_rule && (held > 4 || step > 4.0f * t->step + (n == 3 ? 0.02f : 0.0005f));
        if (getenv("RECOMP_HALO2_INTERP_CAMLOG")) {   /* test: every camera change */
            static int logged;
            if (logged++ < 40000)
                fprintf(stderr, "[CAM] frame %u view %d %s held %u step %.5f ref %.5f cut %d\n", s_frame, s_cam_index - 1,
                        n == 3 ? "pos" : "rot", held, step, t->step, t->cut);
        }
        t->last_step = step;
        t->step = step > t->step * 0.8f ? step : t->step * 0.8f;   /* the largest recent step, fading */
        t->stepping = held >= 2;                    /* held for a frame before this change */
        t->changed_frame = s_frame;
        /* from what the last frame showed, if it was still on its way */
        memcpy(t->prev, t->shown_frame == s_frame - 1 ? t->shown : t->cur, n * sizeof(float));
        memcpy(t->cur, raw, n * sizeof(float));
        t->t = now;
    } else if (s_frame - t->changed_frame > 3)
        t->stepping = 0;                            /* holding still: nothing to blend */
    if (!t->stepping || (t->cut && s_frame - t->changed_frame < 2))
        return -1.0f;
    alpha = (float)((now - t->t + s_frame_dt) * 30.0);
    if (alpha >= 1.0f)
        return -1.0f;
    return alpha < 0.0f ? 0.0f : alpha;
}

/* Blend the camera (CAM_FLOATS floats) at guest address va where it steps. */
static void blend_camera_at(uint32_t va)
{
    float raw[CAM_FLOATS], a_pos, a_rot, d;
    CamState *c;
    double now;
    int k;
    memcpy(raw, guest_floats(va), sizeof raw);
    for (k = 0; k < CAM_FLOATS; k++)
        if (!isfinite(raw[k]))
            return;
    if (raw[0] == 0 && raw[1] == 0 && raw[2] == 0)
        return;                                     /* a 2D pass */
    if (s_lift_on && s_lift_mode == 0)
        return;                                     /* test: flag file "0" turns the blend off */
    if (s_cam_frame != s_frame) {                   /* the first view this frame */
        s_cam_frame = s_frame;
        s_cam_index = 0;
    }
    if (s_cam_index >= MAX_VIEWS)
        return;
    c = &s_cam[s_cam_index++];
    now = now_seconds();
    a_pos = track_part(&c->pos, raw, 3, now);
    a_rot = track_part(&c->rot, raw + 3, CAM_FLOATS - 3, now);
    /* A cut: over 20 units or about 45 degrees in one update (fly-through
     * shots move over 5 units an update, so the objects' limit is too tight). */
    if (a_pos >= 0 && dist2(c->pos.prev, c->pos.cur) > CAMERA_CUT_DISTANCE * CAMERA_CUT_DISTANCE)
        a_pos = -1.0f;
    if (a_rot >= 0 && c->rot.prev[0] * c->rot.cur[0] + c->rot.prev[1] * c->rot.cur[1] +
                      c->rot.prev[2] * c->rot.cur[2] < 0.7f)
        a_rot = -1.0f;
    /* A cut in either part is a cut of the camera, until the next update: a
     * part that holds still after its cut must not keep the other unblended. */
    if ((c->pos.cut && s_frame - c->pos.changed_frame < 2) || (c->rot.cut && s_frame - c->rot.changed_frame < 2)) {
        if (c->pos.changed_frame == s_frame || c->rot.changed_frame == s_frame) {
            s_dbg_cam_cuts++;
            s_objlog_until = s_frame + 14;
            if (getenv("RECOMP_HALO2_INTERP_OBJLOG"))
                fprintf(stderr, "[OBJ] camera cut at frame %u\n", s_frame);
            /* The objects moved for the new shot too, some by less than a
             * teleport: drawn between their old and new places, a character
             * filled the screen for a frame (2026-11-24). Draw the cut as the
             * game has it. */
            restore_objects();
            restore_points();
            {   /* and forget what was drawn: the next blend would start from the
                 * old shot's poses (each in-between frame of the next four updates
                 * showed some of it, 2026-11-25) */
                uint32_t i;
                for (i = 0; i < MAX_OBJECTS; i++)
                    s_obj[i].shown_frame = 0;
            }
        }
        return;
    }
    if (a_pos < 0 && a_rot < 0)
        return;
    if (camera_at_a_body(raw)) {
        s_dbg_cam_fp++;
        return;
    }
    memcpy(s_cam_shown, raw, sizeof raw);
    if (a_pos >= 0)
        for (k = 0; k < 3; k++)
            s_cam_shown[k] = c->pos.prev[k] + (c->pos.cur[k] - c->pos.prev[k]) * a_pos;
    if (a_rot >= 0) {
        for (k = 3; k < CAM_FLOATS; k++)
            s_cam_shown[k] = c->rot.prev[k - 3] + (c->rot.cur[k - 3] - c->rot.prev[k - 3]) * a_rot;
        normalize3(s_cam_shown + 3);                /* forward */
        d = s_cam_shown[6] * s_cam_shown[3] + s_cam_shown[7] * s_cam_shown[4] + s_cam_shown[8] * s_cam_shown[5];
        for (k = 0; k < 3; k++)                     /* up, square to forward */
            s_cam_shown[6 + k] -= d * s_cam_shown[3 + k];
        normalize3(s_cam_shown + 6);
    }
    if (a_pos >= 0) {
        memcpy(c->pos.shown, s_cam_shown, 3 * sizeof(float));
        c->pos.shown_frame = s_frame;
    }
    if (a_rot >= 0) {
        memcpy(c->rot.shown, s_cam_shown + 3, (CAM_FLOATS - 3) * sizeof(float));
        c->rot.shown_frame = s_frame;
    }
    memcpy(s_cam_raw, raw, sizeof raw);
    memcpy(guest_floats(va), s_cam_shown, sizeof s_cam_shown);
    s_cam_va = va;
    s_cam_moved = 1;
    s_dbg_cam_blends++;
}

/* Entry of the main view's renderer sub_0002B9F0 (patch 13): it copies its camera from eax into
 * the render camera and builds the view matrices from there, and a second matrix from its second
 * argument (eax when that is 0). Blending the source makes every copy agree. */
void h2_interp_view_render(uint32_t camera, uint32_t camera2)
{
    if (s_on <= 0 || s_probe > 0 || camera < 0x10000u || camera >= GUEST_HIGH_END)
        return;
    if (camera2 && camera2 != camera && camera2 >= 0x10000u && camera2 < GUEST_HIGH_END &&
        !memcmp(guest_floats(camera2), guest_floats(camera), CAM_FLOATS * 4)) {
        blend_camera_at(camera);
        if (s_cam_moved && s_cam_va == camera) {
            memcpy(guest_floats(camera2), s_cam_shown, sizeof s_cam_shown);
            s_cam_va2 = camera2;
        }
    } else
        blend_camera_at(camera);
}

static void restore_camera(void)
{
    if (!s_cam_moved)
        return;
    s_cam_moved = 0;
    if (!memcmp(guest_floats(s_cam_va), s_cam_shown, sizeof s_cam_shown))
        memcpy(guest_floats(s_cam_va), s_cam_raw, sizeof s_cam_raw);
    if (s_cam_va2 && !memcmp(guest_floats(s_cam_va2), s_cam_shown, sizeof s_cam_shown))
        memcpy(guest_floats(s_cam_va2), s_cam_raw, sizeof s_cam_raw);
    s_cam_va2 = 0;
}

/* ---- the first-person model
 *
 * Inside the main view, sub_0003D2B0 has sub_00168DED build the local player's
 * first-person node sets (arms, weapon: up to 4 sets of a 12-byte header and 64
 * nodes at 0x4CE3F8) from the view's camera, every frame (patch 14). Their
 * animation steps at 30 Hz (measured 2026-11-24: an idle energy sword changed
 * on every other frame) while the camera moves every frame. In the camera's own
 * space only the animation is left: blend that between its last two steps, put
 * it back on this frame's camera, and give the game its own values back after
 * the draw (a weapon's markers may be read by the next update). */
#define FP_SETS        4
#define FP_SET_BYTES   3340u        /* 12-byte header + 64 nodes */
#define FP_MAX_NODES   64
#define FP_REL         12           /* forward, left, up, position in the camera's basis */

typedef struct {
    int n;                          /* nodes tracked; 0: none */
    float prev[FP_MAX_NODES * FP_REL], cur[FP_MAX_NODES * FP_REL], shown[FP_MAX_NODES * FP_REL];
    double t;
    uint32_t changed_frame, shown_frame;
    int stepping, cut;
    float ref;                      /* largest recent step (fading) */
    int written;                    /* the set holds blended nodes this frame */
    float orig[FP_MAX_NODES * NODE_FLOATS];
} FpTrack;
static FpTrack s_fp[FP_SETS];
static uint32_t s_fp_frame = 0xFFFFFFFFu, s_fp_sets;
static unsigned s_dbg_fp, s_dbg_fp_cuts;

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void h2_interp_first_person(uint32_t sets, uint32_t count)
{
    float cam[9], f[3], l[3], u[3], d;
    double now;
    uint32_t si, j;
    int k;
    if (s_on <= 0 || s_probe > 0 || !s_fp_rule || s_fp_frame == s_frame)
        return;                                     /* once a frame: the main view */
    s_fp_frame = s_frame;
    s_fp_sets = 0;
    memcpy(cam, guest_floats(RENDER_CAMERA), sizeof cam);
    for (k = 0; k < 9; k++)
        if (!isfinite(cam[k]))
            return;
    memcpy(f, cam + 3, sizeof f); normalize3(f);
    memcpy(u, cam + 6, sizeof u);
    d = dot3(u, f);
    for (k = 0; k < 3; k++) u[k] -= d * f[k];
    normalize3(u);
    l[0] = u[1] * f[2] - u[2] * f[1]; l[1] = u[2] * f[0] - u[0] * f[2]; l[2] = u[0] * f[1] - u[1] * f[0];
    if (dot3(f, f) < 0.5f || dot3(u, u) < 0.5f)
        return;
    now = now_seconds();
    if (count > FP_SETS)
        count = FP_SETS;
    for (si = 0; si < count; si++) {
        FpTrack *t = &s_fp[si];
        uint32_t base = sets + si * FP_SET_BYTES + 12, n = 0;
        float rel[FP_MAX_NODES * FP_REL], most = 0.0f, alpha;
        int changed = 0;
        while (n < FP_MAX_NODES) {                  /* the set's nodes: matrices, scale near 1 */
            const float *m = guest_floats(base + n * NODE_BYTES);
            if (!(m[0] > 0.5f && m[0] < 2.0f) || !isfinite(m[10]) || !isfinite(m[11]) || !isfinite(m[12]) ||
                fabsf(dot3(m + 1, m + 1) - 1.0f) > 0.05f || fabsf(dot3(m + 10, m + 10)) > 1e10f)
                break;
            n++;
        }
        if (!n) {
            t->n = 0;
            continue;
        }
        for (j = 0; j < n; j++) {                   /* into the camera's space */
            const float *m = guest_floats(base + j * NODE_BYTES);
            float *r = rel + j * FP_REL, pos[3];
            for (k = 0; k < 3; k++) {
                r[k * 3 + 0] = dot3(m + 1 + k * 3, f);
                r[k * 3 + 1] = dot3(m + 1 + k * 3, l);
                r[k * 3 + 2] = dot3(m + 1 + k * 3, u);
            }
            pos[0] = m[10] - cam[0]; pos[1] = m[11] - cam[1]; pos[2] = m[12] - cam[2];
            r[9] = dot3(pos, f); r[10] = dot3(pos, l); r[11] = dot3(pos, u);
        }
        if (t->n != (int)n) {                       /* a new set (a weapon switch): no history */
            t->n = (int)n;
            memcpy(t->cur, rel, n * FP_REL * sizeof(float));
            memcpy(t->prev, rel, n * FP_REL * sizeof(float));
            t->changed_frame = s_frame;
            t->stepping = 0;
            t->cut = 1;
            t->ref = 0.0f;
            continue;
        }
        for (j = 0; j < n * FP_REL; j++)            /* a change, not the float noise of a moving camera */
            if (fabsf(rel[j] - t->cur[j]) > 2e-4f) { changed = 1; break; }
        if (changed) {
            uint32_t held = s_frame - t->changed_frame;
            for (j = 0; j < n; j++) {
                const float *a = rel + j * FP_REL + 9, *b = t->cur + j * FP_REL + 9;
                float dd = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
                if (dd > most) most = dd;
            }
            most = sqrtf(most);
            if (held > 4)
                t->ref = 0.0f;
            /* a jump (a new animation, a melee, a reload start) is shown as it is */
            t->cut = most > 0.05f && most > 4.0f * t->ref + 0.02f;
            if (t->cut) s_dbg_fp_cuts++;
            t->ref = most > t->ref * 0.8f ? most : t->ref * 0.8f;
            t->stepping = held >= 2;
            memcpy(t->prev, t->shown_frame == s_frame - 1 ? t->shown : t->cur, n * FP_REL * sizeof(float));
            memcpy(t->cur, rel, n * FP_REL * sizeof(float));
            t->changed_frame = s_frame;
            t->t = now;
        } else if (s_frame - t->changed_frame > 3)
            t->stepping = 0;
        if (!t->stepping || t->cut)
            continue;
        alpha = (float)((now - t->t + s_frame_dt) * 30.0);
        if (alpha >= 1.0f)
            continue;
        if (alpha < 0.0f)
            alpha = 0.0f;
        for (j = 0; j < n * FP_REL; j++)
            t->shown[j] = t->prev[j] + (t->cur[j] - t->prev[j]) * alpha;
        t->shown_frame = s_frame;
        memcpy(t->orig, guest_floats(base), n * NODE_BYTES);
        for (j = 0; j < n; j++) {                   /* back on this frame's camera */
            float *m = guest_floats(base + j * NODE_BYTES), *r = t->shown + j * FP_REL;
            for (k = 0; k < 4; k++) {
                float *dst = k < 3 ? m + 1 + k * 3 : m + 10, *src = r + k * 3;
                float x = src[0] * f[0] + src[1] * l[0] + src[2] * u[0];
                float y = src[0] * f[1] + src[1] * l[1] + src[2] * u[1];
                float z = src[0] * f[2] + src[1] * l[2] + src[2] * u[2];
                dst[0] = x; dst[1] = y; dst[2] = z;
                if (k < 3)
                    normalize3(dst);
                else {
                    dst[0] += cam[0]; dst[1] += cam[1]; dst[2] += cam[2];
                }
            }
        }
        t->written = 1;
        s_fp_sets |= 1u << si;
        s_dbg_fp++;
    }
}

/* The game's own first-person nodes back after the draw. */
static void restore_first_person(void)
{
    uint32_t si;
    for (si = 0; si < FP_SETS; si++) {
        FpTrack *t = &s_fp[si];
        if (!t->written)
            continue;
        t->written = 0;
        memcpy(guest_floats(0x4CE3F8u + si * FP_SET_BYTES + 12), t->orig, (size_t)t->n * NODE_BYTES);
    }
    s_fp_sets = 0;
}

void h2_interp_render_begin(void)
{
    if (!interp_enabled())
        return;
    if (s_probe > 0 && getenv("RECOMP_HALO2_INTERP_PROBE_LATE"))
        probe_frame();          /* RECOMP_HALO2_INTERP_PROBE_LATE=1: after the per-frame update, just before the draw */
    trace_point('D');
    s_frame++;
}

/* The line a test clip (RECOMP_CLIP=1, Delete) keeps for this frame: the game's
 * update counter, what was blended this frame, the frame interval, and the
 * player's body as drawn against where the game has it. */
static unsigned s_note_obj, s_note_pts, s_note_cam, s_note_chg, s_note_late, s_note_cut, s_note_snap, s_note_fpcut;
static void clip_note_reset(void) { s_note_obj = s_note_pts = s_note_cam = s_note_chg = s_note_late = s_note_cut = s_note_snap = s_note_fpcut = 0; }

static void clip_note(void)
{
    uint32_t hdr = MEM32(PLAYERS_ARRAY_PTR), objs = MEM32(OBJECT_ARRAY_PTR);
    char body[96] = "body -";
    if (hdr >= 0x80000000u && hdr < GUEST_HIGH_END && MEM32(hdr + 0x2C) == DATA_ARRAY_SIG &&
        objs >= 0x80000000u && objs < GUEST_HIGH_END && MEM32(objs + 0x2C) == DATA_ARRAY_SIG &&
        (MEM32(MEM32(hdr + ARRAY_DATA)) & 0xFFFFu)) {
        uint32_t unit = MEM32(MEM32(hdr + ARRAY_DATA) + PLAYER_UNIT), idx = unit & 0xFFFFu;
        if (unit != 0xFFFFFFFFu && idx < MAX_OBJECTS && s_obj[idx].handle == unit && s_obj[idx].cur) {
            const Obj *o = &s_obj[idx];
            float dx = 0, dy = 0, dz = 0;
            if (o->moved) { dx = o->shown[0] - o->cur[0]; dy = o->shown[1] - o->cur[1]; dz = o->shown[2] - o->cur[2]; }
            snprintf(body, sizeof body, "body %.2f %.2f %.2f drawn %+.3f %+.3f %+.3f step %.3f",
                     o->cur[0], o->cur[1], o->cur[2], dx, dy, dz, o->step);
        }
    }
    {
        char cam[80] = "";
        if (s_dbg_cam_blends != s_note_cam)
            snprintf(cam, sizeof cam, " | camera %.2f %.2f %.2f drawn %+.3f %+.3f %+.3f",
                     s_cam_raw[0], s_cam_raw[1], s_cam_raw[2], s_cam_shown[0] - s_cam_raw[0],
                     s_cam_shown[1] - s_cam_raw[1], s_cam_shown[2] - s_cam_raw[2]);
        snprintf(g_rclip_note, sizeof g_rclip_note, "upd %u | changed %u late %u | blended: objects %u particles %u camera %u | cuts: camera %u first-person %u, objects held as cut %u | first-person sets blended %X | frame %.1f ms | %s%s",
                 MEM32(GAME_TICK_COUNTER), s_dbg_changed - s_note_chg, s_dbg_late - s_note_late, s_dbg_blended - s_note_obj, s_dbg_points_blended - s_note_pts,
                 s_dbg_cam_blends - s_note_cam, s_dbg_cam_cuts - s_note_cut, s_dbg_fp_cuts - s_note_fpcut, s_dbg_snap - s_note_snap,
                 s_fp_sets, s_frame_dt * 1000.0, body, cam);
    }
    s_note_chg = s_dbg_changed; s_note_late = s_dbg_late; s_note_obj = s_dbg_blended; s_note_pts = s_dbg_points_blended; s_note_cam = s_dbg_cam_blends;
    s_note_cut = s_dbg_cam_cuts; s_note_snap = s_dbg_snap; s_note_fpcut = s_dbg_fp_cuts;
}

void h2_interp_render_end(void)
{
    if (s_on <= 0) {
        if (s_on == 0 && !g_rclip_note[0])
            snprintf(g_rclip_note, sizeof g_rclip_note, "smoothing off (RECOMP_HALO2_INTERP not set)");
        return;
    }
    {   /* objects the blend left alone that the game changed after the update hook */
        uint32_t i;
        for (i = 0; i < MAX_OBJECTS; i++) {
            const Obj *o = &s_obj[i];
            if (o->handle && !o->moved && o->cur && object_still(i, o)) {
                obj_read(o, s_raw);
                if (memcmp(s_raw, o->cur, o->n * 4u))
                    s_dbg_late++;
            }
        }
    }
    clip_note();
    {
        double t0 = now_seconds();
        restore_objects();
        restore_points();
        restore_camera();
        restore_first_person();
        s_dbg_cost += now_seconds() - t0;
    }
    probe_restore();
    if (++s_dbg_frames % 600 == 0) {
        fprintf(stderr, "[INTERP] last 600 frames: %u object frames, %u blended, %u restored, %u kept by the game,"
                        " %u snaps, %u every-frame nodes, %u every-frame-only changes, %u new, %u changed, %u changed late, rejected %u/%u/%u/%u; camera %u blended, %u at a body, %u cuts; first person %u blended, %u cuts; particles %u, %u blended; %.1f us a frame\n",
                s_dbg_objects, s_dbg_blended, s_dbg_restored, s_dbg_kept, s_dbg_snap, s_dbg_everyframe, s_dbg_live_only, s_dbg_new, s_dbg_changed, s_dbg_late, s_dbg_rej[0], s_dbg_rej[1], s_dbg_rej[2], s_dbg_rej[3],
                s_dbg_cam_blends, s_dbg_cam_fp, s_dbg_cam_cuts, s_dbg_fp, s_dbg_fp_cuts, s_dbg_points, s_dbg_points_blended, s_dbg_cost * 1e6 / 600.0);
        s_dbg_points = s_dbg_points_blended = 0;
        clip_note_reset();
        s_dbg_cost = 0;
        s_dbg_objects = s_dbg_blended = s_dbg_restored = s_dbg_kept = s_dbg_snap = s_dbg_new = s_dbg_changed = s_dbg_late = s_dbg_everyframe = s_dbg_live_only = 0;
        memset(s_dbg_rej, 0, sizeof s_dbg_rej);
        s_dbg_cam_blends = s_dbg_cam_fp = s_dbg_cam_cuts = s_dbg_fp = s_dbg_fp_cuts = 0;
    }
}
