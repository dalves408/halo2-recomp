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
#include "settings.h"   /* halo2_cutscene_view */
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
#define TAG_INSTANCES     0x4F0414u   /* -> the tag table: 16 bytes a tag (group, index, data, size); retail 0x4E3B44 */
#define DEF_MODEL         0x38u       /* object definition: the model tag's index */
#define MODEL_GRAPH       0x14u       /* model: the animation graph tag's index */
#define GRAPH_NODES       0x0Cu       /* animation graph: node count, then nodes (0x20 bytes each, parent index at +8) */

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
    uint8_t *carry;             /* per game-moved node: 1 + the blended node it rides with (0: not chosen yet) */
    uint8_t *parent;            /* per node: 1 + its skeleton parent from the animation graph (0: none or unknown) */
    uint32_t upd_frame;         /* the frame the last update was seen */
    uint32_t upd_tick;          /* the game's tick counter at the last update */
} Obj;

/* A node the game moves every frame itself (a cutscene character's head and
 * arms between updates, measured 2026-11-24 in the Arbiter intro: 17 of 48
 * nodes): its change is no update, and the blend leaves it as the game has it. */
static int s_no_live;   /* test: RECOMP_HALO2_INTERP_NOLIVE=1 treats every node as updated with the world */
static int s_fp_rule;        /* first-person blend; RECOMP_HALO2_INTERP_FP=0 turns it off (test) */
static int s_cam_cut_rule;   /* the held-still / big-step camera cut rule; RECOMP_HALO2_INTERP_CAMCUT=0 turns it off (test) */
static int s_cam_minor_rule; /* a tiny change between updates is no update; RECOMP_HALO2_INTERP_CAMMINOR=0 turns it off (test) */
static int s_every_rule;     /* a few nodes changing the frame after an update are the game's own; RECOMP_HALO2_INTERP_EVERYRULE=0 turns it off (test) */
static unsigned s_dbg_every_kept;   /* times that rule kept an object's update timing */
static int s_tick_rule;      /* one update per game tick; RECOMP_HALO2_INTERP_TICKRULE=0 turns it off (test) */
static unsigned s_dbg_same_tick;    /* second poses within one tick taken without restarting the blend */
/* The game's tick counter: its value, and the host time and frame it changed. An
 * update is timed from its tick, not from the frame it is first seen on: bodies
 * were seen one frame late at times (2026-11-25, Outskirts Pelican marines), and
 * timed from then they moved unevenly. RECOMP_HALO2_INTERP_TICKTIME=0 times from
 * the frame seen (test). */
static uint32_t s_tick_val, s_tick_frame;
static double s_tick_time;
static int s_ticktime_rule;
static int s_midframe_rule;         /* most nodes changed during the frame: an update, not rebuilt nodes */
static int s_noise_rule;            /* a node changed only by float noise has not moved */
static int s_livepos_rule;          /* every-frame nodes: the game's axes, the blend's position (no carry) */
static int s_trackonly;             /* test: RECOMP_HALO2_INTERP_TRACKONLY=1 watches, writes nothing */
static int s_camslot_rule;          /* a view keeps the slot of the nearest camera; RECOMP_HALO2_INTERP_CAMSLOT=0 off */
static int s_camtick_rule;          /* a camera change with a new tick keeps blending; RECOMP_HALO2_INTERP_CAMTICK=0 off */
/* Test switches read in per-object / per-frame code, read once (interp_enabled): a getenv()
 * for every object every frame cost time in normal play. NULL when unset. */
static const char *s_env_livelog;
static const char *s_env_everylog;
static const char *s_env_snaplog;
static const char *s_env_livewhole;
static const char *s_env_livecarry;
static const char *s_env_carrylog;
static const char *s_env_skelcarry;
static const char *s_env_carrysticky;
static const char *s_env_carrypos;
static const char *s_env_objlog;
static const char *s_env_camlog;
static const char *s_env_probe_late;
static const char *s_env_skellog;
static void phase_probe(char where);
static void interp_switch_pending(void);
static unsigned s_dbg_noise;
static unsigned s_dbg_midframe;
static unsigned s_dbg_late_seen;    /* updates seen later than the frame after their tick */
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

/* The player's setting (smooth_motion in settings.ini, F1 menu, Config.exe):
 * on by default. RECOMP_HALO2_INTERP=0/1 overrides it (test builds; release
 * builds drop RECOMP_* variables). A change takes effect at the start of the
 * next frame's update hook, never mid-frame: the frame before has already put
 * the game's own values back. */
static int s_setting = 1;
static int s_override = -1;             /* RECOMP_HALO2_INTERP: -1 none, 0 off, 1 on */
static volatile LONG s_want = -1;       /* a switch waiting for the next frame */

void h2_interp_set_enabled(int on)
{
    s_setting = on ? 1 : 0;
    InterlockedExchange(&s_want, s_setting);
}

static int interp_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_HALO2_INTERP");
        LARGE_INTEGER f;
        s_override = e && *e ? (e[0] == '1') : -1;
        s_on = s_override >= 0 ? s_override : s_setting;
        s_no_live = getenv("RECOMP_HALO2_INTERP_NOLIVE") != NULL;
        s_fp_rule = !(getenv("RECOMP_HALO2_INTERP_FP") && getenv("RECOMP_HALO2_INTERP_FP")[0] == '0');
        s_cam_cut_rule = !(getenv("RECOMP_HALO2_INTERP_CAMCUT") && getenv("RECOMP_HALO2_INTERP_CAMCUT")[0] == '0');
        s_cam_minor_rule = !(getenv("RECOMP_HALO2_INTERP_CAMMINOR") && getenv("RECOMP_HALO2_INTERP_CAMMINOR")[0] == '0');
        s_every_rule = !(getenv("RECOMP_HALO2_INTERP_EVERYRULE") && getenv("RECOMP_HALO2_INTERP_EVERYRULE")[0] == '0');
        s_tick_rule = !(getenv("RECOMP_HALO2_INTERP_TICKRULE") && getenv("RECOMP_HALO2_INTERP_TICKRULE")[0] == '0');
        s_ticktime_rule = getenv("RECOMP_HALO2_INTERP_TICKTIME") && getenv("RECOMP_HALO2_INTERP_TICKTIME")[0] == '1';   /* measured worse, 2026-11-25: off */
        s_midframe_rule = !(getenv("RECOMP_HALO2_INTERP_MIDFRAME") && getenv("RECOMP_HALO2_INTERP_MIDFRAME")[0] == '0');
        s_noise_rule = !(getenv("RECOMP_HALO2_INTERP_NOISE") && getenv("RECOMP_HALO2_INTERP_NOISE")[0] == '0');
        s_livepos_rule = !(getenv("RECOMP_HALO2_INTERP_LIVEPOS") && getenv("RECOMP_HALO2_INTERP_LIVEPOS")[0] == '0');
        s_trackonly = getenv("RECOMP_HALO2_INTERP_TRACKONLY") && getenv("RECOMP_HALO2_INTERP_TRACKONLY")[0] == '1';
        s_camslot_rule = !(getenv("RECOMP_HALO2_INTERP_CAMSLOT") && getenv("RECOMP_HALO2_INTERP_CAMSLOT")[0] == '0');
        s_camtick_rule = !(getenv("RECOMP_HALO2_INTERP_CAMTICK") && getenv("RECOMP_HALO2_INTERP_CAMTICK")[0] == '0');
        s_env_livelog = getenv("RECOMP_HALO2_INTERP_LIVELOG");
        s_env_everylog = getenv("RECOMP_HALO2_INTERP_EVERYLOG");
        s_env_snaplog = getenv("RECOMP_HALO2_INTERP_SNAPLOG");
        s_env_livewhole = getenv("RECOMP_HALO2_INTERP_LIVEWHOLE");
        s_env_livecarry = getenv("RECOMP_HALO2_INTERP_LIVECARRY");
        s_env_carrylog = getenv("RECOMP_HALO2_INTERP_CARRYLOG");
        s_env_skelcarry = getenv("RECOMP_HALO2_INTERP_SKELCARRY");
        s_env_carrysticky = getenv("RECOMP_HALO2_INTERP_CARRYSTICKY");
        s_env_carrypos = getenv("RECOMP_HALO2_INTERP_CARRYPOS");
        s_env_objlog = getenv("RECOMP_HALO2_INTERP_OBJLOG");
        s_env_camlog = getenv("RECOMP_HALO2_INTERP_CAMLOG");
        s_env_probe_late = getenv("RECOMP_HALO2_INTERP_PROBE_LATE");
        s_env_skellog = getenv("RECOMP_HALO2_INTERP_SKELLOG");
        QueryPerformanceFrequency(&f);
        s_qpc_scale = 1.0 / (double)f.QuadPart;
        if (s_on && getenv("RECOMP_HALO2_INTERP_LIFT"))
            s_lift = (float)atof(getenv("RECOMP_HALO2_INTERP_LIFT"));
        fprintf(stderr, "[INTERP] smooth motion %s (%s)%s\n", s_on ? "on" : "off",
                s_override >= 0 ? "RECOMP_HALO2_INTERP" : "setting", s_lift ? ", lift test" : "");
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
    o->carry = NULL;
    o->parent = NULL;
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

