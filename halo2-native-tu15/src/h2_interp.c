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
    uint32_t shown_frame;       /* the frame shown was last written */
    int snapped;                /* the last update was a teleport: show it as it is */
} Obj;

static Obj s_obj[MAX_OBJECTS];
static int s_on = -1;
static double s_qpc_scale;
/* The frame interval (host seconds, smoothed). The frame an update is first
 * seen shows it one frame's worth along from the previous one, so the last
 * frame before the next update reaches it: even steps at any refresh rate
 * (60 Hz: halfway, then all the way). */
static double s_frame_dt = 1.0 / 60.0, s_last_frame_time;
static uint32_t s_frame;
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
    o->buf = o->prev = o->cur = o->shown = NULL;
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
 * near the object. */
static int looks_like_object(uint32_t data, uint32_t off, uint32_t nodes)
{
    const float *pos = guest_floats(data + OBJECT_POSITION), *m = guest_floats(data + off);
    uint32_t j, a;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]) ||
        fabsf(pos[0]) > 1e5f || fabsf(pos[1]) > 1e5f || fabsf(pos[2]) > 1e5f)
        return 0;
    for (j = 0; j < nodes; j++, m += NODE_FLOATS) {
        if (!(m[0] > 0.01f && m[0] < 100.0f))
            return 0;
        for (a = 1; a < 10; a += 3) {
            float l = m[a] * m[a] + m[a + 1] * m[a + 1] + m[a + 2] * m[a + 2];
            if (!(l > 0.9f && l < 1.1f))
                return 0;
        }
        if (!(dist2(m + 10, pos) < 400.0f))   /* within 20 units */
            return 0;
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
        if (!o->buf)
            return;
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
     * frame before the bodies move (measured 2026-11-23). */
    if (memcmp(s_raw, o->cur, n * 4u)) {
        float *t = o->prev;
        o->prev = o->cur;
        o->cur = t;
        memcpy(o->cur, s_raw, n * 4u);
        o->tick_time = now;
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
        /* An update that comes before the last one was fully shown (the frame and
         * update clocks drift apart when the screen is not 60 Hz) starts from what
         * the last frame showed, not from the last update: no jolt. */
        if (o->shown_frame == s_frame - 1 && !o->snapped)
            memcpy(o->prev, o->shown, n * 4u);
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
            return;
        }
        alpha = (float)((now - o->tick_time + s_frame_dt) * 30.0);
        if (alpha >= 1.0f)
            return;                                 /* the current state: nothing to change */
        if (!memcmp(o->prev, o->cur, n * 4u))
            return;                                 /* not moving */
        if (alpha < 0.0f)
            alpha = 0.0f;
        for (k = 0; k < n; k++)
            o->shown[k] = o->prev[k] + (o->cur[k] - o->prev[k]) * alpha;
        for (j = 0; j < nodes; j++) {               /* keep the axes unit length */
            float *m = o->shown + 3 + j * NODE_FLOATS;
            normalize3(m + 1); normalize3(m + 4); normalize3(m + 7);
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
            else
                s_dbg_kept++;                       /* rebuilt by the game during the frame (a turret aimed) */
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

static void world_updated(void)
{
    uint32_t hdr, entries, max, i;
    double now = now_seconds();
    if (s_last_frame_time > 0 && now - s_last_frame_time > 0 && now - s_last_frame_time < 0.1)
        s_frame_dt += (now - s_last_frame_time - s_frame_dt) * 0.05;
    s_last_frame_time = now;
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
} CamTrack;

typedef struct {
    CamTrack pos, rot;          /* floats 0-2; floats 3-9 */
} CamState;

static CamState s_cam[MAX_VIEWS];
static uint32_t s_cam_frame;    /* frame of the copies counted in s_cam_index */
static int s_cam_index;
static int s_cam_moved;
static float s_cam_raw[CAM_FLOATS], s_cam_shown[CAM_FLOATS];
static unsigned s_dbg_cam_blends, s_dbg_cam_fp;

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
        t->stepping = s_frame - t->changed_frame >= 2;    /* held for a frame before this change */
        t->changed_frame = s_frame;
        /* from what the last frame showed, if it was still on its way */
        memcpy(t->prev, t->shown_frame == s_frame - 1 ? t->shown : t->cur, n * sizeof(float));
        memcpy(t->cur, raw, n * sizeof(float));
        t->t = now;
    } else if (s_frame - t->changed_frame > 3)
        t->stepping = 0;                            /* holding still: nothing to blend */
    if (!t->stepping)
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

void h2_interp_render_begin(void)
{
    if (!interp_enabled())
        return;
    if (s_probe > 0 && getenv("RECOMP_HALO2_INTERP_PROBE_LATE"))
        probe_frame();          /* RECOMP_HALO2_INTERP_PROBE_LATE=1: after the per-frame update, just before the draw */
    s_frame++;
}

void h2_interp_render_end(void)
{
    if (s_on <= 0)
        return;
    {
        double t0 = now_seconds();
        restore_objects();
        restore_points();
        restore_camera();
        s_dbg_cost += now_seconds() - t0;
    }
    probe_restore();
    if (++s_dbg_frames % 600 == 0) {
        fprintf(stderr, "[INTERP] last 600 frames: %u object frames, %u blended, %u restored, %u kept by the game,"
                        " %u snaps, %u new; camera %u blended, %u at a body; particles %u, %u blended; %.1f us a frame\n",
                s_dbg_objects, s_dbg_blended, s_dbg_restored, s_dbg_kept, s_dbg_snap, s_dbg_new,
                s_dbg_cam_blends, s_dbg_cam_fp, s_dbg_points, s_dbg_points_blended, s_dbg_cost * 1e6 / 600.0);
        s_dbg_points = s_dbg_points_blended = 0;
        s_dbg_cost = 0;
        s_dbg_objects = s_dbg_blended = s_dbg_restored = s_dbg_kept = s_dbg_snap = s_dbg_new = 0;
        s_dbg_cam_blends = s_dbg_cam_fp = 0;
    }
}
