/*
 * FF13-SpeedrunPractice — speedrun practice mod for FINAL FANTASY XIII (Steam).
 * Copyright (C) 2026 Akiyoshi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "srp_internal.h"

// ============================ WARP (record a spot / warp the leader to it) ===
// AnywhereSave owns the inline hooks at 0xD6D2D0 / 0x7982C0 / 0x61CBAD / 0x61CE3E and ff13_hook's
// memcmp guard is per-address, so those must NOT be re-hooked here. Instead the field leader is
// CAPTURED at GetWorldPos (0x78EEE0) and the committed position OVERRIDDEN at the per-object commit
// method (0x79C5E0), both AnywhereSave-free, by overwriting the source vector it commits from.
// All addresses are RVAs (VA - 0x400000).
#define VT_CHARA        0x10C934C      // character/actor class vtable (reject non-character reads)
const uint8_t P_TP6[6] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0}; // shared prologue of both entries

// ---- pool + cluster (ported from AnywhereSave's cluster_restore; duplicated on purpose) --------
// One shared transform pool: base = *(*([SceneObj+0x2C]+0xC)); a character's slot is
// base + [[SceneObj+0x2C]+8]*0x70, {X,Y,Z,W} floats at +0/+4/+8/+0xC.
static volatile uint32_t g_poolBase = 0;
static void pool_capture_base(uint32_t ecx) {                 // ecx = field SceneObject
    if (g_poolBase) return;
    uint32_t sh = *(uint32_t*)(uintptr_t)(ecx + 0x2C);
    if (!sh || IsBadReadPtr((void*)(uintptr_t)(sh + 0x0C), 4)) return;
    uint32_t holder = *(uint32_t*)(uintptr_t)(sh + 0x0C);
    if (!holder || IsBadReadPtr((void*)(uintptr_t)holder, 4)) return;
    g_poolBase = *(uint32_t*)(uintptr_t)holder;
}
void __cdecl on_getworldpos(uint32_t ecx) {          // ff13_install_ecx_hook (pool-base capture)
    if (*(uint32_t*)(uintptr_t)ecx == VT_CHARA) pool_capture_base(ecx);
}

// Warp override: while active, force the locked leader slot's committed position to (g_ovrX,Y,Z).
// g_candFlags[idx] is set by warp_flag_cluster for the party cluster. This is the ONLY behavior-
// changing hook; it also lazily captures the pool base (0x79C5E0 fires once/frame/character).
// Leader discriminator: the leader's world position EQUALS the party reference (pool slot 2/0),
// while save points, treasures and allies -- all of which share the character vtable and sit in the
// cluster -- do not. The slot index is locked on the first frame, before it starts moving.
#define WARP_LOCK_TOL 1.0f
volatile uint32_t g_warpActive = 0;
volatile float g_ovrX = 0, g_ovrY = 0, g_ovrZ = 0;
volatile float g_refX = 0, g_refY = 0, g_refZ = 0;   // party/leader reference captured at the warp
volatile uint32_t g_leaderIdx = 0xFFFFFFFF;          // locked leader slot index (once matched)
volatile uint32_t g_warpMoved = 0, g_warpSkip = 0;       // moved (leader) vs skipped (others) commits
// Slots the lock must not take: a staged tableau statue can stand AT the reference position (a
// checkpoint-restored 7050 field), win the first-match lock and soak the override while the real
// leader's commits are skipped. Filled by the way-back's retry when a "moved" warp moved nobody.
#define WARP_LOCK_SKIP_MAX 4
static volatile uint32_t g_lockSkip[WARP_LOCK_SKIP_MAX];
static volatile int      g_lockSkipN = 0;
// Widened per retry: with a statue excluded the real leader can sit a few units off the slot-2/0
// reference. A wrong wider lock moves nobody and gets excluded in turn.
static volatile float    g_lockTol = WARP_LOCK_TOL;
static int lock_skipped(uint32_t idx) {
    for (int i = 0; i < g_lockSkipN && i < WARP_LOCK_SKIP_MAX; i++)
        if (g_lockSkip[i] == idx) return 1;
    return 0;
}
#ifdef FF13_DIAG
// Where everything in a picked fight actually stands: this commit runs once per frame per actor and
// carries the world position it is about to write, so one frame of it is a snapshot of the arena.
// Armed a few seconds into a picked fight (after the intro camera has finished) for one frame.
static void pos_snapshot(uint32_t ecx, float* src) {
    if (!g_posSnap || (int)(GetTickCount() - g_posSnap) >= 0) return;
    if (g_posN >= 40 || !src || IsBadReadPtr(src, 16) || IsBadReadPtr((void*)(uintptr_t)ecx, 0x30))
        return;
    uint32_t vt = *(uint32_t*)(uintptr_t)ecx;
    uint32_t sh = *(uint32_t*)(uintptr_t)(ecx + 0x2C);
    uint32_t idx = (sh && !IsBadReadPtr((void*)(uintptr_t)(sh + 8), 4))
                 ? *(uint32_t*)(uintptr_t)(sh + 8) : 0xFFFFFFFF;
    g_posN++;
    ff13logv("[FF13-SRP] actor vt=%08X slot=%u  x=%.2f y=%.2f z=%.2f\n",
             vt, idx, src[0], src[1], src[2]);
}
#else
#define pos_snapshot(a, b) ((void)0)
#endif

// ---- placing the boss the arena path leaves at the origin --------------------------------------
// The arena door builds the enemy and then leaves it at (0,0,0): an actor with no transform, which
// is what "invisible, still attacks, cannot be damaged" is. Only while a PICKED fight is running,
// and only for an actor whose committed position is exactly the origin -- a real position is never
// touched.

static void place_boss(uint32_t ecx, float* src) {
    if (g_bossGotMatrix) return;        // it has a place of its own now (see on_place_join)
    if (!g_placeBoss || !src || IsBadReadPtr((void*)(uintptr_t)(ecx + 0x2C), 4)) return;
    uint32_t sh = *(uint32_t*)(uintptr_t)(ecx + 0x2C);
    if (!sh || IsBadReadPtr((void*)(uintptr_t)(sh + 8), 4)) return;
    uint32_t idx = *(uint32_t*)(uintptr_t)(sh + 8);
    // Locked by slot the first time it is seen, so it keeps being placed after a real position has
    // been written into it -- without the lock it stops matching "exactly the origin" and drifts.
    for (int i = 0; i < g_bossSlotN; i++) {
        if (g_bossSlots[i] != idx) continue;
        // Held for the whole fight, which is the least wrong of what has been tried: released
        // outright the boss drifts away, on the leash it is dragged back every frame it strays and
        // reads as a teleport each time it swings. The fix is to stop placing it by hand at all
        // (relative placement -- see .logs/toolH_battleswap_impl.md).
        src[0] = g_bossSlotPos[i][0]; src[1] = g_bossSlotPos[i][1]; src[2] = g_bossSlotPos[i][2];
        return;
    }
    if (src[0] != 0.0f || src[1] != 0.0f || src[2] != 0.0f) return;   // it has a place of its own
    if (g_bossSlotN >= BOSS_SLOTS) return;
    // Adopting an actor is "it turned up with no position at all", which is true of a boss and its
    // parts as the fight is built and of nothing else afterwards -- hence the window bound, so a
    // stray commit later in a long fight cannot be swept onto the boss's spot.
    if ((int)(GetTickCount() - g_bossOpenUntil) >= 0) return;
    if (!stage_is_ours(g_arena ? g_arena->arena : NULL)) return;   // not this fight's stage yet
    float p[3];
    if (!stage_place(g_bossOx, g_bossOy, g_bossOz, p)) return;        // stage not up -- wait a frame
    // This path has no name to go on -- it is the commit hook, not the placement one -- so a scene
    // listing several enemies is served in its file's order.
    {
        float ox, oy, oz;
        arena_enemy_pick(NULL, &ox, &oy, &oz);
        if (ox != g_bossOx || oy != g_bossOy || oz != g_bossOz) stage_place(ox, oy, oz, p);
    }
    if (!g_bossSlotN) {
        // The first one is the scene's first enemy = the body, which is what the party is turned
        // to face, so it is the one kept in g_boss*.
        g_bossX = p[0]; g_bossY = p[1]; g_bossZ = p[2];
        g_stageArena = g_arena ? g_arena->arena : NULL;   // the matrix is this arena's now
#ifdef FF13_DIAG
        g_bossObj = ecx; g_bossProbeAt = GetTickCount() + 2500;
#endif
    }
    g_bossSlotPos[g_bossSlotN][0] = p[0];
    g_bossSlotPos[g_bossSlotN][1] = p[1];
    g_bossSlotPos[g_bossSlotN][2] = p[2];
    g_bossSlots[g_bossSlotN++] = idx;
    src[0] = p[0]; src[1] = p[1]; src[2] = p[2];
    if (g_placeLogged < BOSS_SLOTS) {
        g_placeLogged++;
        ff13logv("[FF13-SRP] battle picker: actor %d had no position -- slot %u placed at "
                 "%.2f %.2f %.2f and held there\n", g_bossSlotN, idx,
                 (double)p[0], (double)p[1], (double)p[2]);
    }
}

#ifdef FF13_DIAG
// Where the facing lives: sample one character's object twice a second while it stands still and
// turns on the spot, and print the floats that moved.
#define TURN_WATCH_BYTES 0x800
static volatile uint32_t g_twObj = 0;
static float   g_twPrev[TURN_WATCH_BYTES / 4];
static volatile DWORD g_twNext = 0;
static volatile int   g_twN = 0;
static volatile int   g_twHave = 0;

// Lock onto the LEADER: he is the one being turned, and the character standing at the party
// reference. Only the lock happens here -- the sampling is on the frame tick, because a character
// that has stopped moving stops committing.
static void turn_watch(uint32_t ecx, const float* pos) {
    if (g_twObj || g_twN >= 24) return;
    float cx, cy, cz;
    if (!field_party_pos(&cx, &cy, &cz)) return;
    float dx = pos[0] - cx, dz = pos[2] - cz;
    if (dx * dx + dz * dz > 2.25f) return;            // not the one at the reference
    if (IsBadReadPtr((void*)(uintptr_t)(ecx + CHARA_NAME_OFF), 4)) return;
    uint32_t pn = *(uint32_t*)(uintptr_t)(ecx + CHARA_NAME_OFF);
    if (pn <= 0x10000 || IsBadStringPtrA((const char*)(uintptr_t)pn, 24)) return;
    if (chara_id_of((const char*)(uintptr_t)pn) > 5) return;
    g_twObj = ecx;
    ff13logv("[FF13-SRP] turn watch: following the leader \"%.16s\" @%08X -- stand still and turn\n",
             (const char*)(uintptr_t)pn, (unsigned)ecx);
}

void turn_watch_tick(void) {
    DWORD now = GetTickCount();
    uint32_t ecx = g_twObj;
    if (!ecx || g_twN >= 24 || (int)(now - g_twNext) < 0) return;
    if (IsBadReadPtr((void*)(uintptr_t)ecx, TURN_WATCH_BYTES)) return;
    g_twNext = now + 500;
    const float* cur = (const float*)(uintptr_t)ecx;
    if (!g_twHave) {
        memcpy(g_twPrev, cur, TURN_WATCH_BYTES);
        g_twHave = 1;
        return;
    }
    char line[900]; int o = 0;
    o += snprintf(line + o, sizeof line - o, "[FF13-SRP] turn watch:");
    int n = 0;
    for (int k = 0; k < TURN_WATCH_BYTES / 4 && o < (int)sizeof line - 40; k++) {
        float a = g_twPrev[k], b = cur[k];
        if (a != a || b != b) continue;
        if (a > 1e5f || a < -1e5f || b > 1e5f || b < -1e5f) continue;
        float d = b - a; if (d < 0) d = -d;
        if (d < 0.002f) continue;                     // unchanged
        if (b > 3.3f || b < -3.3f) continue;          // an angle or a cosine, nothing bigger
        o += snprintf(line + o, sizeof line - o, "  +%03X %.3f->%.3f", k * 4, (double)a, (double)b);
        n++;
    }
    memcpy(g_twPrev, cur, TURN_WATCH_BYTES);
    if (!n) return;
    g_twN++;
    ff13logv("%s\n", line);
}
#else
#define turn_watch(a, b) ((void)0)
#endif

void __cdecl on_commit(uint32_t ecx, float* src) {   // ff13_install_ecx_stackarg_hook(...,4)
    pos_snapshot(ecx, src);
    place_boss(ecx, src);
    if (*(uint32_t*)(uintptr_t)ecx != VT_CHARA) return;     // character receivers only
    turn_watch(ecx, src);
    uint32_t sh = *(uint32_t*)(uintptr_t)(ecx + 0x2C);
    if (!sh || IsBadReadPtr((void*)(uintptr_t)sh, 0x10)) return;
    if (!g_poolBase) pool_capture_base(ecx);
    // Only while the party is being walked onto the arena and through the first moments of the
    // fight, which is all _init needs. Holding it any longer pins them and they cannot walk.
    if (src && (int)(GetTickCount() - g_apUntil) < 0) {
        uint32_t i2 = *(uint32_t*)(uintptr_t)(sh + 0x08);
        arena_collect(ecx, i2, src);
        if (!g_apReady) goto not_staged;
        for (int i = 0; i < g_apN && i < 3; i++) if (g_apReady && g_apIdx[i] == i2) {
            src[0] = g_apPos[i][0]; src[1] = g_apPos[i][1]; src[2] = g_apPos[i][2];
            return;
        }
    }
not_staged:;
    if (!g_warpActive || !src) return;
    uint32_t idx = *(uint32_t*)(uintptr_t)(sh + 0x08);
    if (idx >= WARP_MAXSLOT || !g_candFlags[idx]) return;     // only the flagged party cluster
    // Lock the leader = the flagged object at the reference position, on the first match and before
    // anything is moved, so the index stays valid for the whole window.
    if (g_leaderIdx == 0xFFFFFFFF) {
        float dx=src[0]-g_refX, dy=src[1]-g_refY, dz=src[2]-g_refZ;
        if (dx<0)dx=-dx; if (dy<0)dy=-dy; if (dz<0)dz=-dz;
        float tol = g_lockTol;
        if (dx<tol && dy<tol && dz<tol && !lock_skipped(idx))
            g_leaderIdx = idx;
    }
    if (idx != g_leaderIdx) { g_warpSkip++; return; }         // move ONLY the leader
    src[0] = g_ovrX; src[1] = g_ovrY; src[2] = g_ovrZ;      // override the leader's committed position
    g_warpMoved++;
}

// Read pool slot i's {X,Y,Z(,W)} (guarded). Returns 0 on a bad read.
static int pool_read_slot(uint32_t pb, uint32_t i, float* x, float* y, float* z, float* w) {
    uintptr_t a = pb + (uintptr_t)i * 0x70;
    if (IsBadReadPtr((void*)a, 0x10)) return 0;
    *x = *(float*)a; *y = *(float*)(a+4); *z = *(float*)(a+8);
    if (w) *w = *(float*)(a+0xC);
    return 1;
}
// Live leader/party reference position: slot 2 then slot 0 (AnywhereSave's proven, field-valid choice).
int field_party_pos(float* x, float* y, float* z) {
    uint32_t pb = g_poolBase; if (!pb) return 0;
    if (pool_read_slot(pb, 2, x, y, z, 0) && !(*x==0 && *y==0 && *z==0)) return 1;
    if (pool_read_slot(pb, 0, x, y, z, 0) && !(*x==0 && *y==0 && *z==0)) return 1;
    return 0;
}
void arena_clear_flags(void) { memset((void*)g_candFlags, 0, sizeof g_candFlags); }

#define RVA_CAMCTRL_PTR 0x23FF554     // VA 0x27FF554 -> FieldCameraController
const uint8_t P_CAMREC[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x38};
volatile int g_camRecOk = 0;

// The follow caches: three Vector3 in the camera controller stay frozen at the OLD leader position
// when the leader is moved by writing his slot, so the view glides in from where he was.
// Recentring alone does not fix it -- these have to be carried by the same delta and held there for
// a moment, which is a plain position write and cannot corrupt the view.
static const uint32_t kCamFollow[3] = { 0x9C, 0x1A8, 0x1B8 };
static volatile float g_camSnap[3][3];
static volatile int   g_camSnapN = 0;
static volatile DWORD g_camSnapUntil = 0;

void camera_follow_carry(float dx, float dy, float dz) {
    g_camSnapN = 0;
    if (IsBadReadPtr((void*)(g_base + RVA_CAMCTRL_PTR), 4)) return;
    char* cc = *(char**)(g_base + RVA_CAMCTRL_PTR);
    if (!cc || IsBadReadPtr(cc, 0x1C4)) return;
    for (int i = 0; i < 3; i++) {
        const float* p = (const float*)(cc + kCamFollow[i]);
        g_camSnap[i][0] = p[0] + dx; g_camSnap[i][1] = p[1] + dy; g_camSnap[i][2] = p[2] + dz;
    }
    g_camSnapN = 3;
    g_camSnapUntil = GetTickCount() + 1200;
    ff13logv("[FF13-SRP] camera: follow caches carried by %.1f %.1f %.1f\n",
             (double)dx, (double)dy, (double)dz);
}

// Asked for from the worker thread after a warp; run here because it calls into the game.
static volatile int g_camRecDue = 0;
void camera_recentre_due(void) {
    if (!g_camRecDue) return;
    g_camRecDue = 0;
    field_camera_recenter();
}

void camera_follow_hold(void) {
    if (!g_camSnapN || (int)(GetTickCount() - g_camSnapUntil) >= 0) { g_camSnapN = 0; return; }
    if (IsBadReadPtr((void*)(g_base + RVA_CAMCTRL_PTR), 4)) return;
    char* cc = *(char**)(g_base + RVA_CAMCTRL_PTR);
    if (!cc || IsBadWritePtr(cc, 0x1C4)) return;
    for (int i = 0; i < 3; i++) {
        float* p = (float*)(cc + kCamFollow[i]);
        p[0] = g_camSnap[i][0]; p[1] = g_camSnap[i][1]; p[2] = g_camSnap[i][2];
    }
}

void field_camera_recenter(void) {
    if (!g_camRecOk || IsBadReadPtr((void*)(g_base + RVA_CAMCTRL_PTR), 4)) return;
    void* cc = *(void**)(g_base + RVA_CAMCTRL_PTR);
    if (!cc || IsBadReadPtr(cc, 0x160)) return;
    ((void (__thiscall *)(void*))(uintptr_t)(g_base + RVA_CAM_RECENTER))(cc);
}

// ---------------------------------------------------------------------------
// The side the fight opens from.
//
// Hooked at 0x5A5807, in the battle camera control's own wide-shot code (0x5A33B0, the one that
// prints "last full wide shot is invalid" from BattleCameraControl.cpp), after the fallback copy
// and on the join of both paths. The pair at +0x24C/+0x25C is NOT an eye and a target: they are the
// two SUBJECTS the wide shot frames (the party's middle and the enemy) and turning them turns the
// wrong thing -- read them, do not write them. The fields that DO decide the opening shot are the
// ones cam_control_set writes below.
#define OFF_WS_ESP   0xB28        // [esp+0xB28] there = the camera control
#define OFF_WS_A     0x24C
#define OFF_WS_B     0x25C
const uint8_t P_WIDESHOT[8] = {0xF3,0x0F,0x10,0x05,0xCC,0x05,0x6A,0x02};
// The fields the story fight's control holds, measured (see kSceneArena).
#define OFF_CC_MATRIX  0x00C     // the camera as a 4x4 -- row 2 forward, row 3 position
#define OFF_CC_EYE     0x11C     // where it is, again
#define OFF_CC_AT      0x12C     // what it looks at
#define OFF_CC_FWD     0x14C     // which way that is
#define OFF_CC_EYE0    0x274     // where it opened
#define OFF_CC_AT0     0x284     // and what it looked at there
// And the pair that actually drags the camera in; setting the opening pair above without these
// changed nothing.
#define OFF_CC_DRAG    0x294
#define OFF_CC_DRAG2   0x2A4
// The angles it rests at -- without them a correct opening swings away the moment it is let go.
// +0x1E4 is the camera's bearing round the stage origin, measured as atan2(-x,-z).
#define OFF_CC_ANGLES  0x1E0

// A scene added to the table without its camera read out would otherwise get a matrix of zeroes
// written over its own. Row 3's w is 1 in a real one.
static int arena_has_camera(const ff13_arena* a) {
    return g_arenaCamera && a && a->cm[15] == 1.0f;
}

static void cam_control_set(uint32_t obj) {
    const ff13_arena* a = g_arena;
    if (!arena_has_camera(a)) return;
    if (IsBadWritePtr((void*)(uintptr_t)obj, 0x2A0)) return;
    char* o = (char*)(uintptr_t)obj;
    memcpy(o + OFF_CC_MATRIX, a->cm, sizeof a->cm);
    memcpy(o + OFF_CC_EYE,  &a->cm[12], 12);
    memcpy(o + OFF_CC_AT,   a->ct,  12);
    memcpy(o + OFF_CC_FWD,  &a->cm[8], 12);
    memcpy(o + OFF_CC_EYE0, a->c0,  12);
    memcpy(o + OFF_CC_AT0,  a->ct0, 12);
    memcpy(o + OFF_CC_DRAG,  a->cd,  12);
    memcpy(o + OFF_CC_DRAG2, a->cd2, 12);
    memcpy(o + OFF_CC_ANGLES, a->ca,  8);
}

void __cdecl on_wide_shot(uint32_t esp) {
    if (IsBadReadPtr((void*)(uintptr_t)(esp + OFF_WS_ESP), 4)) return;
    uint32_t obj = *(uint32_t*)(uintptr_t)(esp + OFF_WS_ESP);
    if (!obj || IsBadReadPtr((void*)(uintptr_t)(obj + OFF_WS_A), 0x20)) return;
    g_camCtl = obj;
    if (g_wsSeen) return;                      // once per fight: the first shot is the opening one
    g_wsSeen = 1;
    if (!g_arena || (int)(GetTickCount() - g_arenaCamUntil) >= 0) return;   // not one of ours
#ifdef FF13_DIAG
    cam_control_dump("as found");
#endif
    if (!arena_has_camera(g_arena)) return;
    cam_control_set(obj);
    // Held for the opening rather than set once: the control's own matrix IS what gets rendered, so
    // holding it is the one thing that cannot be quietly undone. Let go after that, or the fight
    // would start with a camera nobody can turn.
    g_camHoldUntil = GetTickCount() + (DWORD)g_arena->camHoldMs;
    ff13logv("[FF13-SRP] battle camera: opening from the story fight\'s own %.2f %.2f %.2f (%s)\n",
             (double)g_arena->c0[0], (double)g_arena->c0[1], (double)g_arena->c0[2],
             g_arena->camHoldMs > 0 ? "held" : "set once, then the fight's own camera has it");
#ifdef FF13_DIAG
    cam_control_dump("after setting the camera");
#endif
}

#ifdef FF13_DIAG
// Where the camera actually IS, once a fight is under way. 0x6C0880 is the engine's own "give me
// the current camera" -- it takes an out buffer and no `this` (it fetches the render camera manager
// at [0x28007A0] itself), so it can simply be CALLED, read-only, off the frame tick.
const uint8_t P_GETCAM[6] = {0x55,0x8B,0xEC,0x8B,0x45,0x08};
volatile int   g_getCamOk = 0;
static volatile int   g_bcamN = 0;
static volatile DWORD g_bcamNext = 0;
#endif
static volatile int   g_bcamWas = 0;

void battle_cam_watch(void) {
    int in = bm_state() != 0;
    DWORD now = GetTickCount();
    if (in && !g_bcamWas) {                      // a fight just started
        g_wsSeen = 0; g_camCtl = 0;
        blackout_hold_on();
#ifdef FF13_DIAG
        g_bcamN = 0; g_bcamNext = now;
#endif
    }
    g_bcamWas = in;
    if (in && g_camCtl && g_arena && (int)(now - g_camHoldUntil) < 0) cam_control_set(g_camCtl);
#ifdef FF13_DIAG
    if (!g_getCamOk || !in || g_bcamN >= 12 || (int)(now - g_bcamNext) < 0) return;
    g_bcamNext = now + 250;
    float m[16];
    memset(m, 0, sizeof m);
    ((void (__cdecl *)(float*))(uintptr_t)(g_base + RVA_GETCAM))(m);
    ff13logv("[FF13-SRP] camera at +%dms: %.2f %.2f %.2f  (facing %.2f %.2f %.2f)\n",
             g_bcamN * 250, (double)m[12], (double)m[13], (double)m[14],
             (double)m[8], (double)m[9], (double)m[10]);
    g_bcamN++;
    if (g_bcamN == 7) cam_control_dump("settled");
#endif
}

#ifdef FF13_DIAG
// The whole control, as floats: reading a story fight's and one of ours at the same moment and
// diffing them is what found every offset above.
void cam_control_dump(const char* when) {
    uint32_t o = g_camCtl;
    if (!o || IsBadReadPtr((void*)(uintptr_t)o, 0x400)) return;
    const float* f = (const float*)(uintptr_t)o;
    ff13logv("[FF13-SRP] camera control @%08X (%s):\n", (unsigned)o, when);
    for (int i = 0; i < 0x400 / 4; i += 8) {
        ff13logv("[FF13-SRP]   +%03X %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f\n", i * 4,
                 (double)f[i],   (double)f[i+1], (double)f[i+2], (double)f[i+3],
                 (double)f[i+4], (double)f[i+5], (double)f[i+6], (double)f[i+7]);
    }
}
#endif

#ifdef FF13_DIAG
// Who ends up where: the three places a fight starts from are known, but not which of the three
// field characters the game puts in each. Prints where they stand the instant a battle starts.
// Read-only, and only in a diagnostic build.
void arena_probe_formation(void) {
    uint32_t pb = g_poolBase; if (!pb) return;
    float cx, cy, cz;
    if (!field_party_pos(&cx, &cy, &cz)) return;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)pb, &mbi, sizeof mbi)) return;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    float px[4], py[4], pz[4], pd[4];
    uint32_t pk[4];
    int n = 0;
    for (uint32_t k = 0; k < WARP_MAXSLOT; k++) {
        if (k == 0 || k == 2) continue;
        uintptr_t a = pb + (uintptr_t)k * 0x70;
        if (a + 0x10 > regionEnd) break;
        float x=*(float*)a, y=*(float*)(a+4), z=*(float*)(a+8), w=*(float*)(a+0xC);
        if (w != 1.0f) continue;
        float ay = y - cy; if (ay < 0) ay = -ay;
        if (ay > ARENA_YBAND) continue;
        float dx=x-cx, dz=z-cz; float d = dx*dx + dz*dz;
        if (d > ARENA_REACH * ARENA_REACH) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) {
            float ex=x-px[i], ez=z-pz[i];
            if (ex*ex + ez*ez < ARENA_SAME * ARENA_SAME) { dup = 1; break; }
        }
        if (dup) continue;
        int at = n < 4 ? n : 4;
        while (at > 0 && pd[at-1] > d) {
            if (at < 4) { pd[at]=pd[at-1]; px[at]=px[at-1]; py[at]=py[at-1]; pz[at]=pz[at-1];
                          pk[at]=pk[at-1]; }
            at--;
        }
        if (at < 4) { pd[at]=d; px[at]=x; py[at]=y; pz[at]=z; pk[at]=k; if (n < 4) n++; }
    }
    ff13logv("[FF13-SRP] formation probe: reference %.2f %.2f %.2f\n", cx, cy, cz);
    for (int i = 0; i < n; i++)
        ff13logv("[FF13-SRP] formation probe:   #%d slot %u at %.2f %.2f %.2f (%.1f from the "
                 "leader)\n", i, pk[i], px[i], py[i], pz[i], (double)sqrtf(pd[i]));
}
#endif

// Who stands where, decided by WHO they are rather than where they were walking: grouping by
// position collapses characters standing close together and splits one character across the
// several pool slots it owns. Each character OBJECT owns exactly one pool slot.
static void arena_assign(void) {
    if (g_apReady || g_apN < 1 || !g_arena) return;
    int n = g_apN > 3 ? 3 : g_apN;
    int lead = 0;
    for (int i = 1; i < n; i++) if (g_apLeadD[i] < g_apLeadD[lead]) lead = i;
    // Formation places: 1 = centre, 0 = left, 2 = right. Three fill all of them; a duo takes centre
    // and right (measured: the second always stands on the leader's right), one alone the centre.
    // A party of two is real -- chapter 1 fields two -- so this must never require three.
    int who[3], wpOf[3];
    who[0] = lead; wpOf[0] = 1;
    if (n == 3) {
        int a = -1, b = -1;
        for (int i = 0; i < 3; i++) { if (i == lead) continue; if (a < 0) a = i; else b = i; }
        if (g_apCharId[a] > g_apCharId[b]) { int t = a; a = b; b = t; }   // lower number on the left
        who[1] = a; wpOf[1] = 0;
        who[2] = b; wpOf[2] = 2;
    } else if (n == 2) {
        who[1] = (lead == 0) ? 1 : 0; wpOf[1] = 2;
    }
    // WHERE those places are is not settled here: an entry measured off the real fight has them
    // outright, one derived from the archive must wait for the battle to have a stage (arena_hold).
    for (int i = 0; i < n; i++) g_apWp[who[i]] = wpOf[i];
    g_apStage = arena_has_party(g_arena) ? 0 : 1;
    for (int i = 0; i < n; i++) {
        int c = who[i];
        g_apPos[c][0] = g_arena->wp[wpOf[i]][0];
        g_apPos[c][1] = g_arena->wp[wpOf[i]][1];
        g_apPos[c][2] = g_arena->wp[wpOf[i]][2];
    }
    if (n == 3)
        ff13logv("[FF13-SRP] battle picker: formation -- %.16s centre (leader), %.16s left, "
                 "%.16s right\n", g_apName[lead], g_apName[who[1]], g_apName[who[2]]);
    else if (n == 2)
        ff13logv("[FF13-SRP] battle picker: formation -- %.16s centre (leader), %.16s right "
                 "(a party of two)\n", g_apName[lead], g_apName[who[1]]);
    else
        ff13logv("[FF13-SRP] battle picker: formation -- %.16s centre, alone\n", g_apName[lead]);
    g_apReady = 1;
}

// The commit hook fires from more than one thread, so the gather must be serialised: without this
// lock, entries came out empty while the counter had moved past them and two characters were given
// the same place. One at a time; a caller that finds it busy tries again next frame.
static volatile LONG g_apLock = 0;

// Written every frame from the game thread while the staging window is open. The commit hook only
// sees a character that is moving; this reaches all three whatever they are doing.
// The character object carries an orthonormal basis -- three rows of four floats followed by a
// position -- and row 1 is the direction it looks. The offset is NOT fixed (at 0x1F0 it was a basis
// for the character being driven and uninitialised NaN for the idle ones), so it is searched for:
// a matrix whose fourth row is where this character stands and whose first two rows are unit
// length. The pool's own rotation channel is not the way in -- 0xD6D320 has no callers in retail.
#define CHARA_ROT_HI 0x1000
static uintptr_t basis_in(uintptr_t base, int len, const volatile float* at) {
    for (int off = 0; off + 0x40 <= len; off += 4) {
        const float* m = (const float*)(base + off);
        float px = m[12] - at[0], py = m[13] - at[1], pz = m[14] - at[2];
        if (px != px || py != py || pz != pz) continue;
        if (px*px + pz*pz > 36.0f || py > 4.0f || py < -4.0f) continue;   // not where it stands
        float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
        float l1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
        if (l0 < 0.95f || l0 > 1.05f || l1 < 0.95f || l1 > 1.05f) continue;
        return base + off;
    }
    return 0;
}

// Only the object's OWN memory: the basis reachable through its pointers is SHARED by the whole
// party (their +0x24 / +0x28 are identical), so writing a facing there would corrupt something
// shared. The idle characters are left unturned rather than followed into it.
static uintptr_t find_basis(uint32_t obj, const volatile float* at) {
    if (IsBadReadPtr((void*)(uintptr_t)obj, CHARA_ROT_HI)) return 0;
    return basis_in((uintptr_t)obj, CHARA_ROT_HI, at);
}

#ifdef FF13_DIAG
// Read only. Does a battle actor carry the same inline basis a field character does?
void boss_basis_probe(void) {
    if (!g_bossProbeAt || (int)(GetTickCount() - g_bossProbeAt) < 0) return;
    g_bossProbeAt = 0;
    uint32_t obj = g_bossObj;
    if (!obj || IsBadReadPtr((void*)(uintptr_t)obj, CHARA_ROT_HI)) {
        ff13logv("[FF13-SRP] boss facing probe: actor @%08X is not readable\n", (unsigned)obj);
        return;
    }
    ff13logv("[FF13-SRP] boss facing probe: actor @%08X vtable %08X (a field character's is %08X), "
             "standing at %.2f %.2f %.2f\n", (unsigned)obj, *(uint32_t*)(uintptr_t)obj,
             (unsigned)VT_CHARA, (double)g_bossX, (double)g_bossY, (double)g_bossZ);
    // Not anchored to where we PUT it: the battle never gave this actor a transform, so whatever it
    // carries still says the origin and a search keyed on the forced position cannot match. Rows
    // are taken on their own terms, and one level of pointers is followed as well (unlike the
    // field-character search). Read only throughout; every dereference is guarded.
    int found = 0;
    for (int pass = 0; pass < 2 && found < 8; pass++) {
        for (int off = 0; off + 0x40 <= 0x2000 && found < 8; off += 4) {
            uintptr_t base;
            if (pass == 0) {
                base = (uintptr_t)obj + off;
            } else {
                if (IsBadReadPtr((void*)((uintptr_t)obj + off), 4)) continue;
                uint32_t q = *(uint32_t*)((uintptr_t)obj + off);
                if (q < 0x10000 || q > 0x7FFF0000 || (q & 3)) continue;
                base = (uintptr_t)q;
            }
            if (IsBadReadPtr((void*)base, 0x40)) continue;
            const float* m = (const float*)base;
            int bad = 0;
            for (int k = 0; k < 16; k++) if (m[k] != m[k]) { bad = 1; break; }
            if (bad) continue;
            float l0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
            float l1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
            float l2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
            if (l0 < 0.98f || l0 > 1.02f || l1 < 0.98f || l1 > 1.02f) continue;
            if (l2 < 0.98f || l2 > 1.02f) continue;          // a real basis has THREE rows
            float d = m[0]*m[4] + m[1]*m[5] + m[2]*m[6];
            if (d > 0.02f || d < -0.02f) continue;
            found++;
            ff13logv("[FF13-SRP] boss facing probe: %s +%03X  row0(%.3f %.3f %.3f) "
                     "row1(%.3f %.3f %.3f) row2(%.3f %.3f %.3f) row3(%.3f %.3f %.3f)\n",
                     pass ? "via pointer at" : "inline at", (unsigned)off,
                     m[0], m[1], m[2], m[4], m[5], m[6],
                     m[8], m[9], m[10], m[12], m[13], m[14]);
        }
    }
    if (!found)
        ff13logv("[FF13-SRP] boss facing probe: no three-row basis inline or one pointer deep -- "
                 "a battle actor keeps its transform somewhere else entirely\n");
}
#endif

static void face_towards(int slot, uint32_t obj, const volatile float* from,
                         const volatile float* was, float tx, float tz) {
    if (!obj || IsBadReadPtr((void*)(uintptr_t)obj, CHARA_ROT_HI)) return;
    // The field characters are torn down when the battle takes over, and freed memory still passes
    // IsBadWritePtr -- writing into it crashed. The vtable check is the proof it is still a
    // character; the facing is also only written BEFORE the call, never across the transition.
    if (*(uint32_t*)(uintptr_t)obj != VT_CHARA) return;
    // Matched against BOTH the place it is being sent and the place it came from: the idle
    // characters still carry the position they were standing at when they were recognised, so
    // matching only the target found the leader alone.
    uintptr_t at = g_apRot[slot];
    if (!at) {
        at = find_basis(obj, from);
        if (!at && was) at = find_basis(obj, was);
        g_apRot[slot] = at;
    }
    if (!at) {
#ifdef FF13_DIAG
        if (g_faceLogN < 8) { g_faceLogN++;
            ff13logv("[FF13-SRP] facing: @%08X has no basis standing at %.2f %.2f\n",
                     (unsigned)obj, (double)from[0], (double)from[2]); }
#endif
        return;
    }
    float fx = tx - from[0], fz = tz - from[2];
    float n = sqrtf(fx * fx + fz * fz);
    if (n < 0.001f) return;
    fx /= n; fz /= n;
    float* m = (float*)at;
    m[0] =  fz; m[1] =  0.0f; m[2] = -fx; m[3] = 0.0f;     // right = forward x up
    m[4] =  fx; m[5] =  0.0f; m[6] =  fz; m[7] = 0.0f;     // forward
    m[8] = 0.0f; m[9] = -1.0f; m[10] = 0.0f; m[11] = 0.0f; // up, which points -Y in this basis
#ifdef FF13_DIAG
    if (g_faceLogN < 8) {
        g_faceLogN++;
        ff13logv("[FF13-SRP] facing: @%08X basis at %08X -> forward (%.3f, %.3f)\n",
                 (unsigned)obj, (unsigned)at, (double)fx, (double)fz);
    }
#endif
}

static volatile int g_arenaRecentred = 0;
void arena_recentre_reset(void) { g_arenaRecentred = 0; }

void arena_hold(void) {
    // Fewer than three turned up: place however many were recognised (arena_collect assigns at
    // three on its own; this is the only path a smaller party has). Must run before the second half
    // of the start -- bs_pump calls this first -- so the facing write below still sees
    // g_arenaPending set on this tick.
    if (!g_apReady && g_apN > 0 && (int)(GetTickCount() - g_arenaStartAt) >= 0
        && (int)(GetTickCount() - g_apUntil) < 0) {
        if (!InterlockedExchange(&g_apLock, 1)) {
            arena_assign();
            InterlockedExchange(&g_apLock, 0);
        }
    }
    if (!g_apReady || (int)(GetTickCount() - g_apUntil) >= 0) return;
    // ORDER MATTERS: the battle camera inherits the field camera's direction and the recenter puts
    // the field camera BEHIND THE LEADER -- so turn the leader at the boss first and recentre
    // after, never before. Done once, on the frame the party is recognised and placed.

    uint32_t pb = g_poolBase; if (!pb) return;
    // A derived entry has no world places to warp to until the battle has a stage to measure them
    // from, which is why the hold has to outlast the call for these: the party is put on the arena
    // AFTER the fight starts, not before.
    int n = g_apN > 3 ? 3 : g_apN;
    if (g_apStage) {
        float p[3];
        // Not before the fight has been asked for, and not until the matrix is this fight's own.
        if (g_arenaPending || !stage_is_ours(g_arena ? g_arena->arena : NULL)) return;
        for (int i = 0; i < n; i++) {
            const float* o = arena_party_stage(g_arena, g_apWp[i]);
            if (!stage_place(o[0], o[1], o[2], p)) return;   // stage not up -- nothing written yet
            g_apPos[i][0] = p[0]; g_apPos[i][1] = p[1]; g_apPos[i][2] = p[2];
        }
        g_apStage = 0;
        g_stageArena = g_arena ? g_arena->arena : NULL;   // the matrix is this arena's now
        g_apFace = 1;                     // turn them at the boss once, on this frame
        // The derived window is sized for a stage that is SLOW to come up; holding to the end of it
        // when the stage came up quickly pins the party seconds into the fight (they run at the
        // boss and are dragged back every tick). Once placed, only the settle is needed.
        {
            DWORD cap = GetTickCount() + ARENA_SETTLE_MS;
            if ((int)(cap - g_apUntil) < 0) g_apUntil = cap;
        }
        ff13logv("[FF13-SRP] battle picker: the stage is up -- party placed on it at "
                 "%.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f\n",
                 g_apPos[0][0], g_apPos[0][1], g_apPos[0][2],
                 g_apPos[1][0], g_apPos[1][1], g_apPos[1][2],
                 g_apPos[2][0], g_apPos[2][1], g_apPos[2][2]);
    }
    for (int i = 0; i < n; i++) {
        uintptr_t a = pb + (uintptr_t)g_apIdx[i] * 0x70;
        if (IsBadWritePtr((void*)a, 0x0C)) continue;
        *(float*)a = g_apPos[i][0]; *(float*)(a+4) = g_apPos[i][1]; *(float*)(a+8) = g_apPos[i][2];
        // Before the call only, and at the place the boss is going to stand: the stage transform
        // that gives its real position is not up until the battle is running.
        if (g_arenaPending && arena_has_party(g_arena))
            face_towards(i, g_apObj[i], g_apPos[i], g_apSrc[i],
                         g_arena->bw[0], g_arena->bw[2]);
        else if (g_apFace && g_bossSlotN)
            face_towards(i, g_apObj[i], g_apPos[i], g_apSrc[i], g_bossX, g_bossZ);
    }
    g_apFace = 0;
    if (!g_arenaRecentred && g_arenaPending) {
        g_arenaRecentred = 1;
        field_camera_recenter();                 // now that the leader is looking the right way
    }
}

void arena_collect(uint32_t ecx, uint32_t slot, const float* pos) {
    if (g_apReady || g_apN >= 3 || IsBadReadPtr((void*)(uintptr_t)ecx, CHARA_NAME_OFF + 4)) return;
    if (InterlockedExchange(&g_apLock, 1)) return;
    if (g_apReady || g_apN >= 3) { InterlockedExchange(&g_apLock, 0); return; }
    uint32_t pn = *(uint32_t*)(uintptr_t)(ecx + CHARA_NAME_OFF);
    if (pn <= 0x10000 || IsBadStringPtrA((const char*)(uintptr_t)pn, 24))
        { InterlockedExchange(&g_apLock, 0); return; }
    char nm[24];                                          // copied: the log must not race the game
    strncpy(nm, (const char*)(uintptr_t)pn, sizeof nm - 1);
    nm[sizeof nm - 1] = 0;
    int id = chara_id_of(nm);
    if (id > 5) { InterlockedExchange(&g_apLock, 0); return; }   // not one of the six
    for (int i = 0; i < g_apN; i++) if (g_apCharId[i] == id)         // already have this one
        { InterlockedExchange(&g_apLock, 0); return; }
    float dx = pos[0] - g_apRefX, dz = pos[2] - g_apRefZ;
    g_apIdx[g_apN]    = slot;
    g_apObj[g_apN]    = ecx;
    g_apCharId[g_apN] = id;
    g_apLeadD[g_apN]  = dx * dx + dz * dz;                // the leader IS the reference position
    strncpy(g_apName[g_apN], nm, sizeof g_apName[0] - 1);
    g_apName[g_apN][sizeof g_apName[0] - 1] = 0;
    g_apSrc[g_apN][0] = pos[0]; g_apSrc[g_apN][1] = pos[1]; g_apSrc[g_apN][2] = pos[2];
    ff13logv("[FF13-SRP] battle picker:   #%d %.16s (id %d) is slot %u, %.1f from the reference\n",
             g_apN, nm, id, slot, (double)sqrtf(g_apLeadD[g_apN]));
    g_apN++;
    if (g_apN == 3) arena_assign();
    InterlockedExchange(&g_apLock, 0);
}

int warp_flag_cluster(float cx, float cy, float cz) {
    uint32_t pb = g_poolBase; if (!pb) return 0;
    memset((void*)g_candFlags, 0, sizeof g_candFlags);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)pb, &mbi, sizeof mbi)) return 0;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    const float tol = 8.0f;
    int n = 0;
    for (uint32_t k = 0; k < WARP_MAXSLOT; k++) {
        uintptr_t a = pb + (uintptr_t)k * 0x70;
        if (a + 0x10 > regionEnd) break;
        float x=*(float*)a, y=*(float*)(a+4), z=*(float*)(a+8), w=*(float*)(a+0xC);
        float dx=x-cx, dy=y-cy, dz=z-cz;
        if (dx<0) dx=-dx; if (dy<0) dy=-dy; if (dz<0) dz=-dz;
        if (w==1.0f && dx<tol && dy<tol && dz<tol) { g_candFlags[k]=1; n++; }
    }
    return n;
}


// Pin the leader where they already are, for as long as a picked fight is under its cover: the warp
// override aimed at the party's own position, so "walk" resolves to standing still. Only the
// leader is pinned; the others keep following, which is how a party standing about looks.
void hold_still_begin(void) {
    float cx = 0, cy = 0, cz = 0;
    if (!field_party_pos(&cx, &cy, &cz)) return;
    g_lockSkipN = 0; g_lockTol = WARP_LOCK_TOL;
    warp_flag_cluster(cx, cy, cz);
    g_refX = cx; g_refY = cy; g_refZ = cz;
    g_leaderIdx = 0xFFFFFFFF;
    g_ovrX = cx; g_ovrY = cy; g_ovrZ = cz;
    g_warpMoved = 0; g_warpSkip = 0;
    g_warpActive = 1;
    ff13logv("[FF13-SRP] battle picker: holding the party at %.2f %.2f %.2f while the cover is up\n",
             (double)cx, (double)cy, (double)cz);
}
void hold_still_end(void) {
    if (!g_warpActive) return;
    g_warpActive = 0;
    memset((void*)g_candFlags, 0, sizeof g_candFlags);
}

// Recorded warp points -- ONE per zone, persisted next to the exe so they survive a game restart.
// Own sidecar file (NOT AnywhereSave's) -- one text line "zone x y z" per recorded zone.
typedef struct { int valid; uint32_t zone; float x, y, z; } warp_point_t;
#define WARP_MAXPOINTS 128
static warp_point_t g_warpPoints[WARP_MAXPOINTS];
static int        g_warpCount = 0;
static warp_point_t* warp_find(uint32_t zone) {                 // existing point for a zone, or NULL
    for (int i = 0; i < g_warpCount; i++)
        if (g_warpPoints[i].valid && g_warpPoints[i].zone == zone) return &g_warpPoints[i];
    return 0;
}
static warp_point_t* warp_upsert(uint32_t zone) {               // find-or-add a slot for a zone
    warp_point_t* p = warp_find(zone);
    if (p) return p;
    if (g_warpCount < WARP_MAXPOINTS) { p = &g_warpPoints[g_warpCount++]; p->valid = 1; p->zone = zone; return p; }
    return 0;                                                // table full (128 zones): drop the new one
}

// In-overlay confirmation prompt: the overlay renders these, the poll loop drives confirm/cancel.
// g_warpPrompt: 0=none, 1=record-confirm, 2=warp-confirm.
volatile int   g_warpPrompt   = 0;
char           g_warpPromptMsg[288];        // the confirm question (shown while g_warpPrompt!=0)
volatile DWORD g_warpPromptUntil = 0;       // auto-cancel deadline (GetTickCount)
static uint32_t       g_warpPendZone = 0;          // pending record values, committed on confirm
static float          g_warpPendX = 0, g_warpPendY = 0, g_warpPendZ = 0;
#define WARP_SIDECAR "ff13-srpractice_warp.dat"
static void warp_save(void) {                                // rewrite the whole table (one line/zone)
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), WARP_SIDECAR)) return;
    FILE* f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < g_warpCount; i++)
        if (g_warpPoints[i].valid)
            fprintf(f, "%u %.6f %.6f %.6f\n",
                    g_warpPoints[i].zone, g_warpPoints[i].x, g_warpPoints[i].y, g_warpPoints[i].z);
    fclose(f);
}
void warp_load(void) {                                // load every "zone x y z" line into the table
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), WARP_SIDECAR)) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    unsigned zone; float x, y, z; int n = 0;
    while (g_warpCount < WARP_MAXPOINTS && fscanf(f, "%u %f %f %f", &zone, &x, &y, &z) == 4) {
        warp_point_t* p = warp_upsert(zone);                   // upsert dedups repeated zones in the file
        if (p) { p->x = x; p->y = y; p->z = z; n++; }
    }
    fclose(f);
    if (n) ff13loga("[FF13-SRP] warp: loaded %d zone point(s)\n", n);
}

// Record where the party is standing. No question is asked -- the deliberate hold is the question.
// It still goes through the prompt fields and warp_confirm rather than duplicating the commit, so
// there is one place a point is written.
void warp_record(void) {
    if (g_warpPrompt) return;                                 // a prompt is already up
    int st = field_zone_state();
    char rkey[32]; vk_label(g_warpVk, rkey, sizeof rkey);
    ff13log("[FF13-SRP] warp: %s pressed (zone_state=%d, need 3=field, poolBase=%08X).\n",
             rkey, st, (unsigned)g_poolBase);
    if (st != 3) { flash_say(ui_text(T_WARP_FIELD_ONLY)); return; }
    float x, y, z;
    if (!field_party_pos(&x, &y, &z)) {
        ff13loga("[FF13-SRP] warp: %s party-ref read failed (poolBase=%08X).\n", rkey,
                          (unsigned)g_poolBase);
        flash_say(ui_text(T_WARP_NO_READ));
        return;
    }
    g_warpPendZone = field_zone_id(); g_warpPendX = x; g_warpPendY = y; g_warpPendZ = z;
    g_warpPrompt = 1;
    warp_confirm();
}

// The warp itself (runs on confirm). Snapshot the leader/party reference position, flag the cluster
// of pool slots at it, then hold the commit override so 0x79C5E0's per-frame commit re-points the
// leader to the target. Leader-only, so allies and co-located props stay where they are.
// BLOCKS: must run on the worker thread, never the game thread.
// The hold is measured in commits, not milliseconds: each commit only carries the leader part of
// the way (the game clamps how far a character travels in one step), and a field that is still
// streaming delivers single-figure frames. So hold until the party is THERE, clock as the give-up.
#define WARP_HOLD_MIN_MS   700     // floor: letting go early snaps back
#define WARP_HOLD_MAX_MS  6000     // a warp that has not landed by now is not going to
// "There" is a spot you can stand on, not a coordinate: the game resolves the last few units of a
// warp against the ground, so an arrival is up to ~11 units out. A tight tolerance only calls a
// perfectly good landing a failure.
#define WARP_ARRIVE_TOL     12.0f

#define WARP_STALL_MS      500     // no movement for this long, and this hop is as far as it goes
#define WARP_STALL_TOL       0.5f  // what counts as "not moving"

static DWORD warp_hold_until_there(float tx, float ty, float tz) {
    DWORD t0 = GetTickCount(), moved = t0;
    float lx = 0, ly = 0, lz = 0; int haveLast = 0;
    for (;;) {
        Sleep(16);
        DWORD now = GetTickCount(), held = now - t0;
        if (held >= WARP_HOLD_MAX_MS) return held;
        float x, y, z;
        if (!field_party_pos(&x, &y, &z)) continue;
        if (held >= WARP_HOLD_MIN_MS
            && fabsf(x - tx) < WARP_ARRIVE_TOL && fabsf(y - ty) < WARP_ARRIVE_TOL
            && fabsf(z - tz) < WARP_ARRIVE_TOL) return held;
        // Give up on a hop as soon as it stops making progress: waiting out the full hold would
        // cost the maximum per hop, and the caller can tell from where the party ended up.
        if (haveLast && (fabsf(x - lx) >= WARP_STALL_TOL || fabsf(y - ly) >= WARP_STALL_TOL
                         || fabsf(z - lz) >= WARP_STALL_TOL)) moved = now;
        lx = x; ly = y; lz = z; haveLast = 1;
        if (held >= WARP_HOLD_MIN_MS && now - moved >= WARP_STALL_MS) return held;
    }
}

// One override window: flag the cluster, hold the leader at (tx,ty,tz) until it reads back there,
// let go. Returns where the leader actually ended up.
static DWORD warp_step(float tx, float ty, float tz,
                          float* ax, float* ay, float* az, int* haveAft) {
    float cx=0, cy=0, cz=0;
    field_party_pos(&cx, &cy, &cz);
    warp_flag_cluster(cx, cy, cz);                         // flag the party-cluster slots (candidate set)
    g_refX = cx; g_refY = cy; g_refZ = cz;               // reference the leader will be matched against
    g_leaderIdx = 0xFFFFFFFF;                            // unlock; on_commit re-locks this window
    g_ovrX = tx; g_ovrY = ty; g_ovrZ = tz;
    g_warpActive = 1;
    DWORD held = warp_hold_until_there(tx, ty, tz);
    g_warpActive = 0;
    memset((void*)g_candFlags, 0, sizeof g_candFlags);
    *haveAft = field_party_pos(ax, ay, az);
    return held;
}

// HARD LIMIT, measured on hardware: one warp cannot move the party more than about 390 units,
// whatever the hold length or the frame rate. So walk there in hops under the cap, letting go
// between them -- the party is at a real position after each hop, which is what the field needs in
// order to stream the ground the next hop crosses. Y is carried in step with XZ so an intermediate
// hop does not ask for a height belonging somewhere else.
#define WARP_HOP_MAX     300.0f    // comfortably inside the observed ~390 cap
#define WARP_HOP_SETTLE_MS  350    // between hops: let the field catch up with where the party now is
#define WARP_HOPS_MAX        24    // 24 * 300 = 7200 units, larger than any zone
// A hop that never arrives costs the whole WARP_HOLD_MAX_MS, so the hop count alone is not a bound
// on how long the worker thread stays in here.
#define WARP_TRIP_MAX_MS  12000

static float dist3(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = ax-bx, dy = ay-by, dz = az-bz;
    return (float)sqrt((double)(dx*dx + dy*dy + dz*dz));
}

static int warp_to(float tx, float ty, float tz, const char* why) {
    float cx=0, cy=0, cz=0; int haveCur = field_party_pos(&cx, &cy, &cz);   // leader/party pos BEFORE
    // Already there: do nothing at all, rather than carrying the camera caches by zero and calling
    // the recentre into a field that has just come back from a checkpoint restart.
    if (haveCur && dist3(cx, cy, cz, tx, ty, tz) < WARP_ARRIVE_TOL) {
        ff13log("[FF13-SRP] warp (%s): already there (%.2f,%.2f,%.2f) -- nothing to do\n",
                 why, cx, cy, cz);
        return 1;
    }
    g_warpMoved = 0; g_warpSkip = 0;
    float ax=cx, ay=cy, az=cz; int haveAft = haveCur;
    DWORD held = 0;
    int hops = 0;
    for (; hops < WARP_HOPS_MAX && held < WARP_TRIP_MAX_MS; hops++) {
        float rest = dist3(ax, ay, az, tx, ty, tz);
        if (rest < WARP_ARRIVE_TOL) break;
        float wx = tx, wy = ty, wz = tz;
        if (rest > WARP_HOP_MAX) {                          // aim at a waypoint inside the cap
            float f = WARP_HOP_MAX / rest;
            wx = ax + (tx - ax) * f; wy = ay + (ty - ay) * f; wz = az + (tz - az) * f;
        }
        float px = ax, py = ay, pz = az;
        held += warp_step(wx, wy, wz, &ax, &ay, &az, &haveAft);
        if (!haveAft) break;
        // Stopped dead against something: another hop along the same line would only hit it again.
        if (dist3(px, py, pz, ax, ay, az) < 1.0f) { hops++; break; }
        if (dist3(ax, ay, az, tx, ty, tz) >= WARP_ARRIVE_TOL) Sleep(WARP_HOP_SETTLE_MS);
    }
    float miss = haveAft ? dist3(ax, ay, az, tx, ty, tz) : -1.0f;
    ff13log("[FF13-SRP] warp (%s): target=(%.2f,%.2f,%.2f) cur=%s(%.2f,%.2f,%.2f) "
             "after=%s(%.2f,%.2f,%.2f) [leaderIdx=%u moved=%u skipped=%u hops=%d held=%ums "
             "short=%.1f%s]\n",
             why, tx, ty, tz,
             haveCur?"":"?", cx, cy, cz, haveAft?"":"?", ax, ay, az,
             (unsigned)g_leaderIdx, (unsigned)g_warpMoved, (unsigned)g_warpSkip, hops,
             (unsigned)held, miss,
             (miss >= 0 && miss >= WARP_ARRIVE_TOL) ? " -- DID NOT ARRIVE" : "");

    // Do NOT add a "retreat if the hops fall short" path: measured on hardware, a warp cannot be
    // undone by warping (asked to step back 300 units right after arriving, the party moved <1).
    //
    // Carry the camera follow caches by the delta the party ACTUALLY ended up with, once, at the
    // end, and leave the recentre call itself to the game thread (g_camRecDue).
    if (haveCur && haveAft) {
        camera_follow_carry(ax - cx, ay - cy, az - cz);
        g_camRecDue = 1;
    }
    return haveAft && miss < WARP_ARRIVE_TOL;
}

// The way back from a picked fight, with the one question warp_to cannot answer for itself: is
// this trip possible at all? Measured here rather than at pick time because the checkpoint is not
// known until the fight is over, and a fight's chain can move the checkpoint itself.
void home_warp_back(void) {
    float cx, cy, cz;
    if (field_party_pos(&cx, &cy, &cz)) {
        float d = dist3(cx, cy, cz, g_homeX, g_homeY, g_homeZ);
        // The cap holds even with the pick spot's cells armed: the arm buys the DESTINATION's
        // ground, and an over-long warp never gets there -- it stops the party part way, on cells
        // nobody asked for. btsc12090's checkpoint is 1150 units out against a ~390 budget.
        if (d > HOME_MAX_DIST) {
            ff13logv("[FF13-SRP] battle picker: the way back is %.0f units and a warp covers about "
                     "%.0f -- not attempting it, the party stays at the checkpoint (%d cell(s) "
                     "armed, which buys ground, not range)\n", d, HOME_MAX_DIST, g_homeCellsN);
            // Deliberately not said on screen: the party can come back somewhere unexpected for
            // reasons this check never sees, so a message here would read as a promise. The README
            // covers it instead.
            return;
        }
    }
    // Ask the streamer for the pick spot's ground up front (STAGE17), so arriving on cells the
    // checkpoint did not stream cannot hang the zone in streaming state 13 (btsc07037, measured).
    if (g_homeCellsN > 0) save_arm_reconcile_sticky(g_homeCells, g_homeCellsN, 120000);
    // Retry with the locked slot excluded when a warp that DID override commits moved nobody:
    // that lock went to a statue standing at the reference, not to the leader (see g_lockSkip).
    g_lockSkipN = 0;
    int done = 0;
    for (int tries = 0; !done && tries <= WARP_LOCK_SKIP_MAX; tries++) {
        g_lockTol = (tries == 0) ? WARP_LOCK_TOL : (tries == 1) ? 4.0f : 8.0f;
        float bx=0, by=0, bz=0; int haveB = field_party_pos(&bx, &by, &bz);
        if (warp_to(g_homeX, g_homeY, g_homeZ, "back to where the fight was picked")) { done = 1; break; }
        float ex=0, ey=0, ez=0;
        if (!haveB || !field_party_pos(&ex, &ey, &ez)) break;
        if (dist3(bx, by, bz, ex, ey, ez) >= 1.0f) break;    // it moved and fell short: a wall, not
        uint32_t bad = g_leaderIdx;                          // a mislock -- retrying would re-hit it
        if (bad == 0xFFFFFFFF) {
            if (tries >= 2) break;                           // widest look found nobody; give up
            ff13logv("[FF13-SRP] battle picker: the way back locked nobody at tol %.0f -- "
                     "looking wider\n", (double)g_lockTol);
            continue;
        }
        if (!g_warpMoved || g_lockSkipN >= WARP_LOCK_SKIP_MAX) break;
        g_lockSkip[g_lockSkipN] = bad;
        g_lockSkipN = g_lockSkipN + 1;
        ff13logv("[FF13-SRP] battle picker: the way back locked slot %u and nobody moved -- "
                 "excluding it and trying again (%d excluded)\n", bad, g_lockSkipN);
    }
    g_lockTol = WARP_LOCK_TOL;
}

static void warp_go(void) {
    warp_point_t* pt = warp_find(field_zone_id());                 // the current zone's recorded point
    if (!pt) { flash_say(ui_text(T_WARP_NO_POINT)); return; }
    g_lockSkipN = 0; g_lockTol = WARP_LOCK_TOL;
    warp_to(pt->x, pt->y, pt->z, "the warp key");
    flash_say(ui_text(T_WARP_WARPED));
}

// Warp: validate + arm the in-overlay "warp?" prompt (same zone only; field only).
void warp_ask(void) {
    if (g_warpPrompt) return;
    int st = field_zone_state();
    uint32_t curzone = field_zone_id();
    warp_point_t* pt = warp_find(curzone);
    char wkey[32]; vk_label(g_warpVk, wkey, sizeof wkey);
    ff13log("[FF13-SRP] warp: %s pressed (zone_state=%d cur_zone=%u have_point=%d poolBase=%08X).\n",
             wkey, st, curzone, pt ? 1 : 0, (unsigned)g_poolBase);
    if (st != 3)     { flash_say(ui_text(T_WARP_FIELD_ONLY)); return; }
    if (!pt) {
        char hkey[32]; vk_label(g_warpVk, hkey, sizeof hkey);
        char msg[224];
        snprintf(msg, sizeof msg, ui_text(T_WARP_NO_POINT_HOLD), hkey);
        flash_say(msg);
        return;
    }
    if (!g_poolBase) { flash_say(ui_text(T_WARP_NO_READ)); return; }
    strncpy(g_warpPromptMsg, ui_text(T_WARP_ASK_WARP), sizeof g_warpPromptMsg - 1);
    g_warpPromptMsg[sizeof g_warpPromptMsg - 1] = 0;
    g_warpPromptUntil = GetTickCount() + 8000;
    g_warpPrompt = 2;
}

// Confirm the armed prompt (Enter): commit a record, or run the warp. Cancel (Esc) just clears it.
void warp_confirm(void) {
    int p = g_warpPrompt;
    g_warpPrompt = 0;
    if (p == 1) {
        // One point per zone, overwriting the zone's old one. The table holds 128 and the game has
        // 23 zones, so it cannot fill; the NULL guard stays because this caller would crash.
        warp_point_t* pt = warp_upsert(g_warpPendZone);
        if (!pt) { ff13logv("[FF13-SRP] warp: point table full (%d) -- not recorded\n",
                            WARP_MAXPOINTS); return; }
        pt->x = g_warpPendX; pt->y = g_warpPendY; pt->z = g_warpPendZ;
        warp_save();
        ff13log("[FF13-SRP] warp: recorded zone=%u pos=(%.2f,%.2f,%.2f) (%d zone(s) stored)\n",
                 pt->zone, pt->x, pt->y, pt->z, g_warpCount);
        flash_say(ui_text(T_WARP_RECORDED));
    } else if (p == 2) {
        warp_go();
    }
}
void warp_cancel(void) {
    if (g_warpPrompt) { g_warpPrompt = 0; flash_say(ui_text(T_WARP_CANCELLED)); }
}
// ============================ end WARP ========================================