/* Test (RECOMP_HALO2_INTERP_DRAWLOG=1): where a skeletal object's first and middle
 * nodes are drawn this frame, to measure how evenly characters move. */
static void draw_log(uint32_t index, uint32_t nodes, const float *s, float alpha, int live, int run, int kept)
{
    static int on = -1;
    static unsigned logged;
    if (on < 0)
        on = getenv("RECOMP_HALO2_INTERP_DRAWLOG") != NULL;
    if (!on || nodes < 20 || logged++ >= 400000u)
        return;
    fprintf(stderr, "[DRAW] %u %u %.4f %.4f %.4f %.4f %.4f %.4f a %.2f live %d run %d kept %d dt %.1f\n", s_frame, index,
            s[3 + 10], s[3 + 11], s[3 + 12],
            s[3 + (nodes / 2) * NODE_FLOATS + 10], s[3 + (nodes / 2) * NODE_FLOATS + 11], s[3 + (nodes / 2) * NODE_FLOATS + 12],
            alpha, live, run, kept, s_frame_dt * 1000.0);
}

/* Test log helper: is node p an ancestor of node j in the skeleton read from the tags? */
static int carry_is_ancestor(const Obj *o, uint32_t j, uint32_t p, uint32_t nodes)
{
    uint32_t up = j, hops = 0;
    while (o->parent[up] && hops++ < nodes) {
        up = o->parent[up] - 1u;
        if (up == p)
            return 1;
    }
    return 0;
}

/* A tag's data, or 0. */
static uint32_t tag_data(uint32_t index)
{
    uint32_t table = MEM32(TAG_INSTANCES), e, d;
    if (index == 0xFFFFFFFFu || table < 0x10000u || table >= GUEST_HIGH_END)
        return 0;
    e = table + (index & 0xFFFFu) * 16u;
    if (e + 16u > GUEST_HIGH_END || MEM32(e + 4) != index)
        return 0;
    d = MEM32(e + 8);
    return d >= 0x10000u && d < GUEST_HIGH_END ? d : 0;
}

/* Each node's skeleton parent, from the object's animation graph (the game
 * builds a node from its parent's matrix, 2026-11-25). Left 0 when the graph
 * cannot be read or does not have the object's node count. */
static void read_parents(Obj *o, uint32_t data, uint32_t nodes)
{
    uint32_t def = tag_data(MEM32(data)), model, graph, list, j;
    static int logged;
    model = def ? tag_data(MEM32(def + DEF_MODEL)) : 0;
    graph = model ? tag_data(MEM32(model + MODEL_GRAPH)) : 0;
    list = graph ? MEM32(graph + GRAPH_NODES + 4) : 0;
    if (s_env_skellog && nodes >= 20 && logged++ < 200)
        fprintf(stderr, "[SKEL] obj tag %08X def %08X model %08X graph %08X count %u nodes %u\n", MEM32(data), def, model,
                graph, graph ? MEM32(graph + GRAPH_NODES) : 0, nodes);
    if (!graph || MEM32(graph + GRAPH_NODES) != nodes || list < 0x10000u || list + nodes * 0x20u > GUEST_HIGH_END)
        return;
    for (j = 0; j < nodes; j++) {
        uint32_t q = MEM32(list + j * 0x20u + 8) & 0xFFFFu;
        o->parent[j] = q < nodes && q != j ? (uint8_t)(q + 1u) : 0;
    }
}

static uint32_t s_objs_moved;   /* objects the blend drew away from the game's pose this frame */
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
        o->node_seen = (uint32_t *)calloc(nodes + 1, sizeof(uint32_t) + 4);
        if (!o->buf || !o->node_seen) {
            obj_forget(o);
            return;
        }
        o->node_run = (uint8_t *)(o->node_seen + nodes + 1);
        o->node_kept = o->node_run + nodes + 1;
        o->carry = o->node_kept + nodes + 1;
        o->parent = o->carry + nodes + 1;
        o->cur = o->prev + n;
        o->shown = o->cur + n;
        o->handle = handle;
        o->data = data;
        o->nodes_off = off;
        o->n = n;
        obj_read(o, o->cur);
        read_parents(o, data, nodes);
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
    if (s_trackonly) {   /* test: watch only -- which skeletal objects change on frames with no new tick */
        if (memcmp(s_raw, o->cur, n * 4u)) {
            static unsigned logged;
            uint32_t ch = 0;
            for (j = 0; j < nodes; j++) {
                const float *a = s_raw + 3 + j * NODE_FLOATS, *b = o->cur + 3 + j * NODE_FLOATS;
                int k2;
                for (k2 = 0; k2 < NODE_FLOATS; k2++)
                    if (fabsf(a[k2] - b[k2]) > 2e-4f) { ch++; break; }
            }
            if (nodes >= 20 && logged++ < 100000u)
                fprintf(stderr, "[RAW] frame %u newtick %d obj %u tag %08X nodes %u changed %u pos %d nodes_va %08X\n", s_frame,
                        s_tick_frame == s_frame, index, MEM32(data), nodes, ch, memcmp(s_raw, o->cur, 12) != 0, data + off);
            memcpy(o->cur, s_raw, n * 4u);
        }
        return;
    }
    /* A change is a new update. The tick counter is no help: it changes a
     * frame before the bodies move (measured 2026-11-23). Changes only in nodes
     * the game moves every frame are not. */
    if (memcmp(s_raw, o->cur, n * 4u)) {
        int update = memcmp(s_raw, o->cur, 12) != 0;
        for (j = 0; j < nodes; j++) {
            const float *a = s_raw + 3 + j * NODE_FLOATS, *b = o->cur + 3 + j * NODE_FLOATS;
            if (!memcmp(a, b, NODE_BYTES))
                continue;
            /* Float noise is no change (2026-11-25, Outskirts Pelican: marines'
             * nodes rebuilt every frame differed only in the last bits, so they
             * counted as moved every frame and were drawn unblended, stepping at
             * 30 Hz). The value is still taken. RECOMP_HALO2_INTERP_NOISE=0 counts it (test). */
            if (s_noise_rule) {
                int k2, moved = 0;
                for (k2 = 0; k2 < NODE_FLOATS && !moved; k2++)
                    moved = fabsf(a[k2] - b[k2]) > 2e-4f;
                if (!moved) {
                    s_dbg_noise++;
                    continue;
                }
            }
            o->node_run[j] = o->node_seen[j] == s_frame - 1 ? (uint8_t)(o->node_run[j] < 255 ? o->node_run[j] + 1 : 255) : 0;
            o->node_seen[j] = s_frame;
            if (!NODE_LIVE(o, j))
                update = 1;
        }
        /* A few nodes changing the frame after an update, with the object in place,
         * are the game's every-frame nodes that skipped a frame, not a new update
         * (2026-11-25, Outskirts Pelican: marines' arms; each such change restarted
         * the whole body's blend and it moved unevenly). Treat them as the game's
         * own for a few updates. RECOMP_HALO2_INTERP_EVERYRULE=0 turns it off (test). */
        if (update && s_every_rule && o->upd_frame == s_frame - 1 && !memcmp(s_raw, o->cur, 12)) {
            uint32_t changed = 0, blended = 0;
            for (j = 0; j < nodes; j++)
                if (!NODE_LIVE(o, j)) {
                    blended++;
                    changed += memcmp(s_raw + 3 + j * NODE_FLOATS, o->cur + 3 + j * NODE_FLOATS, NODE_BYTES) != 0;
                }
            if (changed && changed * 2 <= blended) {
                for (j = 0; j < nodes; j++)
                    if (!NODE_LIVE(o, j) && memcmp(s_raw + 3 + j * NODE_FLOATS, o->cur + 3 + j * NODE_FLOATS, NODE_BYTES))
                        o->node_kept[j] = 4;
                update = 0;
                s_dbg_every_kept++;
            }
        }
        if (s_env_livelog && s_frame % 600 == 1) {   /* test: objects with every-frame nodes */
            uint32_t live = 0;
            for (j = 0; j < nodes; j++) live += NODE_LIVE(o, j);
            if (live)
                fprintf(stderr, "[LIVE] frame %u obj %u tag %08X nodes %u live %u update %d run0 %u\n", s_frame, index,
                        MEM32(data), nodes, live, update, o->node_run[0]);
        }
        if (update)
            for (j = 0; j < nodes; j++)
                if (o->node_kept[j]) o->node_kept[j]--;
        /* A second whole new pose within one game tick is no new update (2026-11-25,
         * Outskirts Pelican: marines got two full poses on back-to-back frames,
         * and each restarted the blend, so they moved unevenly). Take its values,
         * keep the update's timing. RECOMP_HALO2_INTERP_TICKRULE=0 turns it off (test). */
        if (update && s_tick_rule && o->upd_frame && MEM32(GAME_TICK_COUNTER) == o->upd_tick &&
            memcmp(s_raw, o->cur, 12) == 0) {
            update = 0;
            s_dbg_same_tick++;
        }
        if (!update) {
            memcpy(o->cur, s_raw, n * 4u);          /* the game's own every-frame nodes: keep the update's timing */
            s_dbg_live_only++;
        } else {
            float *t = o->prev;
            o->prev = o->cur;
            o->cur = t;
            memcpy(o->cur, s_raw, n * 4u);
            o->tick_time = now;
            if (s_ticktime_rule && s_tick_frame && s_frame - s_tick_frame <= 3) {
                double t = s_tick_time + s_frame_dt;   /* bodies move the frame after the counter */
                if (t < now) {
                    o->tick_time = t;
                    s_dbg_late_seen++;
                }
            }
            s_dbg_changed++;
            if (s_env_everylog && o->upd_frame == s_frame - 1) {   /* test: updates on consecutive frames */
                static unsigned logged;
                uint32_t nch = 0, nlive = 0, first = nodes, parent_obj = MEM32(data + 0x14);
                float most = 0.0f;
                for (j = 0; j < nodes; j++) {
                    float d = dist2(o->prev + 3 + j * NODE_FLOATS + 10, o->cur + 3 + j * NODE_FLOATS + 10);
                    nlive += NODE_LIVE(o, j);
                    if (memcmp(o->prev + 3 + j * NODE_FLOATS, o->cur + 3 + j * NODE_FLOATS, NODE_BYTES)) {
                        nch++;
                        if (first == nodes) first = j;
                    }
                    if (d > most) most = d;
                }
                if (logged++ < 20000u)
                    fprintf(stderr, "[EVERY] frame %u obj %u tag %08X nodes %u pos %.2f %.2f %.2f step %.5f changed %u live %u first %u most %.4f parent %08X shown_prev %d\n",
                            s_frame, index, MEM32(data), nodes, s_raw[0], s_raw[1], s_raw[2], sqrtf(dist2(o->prev, o->cur)),
                            nch, nlive, first, sqrtf(most), parent_obj, o->shown_frame == s_frame - 1);
            }
            o->upd_frame = s_frame;
            o->upd_tick = MEM32(GAME_TICK_COUNTER);
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
                    if (s_env_snaplog) {   /* test: what the bone-jump rule caught */
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
        if (alpha >= 1.0f) {
            draw_log(index, nodes, o->cur, alpha, NODE_LIVE(o, nodes / 2), o->node_run[nodes / 2], o->node_kept[nodes / 2]);
            return;                                 /* the current state: nothing to change */
        }
        if (s_env_livewhole)   /* test: an object with game-moved nodes is not blended at all */
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
                /* ... but its place blended (2026-11-25, Outskirts Pelican: a marine's
                 * every-frame nodes turned every frame yet moved only with the updates,
                 * and carried along with a still neighbour they stepped at 30 Hz).
                 * The game's axes, the blend's position. RECOMP_HALO2_INTERP_LIVEPOS=0
                 * shows the whole node as the game has it and carries it (test). */
                if (s_livepos_rule)
                    memcpy(m, o->cur + 3 + j * NODE_FLOATS, 10 * sizeof(float));
                else
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
        if (!s_livepos_rule && !(s_env_livecarry && s_env_livecarry[0] == '0'))
            for (j = 0; j < nodes; j++) {
                const float *c = o->cur + 3 + j * NODE_FLOATS, *cp, *sp;
                float *m = o->shown + 3 + j * NODE_FLOATS, best = 1e30f, R[9], t[3];
                uint32_t q, p = nodes;
                int a, b;
                if (!NODE_LIVE(o, j))
                    continue;
                /* The nearest blended node, chosen each frame. Keeping the first choice
                 * (RECOMP_HALO2_INTERP_CARRYSTICKY=1, test) measured worse on the
                 * Metropolis intro: 31% of carried-node steps reversed against 18%
                 * (2026-11-25) -- the nearest node is not the skeleton's parent. */
                if (s_env_carrylog && o->carry[j]) {   /* test: would a fresh choice differ? */
                    static int logged;
                    uint32_t fresh = nodes;
                    float fb = 1e30f;
                    for (q = 0; q < nodes; q++) {
                        float d;
                        if (q == j || NODE_LIVE(o, q))
                            continue;
                        d = dist2(c + 10, o->cur + 3 + q * NODE_FLOATS + 10);
                        if (d < fb) { fb = d; fresh = q; }
                    }
                    if (fresh != o->carry[j] - 1u && logged++ < 2000)
                        fprintf(stderr, "[CARRY] frame %u obj %u node %u rides %u, nearest now %u\n",
                                s_frame, index, j, o->carry[j] - 1u, fresh);
                }
                /* Its nearest blended ancestor in the skeleton: the game builds the node
                 * from that chain, so it rides exactly as its own parent is drawn.
                 * RECOMP_HALO2_INTERP_SKELCARRY=0 picks the nearest node (test). */
                if (!(s_env_skelcarry && s_env_skelcarry[0] == '0')) {
                    uint32_t up = j, hops = 0;
                    while (o->parent[up] && hops++ < nodes) {
                        up = o->parent[up] - 1u;
                        if (!NODE_LIVE(o, up)) { p = up; break; }
                    }
                }
                if (p != nodes)
                    o->carry[j] = (uint8_t)(p + 1u);
                else if (o->carry[j] && o->carry[j] - 1u < nodes && o->carry[j] - 1u != j && !NODE_LIVE(o, o->carry[j] - 1u) &&
                    s_env_carrysticky)   /* test: keep the first choice (measured worse, 2026-11-25) */
                    p = o->carry[j] - 1u;
                else {
                    for (q = 0; q < nodes; q++) {
                        float d;
                        if (q == j || NODE_LIVE(o, q))
                            continue;
                        d = dist2(c + 10, o->cur + 3 + q * NODE_FLOATS + 10);
                        if (d < best) { best = d; p = q; }
                    }
                    if (p == nodes)
                        continue;                   /* no blended node: as the game has it */
                    o->carry[j] = (uint8_t)(p + 1u);
                }
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
                if (s_env_carrypos && nodes >= 20) {   /* test: drawn position of carried nodes */
                    static unsigned logged;
                    if (logged++ < 200000u)
                        fprintf(stderr, "[CPOS] %u %u %u %.5f %.5f %.5f rides %u parent %d anc %d pmove %.5f a %.2f\n", s_frame, index, j,
                                m[10], m[11], m[12], p, (int)o->parent[j] - 1, carry_is_ancestor(o, j, p, nodes),
                                sqrtf(dist2(sp + 10, cp + 10)), alpha);
                }
            }
        if (s_env_objlog && nodes >= 20 && s_frame <= s_objlog_until) {   /* test: skeletal objects' blend for 14 frames after a camera cut */
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
    draw_log(index, nodes, o->shown, alpha, NODE_LIVE(o, nodes / 2), o->node_run[nodes / 2], o->node_kept[nodes / 2]);
    memcpy(guest_floats(data + OBJECT_POSITION), o->shown, 12);
    memcpy(guest_floats(data + off), o->shown + 3, (n - 3) * 4u);
    o->moved = 1;
    o->shown_frame = s_frame;
    s_dbg_blended++;
    s_objs_moved++;
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
static Obj *obj_by_nodes(uint32_t va)
{
    uint32_t i;
    for (i = 0; i < MAX_OBJECTS; i++)
        if (s_obj[i].handle && s_obj[i].data + s_obj[i].nodes_off == va)
            return &s_obj[i];
    return NULL;
}

static void restore_objects(void)
{
    uint32_t i, j;
    s_objs_moved = 0;
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
        {
            /* Most nodes changed: a new update landed during the frame, not a node
             * the game rebuilds itself (2026-11-25, Outskirts Pelican: 31 of a
             * marine's 53 nodes were marked and drawn at 30 Hz). Keep the game's
             * values, mark nothing. RECOMP_HALO2_INTERP_MIDFRAME=0 marks them (test). */
            uint32_t nn = (o->n - 3) / NODE_FLOATS, diff = 0;
            int midframe;
            for (j = 0; j < nn; j++)
                diff += memcmp(g + j * NODE_FLOATS, o->shown + 3 + j * NODE_FLOATS, NODE_BYTES) != 0;
            midframe = s_midframe_rule && nn >= 4 && diff * 2 > nn;
            if (midframe)
                s_dbg_midframe++;
            for (j = 0; j < nn; j++) {
                const float *sh = o->shown + 3 + j * NODE_FLOATS;
                if (!memcmp(g + j * NODE_FLOATS, sh, NODE_BYTES))
                    memcpy(g + j * NODE_FLOATS, o->cur + 3 + j * NODE_FLOATS, NODE_BYTES);
                else {
                    s_dbg_kept++;                   /* rebuilt by the game during the frame (a turret aimed) */
                    if (o->node_kept && !midframe)
                        o->node_kept[j] = 8;        /* left alone for the next 8 updates */
                }
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
    if (s_trackonly)
        return;
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
    phase_probe('U');
    interp_switch_pending();
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
/* Test (RECOMP_HALO2_INTERP_PHASE=<guest va>[,bytes]): at each hook of the frame, whether a
 * block of guest memory (an object's nodes) changed since the last hook, to find in
 * which part of the frame the game writes it. */
static void phase_probe(char where)
{
    static int on = -1;
    static uint32_t va, len = 53u * NODE_BYTES, last, logged;
    uint32_t h = 2166136261u, i;
    const uint8_t *p;
    if (on < 0) {
        const char *e = getenv("RECOMP_HALO2_INTERP_PHASE");
        on = e != NULL;
        if (e) {
            char *end;
            va = (uint32_t)strtoul(e, &end, 16);
            if (*end == ',')
                len = (uint32_t)strtoul(end + 1, NULL, 0);
        }
    }
    if (!on || va < 0x10000u || va + len > GUEST_HIGH_END)
        return;
    p = (const uint8_t *)XBOX_PTR(va);
    for (i = 0; i < len; i++)
        h = (h ^ p[i]) * 16777619u;
    if (h != last && logged++ < 200000u)
        fprintf(stderr, "[PHASE] frame %u %c tick %u newtick %d\n", s_frame, where, MEM32(GAME_TICK_COUNTER),
                s_tick_frame == s_frame);
    last = h;
}

/* ---- the view's object set-up between ticks
 *
 * Each view's set-up (sub_0002B970, retail 0x2b990: it builds the visible object list)
 * re-poses some cutscene characters itself -- heads, faces, hands, fur, a dropship's hatch --
 * from the pose it finds in memory, and leaves the result there. The Xbox drew once a tick, so
 * it only ever re-posed the tick's own pose. Drawn at 60 fps with smooth motion, the views
 * between ticks re-pose the blended pose instead (2026-11-25/26, found with phase probes).
 *
 * Around that call (patch 15), on a view the blend drew between poses: each node the re-pose
 * changed stays as the game set it for the draw (put back, the Heretic's hands and a Brute's
 * fur flashed, a Metropolis Phantom's hatch showed a hole), and after the draw it is moved onto
 * the game's own pose -- its place relative to its skeleton parent as drawn, put on the parent's
 * pose -- in the game's memory and the blend's current pose. Taken as drawn, the blend aimed the
 * next frame at a pose from between ticks: the Outskirts Pelican riders' heads and faces were
 * drawn ~0.05 behind their bodies, the faces vanished and their skin stretched into a slab every
 * other frame (82 such frames in the intro; 0 after). Read as a new update and blended toward,
 * marines' heads smeared (2026-11-25, a test laptop).
 *
 * On with smooth motion, off without it. RECOMP_HALO2_REPOSE=0/1 overrides it (test). */
typedef struct { uint32_t va, len, at; } ReposeSnap;
static ReposeSnap *s_rp;
static uint8_t *s_rp_buf;
static uint32_t s_rp_n, s_rp_cap, s_rp_bytes, s_rp_bufcap, s_rp_last_tick, s_rp_frames, s_rp_changed, s_rp_adopted;
static int s_rp_rule = -1, s_rp_active, s_rp_tickview, s_rp_blendtick;
static int s_rp_nodelog = -1;   /* test: RECOMP_HALO2_REPOSE_NODELOG=1 logs re-poses on tick and other views */
static uint32_t s_rp_dumptag, s_rp_dumplo = 0, s_rp_dumphi = 9999;   /* test: RECOMP_HALO2_REPOSE_DUMPTAG / DUMPNODES */

/* Re-poses left to the game for the draw: where, the values from before the re-pose and the
 * re-pose itself; adopt 1: taken into the blend's pose and the game's memory, 2: the game's
 * memory only (see s_rp_blendtick). */
static ReposeSnap *s_rk;
static uint8_t *s_rk_buf;
static uint32_t s_rk_n, s_rk_cap, s_rk_bytes, s_rk_bufcap;
static uint8_t *s_rk_adopt;
static void repose_keep_note(uint32_t va, const uint8_t *before, uint32_t len, int adopt)
{
    if (s_rk_n == s_rk_cap) {
        uint32_t cap = s_rk_cap ? s_rk_cap * 2 : 64;
        ReposeSnap *r = (ReposeSnap *)realloc(s_rk, cap * sizeof *r);
        uint8_t *f;
        if (!r) return;
        s_rk = r;
        f = (uint8_t *)realloc(s_rk_adopt, cap);
        if (!f) return;
        s_rk_adopt = f; s_rk_cap = cap;
    }
    if (s_rk_bytes + 2 * len > s_rk_bufcap) {
        uint32_t cap = (s_rk_bytes + 2 * len) * 2 + 65536;
        uint8_t *b = (uint8_t *)realloc(s_rk_buf, cap);
        if (!b) return;
        s_rk_buf = b; s_rk_bufcap = cap;
    }
    s_rk[s_rk_n].va = va; s_rk[s_rk_n].len = len; s_rk[s_rk_n].at = s_rk_bytes;
    s_rk_adopt[s_rk_n] = (uint8_t)adopt;
    memcpy(s_rk_buf + s_rk_bytes, before, len);
    memcpy(s_rk_buf + s_rk_bytes + len, XBOX_PTR(va), len);
    s_rk_bytes += 2 * len;
    s_rk_n++;
}

/* A node re-posed on a drawn view, moved onto the game's own pose: its place relative to its
 * skeleton parent as drawn (ps: the parent as drawn, ns: the node as drawn) put on the parent's
 * own pose (pc). Node: scale, three axes (each a world vector), position. */
static void rebase_node(const float *ps, const float *pc, const float *ns, float *out)
{
    float d[3], l[3], r[NODE_FLOATS];
    int k, a;
    float s = ps[0] != 0.0f ? pc[0] / ps[0] : 1.0f;
    for (k = 0; k < 3; k++)
        d[k] = ns[10 + k] - ps[10 + k];
    for (a = 0; a < 3; a++)   /* the offset in the drawn parent's axes */
        l[a] = (d[0] * ps[1 + a * 3] + d[1] * ps[2 + a * 3] + d[2] * ps[3 + a * 3]) * s;
    for (k = 0; k < 3; k++)
        r[10 + k] = pc[10 + k] + l[0] * pc[1 + k] + l[1] * pc[4 + k] + l[2] * pc[7 + k];
    for (a = 0; a < 3; a++) {   /* each of the node's axes, the same way */
        const float *v = ns + 1 + a * 3;
        float m0 = v[0] * ps[1] + v[1] * ps[2] + v[2] * ps[3];
        float m1 = v[0] * ps[4] + v[1] * ps[5] + v[2] * ps[6];
        float m2 = v[0] * ps[7] + v[1] * ps[8] + v[2] * ps[9];
        for (k = 0; k < 3; k++)
            r[1 + a * 3 + k] = m0 * pc[1 + k] + m1 * pc[4 + k] + m2 * pc[7 + k];
    }
    r[0] = ns[0] * s;
    memcpy(out, r, sizeof r);
}

/* After the blend gave the game its values back: each re-posed node the game has not changed
 * since is moved onto the game's own pose (see above). A node with no skeleton parent before it
 * is taken as drawn. Then forget the list.
 * RECOMP_HALO2_REPOSE_REBASE=0: every node taken as drawn (test). */
static Obj *obj_by_nodes(uint32_t va);
static int s_rk_rebase = -1;
static uint32_t s_rk_rebased;
static void repose_adopt(void)
{
    uint32_t i, j;
    if (s_rk_rebase < 0) {
        const char *e = getenv("RECOMP_HALO2_REPOSE_REBASE");
        s_rk_rebase = !(e && e[0] == '0');
    }
    for (i = 0; i < s_rk_n; i++) {
        const uint8_t *before = s_rk_buf + s_rk[i].at, *after = before + s_rk[i].len;
        uint8_t *g = (uint8_t *)XBOX_PTR(s_rk[i].va);
        uint32_t nn = s_rk[i].len / NODE_BYTES;
        int memonly = s_rk_adopt[i] == 2;
        Obj *o;
        if (!(o = obj_by_nodes(s_rk[i].va)) || !o->cur)
            continue;
        for (j = 0; j < nn && 3 + (j + 1) * NODE_FLOATS <= o->n; j++) {
            float *c = o->cur + 3 + j * NODE_FLOATS, r[NODE_FLOATS];
            uint32_t p = o->parent ? o->parent[j] : 0;
            if (!memcmp(before + j * NODE_BYTES, after + j * NODE_BYTES, NODE_BYTES) ||
                memcmp(g + j * NODE_BYTES, after + j * NODE_BYTES, NODE_BYTES))
                continue;                           /* not re-posed, or changed by the game since */
            if (s_rk_rebase && p && p - 1u < j) {
                /* the parent as the game saw it (after its own re-pose), and its own pose: the
                 * blend's, or in memory-only mode the game's (moved already when re-posed) */
                rebase_node((const float *)(after + (p - 1u) * NODE_BYTES),
                            memonly ? (const float *)(g + (p - 1u) * NODE_BYTES) : o->cur + 3 + (p - 1u) * NODE_FLOATS,
                            (const float *)(after + j * NODE_BYTES), r);
                if (s_rp_nodelog > 0) {
                    float dx = r[10] - c[10], dy = r[11] - c[11], dz = r[12] - c[12];
                    static unsigned rbl;
                    if (dx * dx + dy * dy + dz * dz > 0.03f * 0.03f && rbl++ < 20000u)
                        fprintf(stderr, "[RB] frame %u tick %u tag %08X node %u parent %u moved %.3f %.3f %.3f%s\n", s_frame,
                                MEM32(GAME_TICK_COUNTER), MEM32(o->data), j, p - 1u, dx, dy, dz, memonly ? " (memory)" : "");
                }
                memcpy(g + j * NODE_BYTES, r, NODE_BYTES);
                if (!memonly)
                    memcpy(c, r, NODE_BYTES);
                s_rk_rebased++;
            } else if (!memonly)
                memcpy(c, g + j * NODE_BYTES, NODE_BYTES);
            s_rp_adopted++;
        }
    }
    s_rk_n = 0;
    s_rk_bytes = 0;
}

/* Patch 15 calls this around sub_0002B970: after = 0 before it (snapshot every object's node
 * block), 1 after it (note the blocks it changed). */
void h2_interp_view_objects(int after)
{
    uint32_t hdr, max, entries, i;
    if (s_rp_rule < 0) {
        const char *e = getenv("RECOMP_HALO2_REPOSE");
        s_rp_rule = e && *e ? (e[0] == '1' ? 1 : 0) : 2;   /* 2: follow smooth motion */
    }
    if (!s_rp_rule || (s_rp_rule == 2 && s_on <= 0))
        return;
    if (after) {
        static unsigned hist[8];
        if (!s_rp_active)
            return;
        s_rp_active = 0;
        for (i = 0; i < s_rp_n; i++) {
            uint8_t *g = (uint8_t *)XBOX_PTR(s_rp[i].va);
            const float *a = (const float *)g, *b = (const float *)(s_rp_buf + s_rp[i].at);
            uint32_t j, nn = s_rp[i].len / NODE_BYTES, jm = 0, nch = 0;
            float most = 0.0f;
            int bucket;
            if (!memcmp(g, b, s_rp[i].len))
                continue;                           /* not re-posed */
            if (s_rp_dumptag) {   /* test: what the re-pose does to one tag's nodes lo..hi */
                Obj *ob = obj_by_nodes(s_rp[i].va);
                static unsigned dumped;
                if (ob && MEM32(ob->data) == s_rp_dumptag && dumped < 3000000u)
                    for (j = s_rp_dumplo; j < nn && j <= s_rp_dumphi; j++) {
                        const float *x = b + j * NODE_FLOATS, *y = a + j * NODE_FLOATS;
                        dumped++;
                        fprintf(stderr, "[RPD] frame %u %s va %08X node %u scale %.3f>%.3f fwd %.2f %.2f %.2f>%.2f %.2f %.2f pos %.3f %.3f %.3f>%.3f %.3f %.3f\n",
                                s_frame, s_rp_tickview ? "T" : "N", s_rp[i].va, j, x[0], y[0], x[1], x[2], x[3], y[1], y[2], y[3],
                                x[10], x[11], x[12], y[10], y[11], y[12]);
                        if (ob->cur && 3 + (j + 1) * NODE_FLOATS <= ob->n) {   /* the blend's pose against the drawn one */
                            const float *c = ob->cur + 3 + j * NODE_FLOATS;
                            fprintf(stderr, "[RPO] frame %u node %u slide %.3f %.3f %.3f tickminusshown %.3f %.3f %.3f obj %.3f %.3f %.3f\n",
                                    s_frame, j, y[10] - x[10], y[11] - x[11], y[12] - x[12],
                                    c[10] - x[10], c[11] - x[11], c[12] - x[12],
                                    ob->cur[0] - getf(ob->data + OBJECT_POSITION), ob->cur[1] - getf(ob->data + OBJECT_POSITION + 4),
                                    ob->cur[2] - getf(ob->data + OBJECT_POSITION + 8));
                        }
                    }
            }
            for (j = 0; j < nn; j++) {   /* how far the re-pose moved the block's nodes (world positions) */
                float dx = a[j * NODE_FLOATS + 10] - b[j * NODE_FLOATS + 10];
                float dy = a[j * NODE_FLOATS + 11] - b[j * NODE_FLOATS + 11];
                float dz = a[j * NODE_FLOATS + 12] - b[j * NODE_FLOATS + 12];
                float d = dx * dx + dy * dy + dz * dz;
                if (d > most) { most = d; jm = j; }
                nch += memcmp(a + j * NODE_FLOATS, b + j * NODE_FLOATS, NODE_BYTES) != 0;
            }
            most = sqrtf(most);
            if (s_rp_nodelog) {   /* test: scale (float 0), axes (1..9) and position changes */
                float rs = 0.0f, ra = 0.0f;
                uint32_t js = 0, ja = 0, k;
                Obj *ob = obj_by_nodes(s_rp[i].va);
                static unsigned rlogged, logged;
                for (j = 0; j < nn; j++) {
                    float d0 = fabsf(a[j * NODE_FLOATS] - b[j * NODE_FLOATS]);
                    if (d0 > rs) { rs = d0; js = j; }
                    for (k = 1; k < 10; k++) {
                        float dk = fabsf(a[j * NODE_FLOATS + k] - b[j * NODE_FLOATS + k]);
                        if (dk > ra) { ra = dk; ja = j; }
                    }
                }
                if ((rs > 0.05f || ra > 0.3f) && rlogged++ < 100000u)
                    fprintf(stderr, "[RPR] frame %u %s va %08X tag %08X nodes %u scale %.3f->%.3f at %u axis %.3f at %u pos %.3f\n",
                            s_frame, s_rp_tickview ? "T" : "N", s_rp[i].va, ob ? MEM32(ob->data) : 0u, nn,
                            b[js * NODE_FLOATS], a[js * NODE_FLOATS], js, ra, ja, most);
                if (most > 0.02f && logged++ < 200000u)
                    fprintf(stderr, "[RPN] frame %u tick %u %s va %08X tag %08X nodes %u changed %u most %.3f at node %u from %.3f %.3f %.3f to %.3f %.3f %.3f\n",
                            s_frame, MEM32(GAME_TICK_COUNTER), s_rp_tickview ? "T" : "N", s_rp[i].va,
                            ob ? MEM32(ob->data) : 0u, nn, nch, most, jm,
                            b[jm * NODE_FLOATS + 10], b[jm * NODE_FLOATS + 11], b[jm * NODE_FLOATS + 12],
                            a[jm * NODE_FLOATS + 10], a[jm * NODE_FLOATS + 11], a[jm * NODE_FLOATS + 12]);
            }
            bucket = most < 0.001f ? 0 : most < 0.005f ? 1 : most < 0.01f ? 2 : most < 0.02f ? 3 :
                     most < 0.05f ? 4 : most < 0.1f ? 5 : most < 0.5f ? 6 : 7;
            hist[bucket]++;
            if (s_rp_tickview)
                continue;                           /* a tick's own view: as the Xbox drew it (logs only) */
            s_rp_changed++;
            repose_keep_note(s_rp[i].va, s_rp_buf + s_rp[i].at, s_rp[i].len, s_rp_blendtick ? 2 : 1);
        }
        if (s_rp_tickview)
            return;
        if (s_rp_frames % 600 == 599 && getenv("RECOMP_HALO2_REPOSE_LOG")) {
            fprintf(stderr, "[REPOSE] moved by <.001 %u <.005 %u <.01 %u <.02 %u <.05 %u <.1 %u <.5 %u more %u\n",
                    hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
            memset(hist, 0, sizeof hist);
        }
        if (++s_rp_frames % 600 == 0 && getenv("RECOMP_HALO2_REPOSE_LOG")) {
            fprintf(stderr, "[REPOSE] last 600 drawn views: %u node blocks re-posed, %u nodes adopted, %u of them rebased\n",
                    s_rp_changed, s_rp_adopted, s_rk_rebased);
            s_rp_changed = s_rp_adopted = s_rk_rebased = 0;
        }
        return;
    }
    {
        uint32_t t = MEM32(GAME_TICK_COUNTER);
        int newtick = t != s_rp_last_tick;
        static int byblend = -1, memonly = -1;
        s_rp_last_tick = t;
        if (s_rp_nodelog < 0) {
            const char *e = getenv("RECOMP_HALO2_REPOSE_DUMPTAG");
            s_rp_nodelog = getenv("RECOMP_HALO2_REPOSE_NODELOG") != NULL;
            if (e && *e) {
                const char *r = getenv("RECOMP_HALO2_REPOSE_DUMPNODES");
                s_rp_dumptag = (uint32_t)strtoul(e, NULL, 16);
                if (r && *r)
                    sscanf(r, "%u-%u", &s_rp_dumplo, &s_rp_dumphi);
            }
            e = getenv("RECOMP_HALO2_REPOSE_BLENDVIEW");
            byblend = !(e && e[0] == '0');
            e = getenv("RECOMP_HALO2_REPOSE_MEMONLY");
            memonly = !(e && e[0] == '0');
        }
        /* A tick's own view is one the blend left alone. When two ticks land on back-to-back
         * frames the blend still draws objects between poses on the second (common on a test laptop):
         * left alone, the re-pose put a pilot's face where the blend drew it and the next frame
         * drew it behind his head, a one-frame slab (2026-11-26, Outskirts Pelican). Such a view
         * is guarded too, but its re-pose goes onto the game's pose in the game's memory only:
         * the blend has just read the new update (taken into it as well, the pilot jumped for a
         * frame). RECOMP_HALO2_REPOSE_BLENDVIEW=0: by the tick counter alone; MEMONLY=0: into
         * both (tests). */
        s_rp_tickview = newtick && !(byblend && s_objs_moved);
        s_rp_blendtick = newtick && !s_rp_tickview && memonly;
        if (s_rp_tickview && !s_rp_nodelog && !s_rp_dumptag)
            return;                                 /* a tick's own view: as the Xbox drew it */
    }
    hdr = MEM32(OBJECT_ARRAY_PTR);
    if (hdr < 0x80000000u || hdr >= GUEST_HIGH_END || MEM32(hdr + 0x2C) != DATA_ARRAY_SIG)
        return;
    max = MEM32(hdr + ARRAY_MAX) & 0xFFFFu;
    entries = MEM32(hdr + ARRAY_DATA);
    if (max > MAX_OBJECTS)
        max = MAX_OBJECTS;
    s_rp_n = 0;
    s_rp_bytes = 0;
    for (i = 0; i < max; i++) {
        uint32_t e = entries + i * 12u, data = MEM32(e + 8), block, size, off;
        if (!(MEM32(e) & 0xFFFFu) || data < 0x80000000u || data >= GUEST_HIGH_END)
            continue;
        block = MEM32(data + OBJECT_NODE_BLOCK);
        size = block & 0xFFFFu;
        off = block >> 16;
        if (!size || size % NODE_BYTES || size > MAX_NODES * NODE_BYTES || off < 0x100u || off > 0x4000u ||
            data + off + size > GUEST_HIGH_END)
            continue;
        if (s_rp_n == s_rp_cap) {
            uint32_t cap = s_rp_cap ? s_rp_cap * 2 : 256;
            ReposeSnap *r = (ReposeSnap *)realloc(s_rp, cap * sizeof *r);
            if (!r)
                return;
            s_rp = r;
            s_rp_cap = cap;
        }
        if (s_rp_bytes + size > s_rp_bufcap) {
            uint32_t cap = (s_rp_bytes + size) * 2 + 65536;
            uint8_t *b = (uint8_t *)realloc(s_rp_buf, cap);
            if (!b)
                return;
            s_rp_buf = b;
            s_rp_bufcap = cap;
        }
        s_rp[s_rp_n].va = data + off;
        s_rp[s_rp_n].len = size;
        s_rp[s_rp_n].at = s_rp_bytes;
        memcpy(s_rp_buf + s_rp_bytes, XBOX_PTR(data + off), size);
        s_rp_bytes += size;
        s_rp_n++;
    }
    s_rp_active = 1;
}

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
        if (!s_env_probe_late)
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
static uint32_t s_cam_used_frame[MAX_VIEWS];   /* the frame each slot last served a view */
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
    phase_probe('C');
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
        /* A tiny change the frame after an update is no update (2026-11-25, Outskirts
         * intro: the direction stepped with the updates and also moved by float
         * noise in between, so it looked as if it moved every frame and was never
         * blended -- the view turned in 30 Hz steps). Take the value, keep the
         * update's timing. RECOMP_HALO2_INTERP_CAMMINOR=0 counts it (test). */
        if (held == 1 && step < 0.15f * t->last_step && s_cam_minor_rule) {
            memcpy(t->cur, raw, n * sizeof(float));
            goto blend;
        }
        /* A cut between two cutscene shots (seen 2026-11-24 in the Arbiter intro:
         * a held shot, then half a unit and a big turn in one update, and the frame
         * between showed a view from neither shot): a change after the camera held
         * still for more than two updates, or far bigger than its last step. A
         * real move only loses the blend of its first step. */
        if (held > 4)
            t->step = 0.0f;                         /* held still: no recent steps */
        t->cut = s_cam_cut_rule && (held > 4 || step > 4.0f * t->step + (n == 3 ? 0.02f : 0.0005f));
        if (s_env_camlog) {   /* test: every camera change */
            static int logged;
            if (logged++ < 40000)
                fprintf(stderr, "[CAM] frame %u view %d %s held %u step %.5f ref %.5f cut %d\n", s_frame, s_cam_index - 1,
                        n == 3 ? "pos" : "rot", held, step, t->step, t->cut);
        }
        t->last_step = step;
        t->step = step > t->step * 0.8f ? step : t->step * 0.8f;   /* the largest recent step, fading */
        /* held for a frame before this change -- or, already stepping, a change that came
         * with a new game tick: updates land on back-to-back frames when the frame rate
         * wobbles (2026-11-25, a test laptop at 14-35 ms a frame), and treating that as a camera
         * moving every frame drew it unblended for a frame while the objects were blended:
         * a one-frame jump of the whole view. RECOMP_HALO2_INTERP_CAMTICK=0 turns it off (test). */
        t->stepping = held >= 2 || (s_camtick_rule && t->stepping && s_frame - s_tick_frame <= 1);
        t->changed_frame = s_frame;
        /* from what the last frame showed, if it was still on its way */
        memcpy(t->prev, t->shown_frame == s_frame - 1 ? t->shown : t->cur, n * sizeof(float));
        memcpy(t->cur, raw, n * sizeof(float));
        t->t = now;
    } else if (s_frame - t->changed_frame > 3)
        t->stepping = 0;                            /* holding still: nothing to blend */
blend:
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
    s_cam_index++;
    {   /* The view's own history: the slot whose camera was nearest, not the slot of its
         * place in the frame's order. A view drawn only on some frames (2026-11-25,
         * a test laptop, Outskirts Pelican cockpit) moved the main view into another
         * camera's slot, and it was blended from that camera: another shot every
         * fourth frame. RECOMP_HALO2_INTERP_CAMSLOT=0 uses the order (test). */
        int slot = s_cam_index - 1, i, best = -1, freeslot = -1;
        float bd = 4.0f * 4.0f;                     /* within 4 units: the same camera */
        if (s_camslot_rule) {
            for (i = 0; i < MAX_VIEWS; i++) {
                float dd;
                if (s_cam_used_frame[i] == s_frame)
                    continue;                       /* already this frame's other view */
                if (!s_cam[i].pos.valid) {
                    if (freeslot < 0) freeslot = i;
                    continue;
                }
                dd = dist2(s_cam[i].pos.cur, raw);
                if (dd < bd) { bd = dd; best = i; }
            }
            if (best < 0) {                         /* a new camera: a free slot, else the least recent */
                best = freeslot;
                if (best < 0) {
                    uint32_t oldest = 0xFFFFFFFFu;
                    for (i = 0; i < MAX_VIEWS; i++)
                        if (s_cam_used_frame[i] != s_frame && s_cam_used_frame[i] <= oldest) { oldest = s_cam_used_frame[i]; best = i; }
                }
            }
            if (best >= 0)
                slot = best;
        }
        s_cam_used_frame[slot] = s_frame;
        c = &s_cam[slot];
        if (s_env_camlog) {   /* test: which history each view took */
            static int vlogged;
            if (vlogged++ < 40000)
                fprintf(stderr, "[CAMV] frame %u view %d slot %d va %08X pos %.3f %.3f %.3f fwd %.3f %.3f %.3f\n", s_frame,
                        s_cam_index - 1, slot, va, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
        }
    }
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
            if (s_env_objlog)
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
    phase_probe('V');
    if (s_on <= 0 || s_probe > 0 || s_trackonly || camera < 0x10000u || camera >= GUEST_HIGH_END)
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
    phase_probe('F');
    uint32_t si, j;
    int k;
    if (s_on <= 0 || s_probe > 0 || s_trackonly || !s_fp_rule || s_fp_frame == s_frame)
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
            t->stepping = held >= 2 || (s_camtick_rule && t->stepping && s_frame - s_tick_frame <= 1);   /* as the camera */
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

/* Is a cutscene showing its letterbox bars? As retail 0x13ccf1 decides it: the game options
 * (TU15 0x4F3218; flag +1, index +2, state +8 == 3) or the byte at 0x4922E2 clear, then the
 * amount at the record 0x51DF50 points to. For the widescreen cutscene view (settings.c). */
static void cutscene_letterbox_check(void)
{
    uint32_t opts = MEM32(0x4F3218u), rec = MEM32(0x51DF50u);
    int cond = 0;
    float amount = 0.0f;
    if (opts >= 0x10000u && opts < GUEST_HIGH_END)
        cond = MEM8(opts + 1) && (int16_t)MEM16(opts + 2) != -1 && MEM32(opts + 8) == 3;
    if (!MEM8(0x4922E2u))
        cond = 1;
    /* The game narrows its camera for a cutscene only while the record's byte +5 is set
     * (retail 0x13ccf0, loc_0002FC76); the bars can stay up after it clears (a level's opening
     * title, The Armory's "One Size Fits All") over the player's own view, which is not cropped.
     * The main menu (game options state 3) sets it for its background: not cropped either. */
    if (cond && rec >= 0x10000u && rec < GUEST_HIGH_END && MEM8(rec + 5) &&
        !(opts >= 0x10000u && opts < GUEST_HIGH_END && MEM32(opts + 8) == 3))
        amount = getf(rec);
    static int lblog = -1;
    static float lb_last;
    if (lblog < 0)
        lblog = getenv("RECOMP_HALO2_LBLOG") != NULL;
    if (lblog && rec >= 0x10000u && rec < GUEST_HIGH_END) {   /* test: the cutscene record's bytes when they change */
        static uint8_t last[32];
        uint8_t now[32];
        int k;
        for (k = 0; k < 32; k++)
            now[k] = MEM8(rec + k);
        static int last_cond = -1;
        if (memcmp(now, last, 32) || cond != last_cond) {
            last_cond = cond;
            fprintf(stderr, "[LBREC] %lu ms cond %d 4922E2 %02X opts %08X [%02X %04X %08X] amount %.3f |", (unsigned long)GetTickCount(),
                    cond, MEM8(0x4922E2u), opts, opts >= 0x10000u && opts < GUEST_HIGH_END ? MEM8(opts + 1) : 0xFF,
                    opts >= 0x10000u && opts < GUEST_HIGH_END ? MEM16(opts + 2) : 0xFFFF,
                    opts >= 0x10000u && opts < GUEST_HIGH_END ? MEM32(opts + 8) : 0xFFFFFFFFu, amount);
            for (k = 0; k < 32; k++)
                fprintf(stderr, "%s%02X", k % 4 ? "" : " ", now[k]);
            fprintf(stderr, "\n");
            memcpy(last, now, 32);
        }
    }
    if (lblog && amount != lb_last) {    /* RECOMP_HALO2_LBLOG=1 (test): the bar amount over time */
        fprintf(stderr, "[LB] %lu ms amount %.3f\n", (unsigned long)GetTickCount(), amount);
        if (amount == 0.0f && getenv("RECOMP_CLIP_TRIGGER")) {  /* test: a clip of the bars leaving */
            FILE *t = fopen(getenv("RECOMP_CLIP_TRIGGER"), "w");
            if (t) { fputs("x", t); fclose(t); }
        }
        lb_last = amount;
    }
    halo2_cutscene_view(amount);
}

void h2_interp_render_begin(void)
{
    phase_probe('D');
    cutscene_letterbox_check();
    if (!interp_enabled())
        return;
    if (s_probe > 0 && s_env_probe_late)
        probe_frame();          /* RECOMP_HALO2_INTERP_PROBE_LATE=1: after the per-frame update, just before the draw */
    trace_point('D');
    s_frame++;
    {   /* when the game's tick counter last changed: an update's time is its tick's */
        uint32_t t = MEM32(GAME_TICK_COUNTER);
        if (t != s_tick_val) {
            s_tick_val = t;
            s_tick_time = now_seconds();
            s_tick_frame = s_frame;
        }
    }
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
    phase_probe('E');
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
        repose_adopt();
        restore_points();
        restore_camera();
        restore_first_person();
        s_dbg_cost += now_seconds() - t0;
    }
    probe_restore();
    if (++s_dbg_frames % 600 == 0) {
        fprintf(stderr, "[INTERP] last 600 frames: %u object frames, %u blended, %u restored, %u kept by the game,"
                        " %u snaps, %u every-frame nodes, %u every-frame-only changes (%u by the skipped-frame rule, %u second poses in a tick, %u timed from their tick, %u mid-frame updates, %u noise-only node changes), %u new, %u changed, %u changed late, rejected %u/%u/%u/%u; camera %u blended, %u at a body, %u cuts; first person %u blended, %u cuts; particles %u, %u blended; %.1f us a frame\n",
                s_dbg_objects, s_dbg_blended, s_dbg_restored, s_dbg_kept, s_dbg_snap, s_dbg_everyframe, s_dbg_live_only, s_dbg_every_kept, s_dbg_same_tick, s_dbg_late_seen, s_dbg_midframe, s_dbg_noise, s_dbg_new, s_dbg_changed, s_dbg_late, s_dbg_rej[0], s_dbg_rej[1], s_dbg_rej[2], s_dbg_rej[3],
                s_dbg_cam_blends, s_dbg_cam_fp, s_dbg_cam_cuts, s_dbg_fp, s_dbg_fp_cuts, s_dbg_points, s_dbg_points_blended, s_dbg_cost * 1e6 / 600.0);
        s_dbg_points = s_dbg_points_blended = 0;
        clip_note_reset();
        s_dbg_cost = 0;
        s_dbg_objects = s_dbg_blended = s_dbg_restored = s_dbg_kept = s_dbg_snap = s_dbg_new = s_dbg_changed = s_dbg_late = s_dbg_everyframe = s_dbg_live_only = s_dbg_every_kept = s_dbg_same_tick = s_dbg_late_seen = s_dbg_midframe = s_dbg_noise = 0;
        memset(s_dbg_rej, 0, sizeof s_dbg_rej);
        s_dbg_cam_blends = s_dbg_cam_fp = s_dbg_cam_cuts = s_dbg_fp = s_dbg_fp_cuts = 0;
    }
}

/* A smooth-motion switch from the settings (h2_interp_set_enabled), at the start of a
 * frame's update hook: the frame before has put the game's own values back already.
 * Turned on, it starts with no history (objects, camera, particles, first person),
 * so the first blend is from what the game has now. */
static void interp_switch_pending(void)
{
    LONG w = InterlockedExchange(&s_want, -1);
    uint32_t i, a;
    if (w < 0 || s_on < 0 || s_override >= 0 || w == s_on)
        return;
    if (w) {
        for (i = 0; i < MAX_OBJECTS; i++)
            if (s_obj[i].handle) obj_forget(&s_obj[i]);
        memset(s_cam, 0, sizeof s_cam);
        memset(s_cam_used_frame, 0, sizeof s_cam_used_frame);
        for (a = 0; a < POINT_ARRAYS; a++)
            if (s_points[a]) memset(s_points[a], 0, s_point_arrays[a].max * sizeof(PointTrack));
        memset(s_fp, 0, sizeof s_fp);
        s_cam_moved = 0;
    }
    s_on = (int)w;
    fprintf(stderr, "[INTERP] smooth motion %s (setting changed)\n", w ? "on" : "off");
}
