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
#include "ff13_savepoints_data.h"
#include "ff13_crypt.h"

// ---- save anywhere ----
// Save at any point on the field, and load back to that exact point. Settings live in
// ff13-srpractice.ini: [save] (list_mark / save_dir) and [hotkeys] (save / camera_unstick), read
// by config_load in srp_core.c.
// Text appended to the slot's title in the game's own save list ("Save Data 01*"). Empty = leave
// the list alone. [save] list_mark.
wchar_t g_listMark[9] = L"*";
char g_saveDir[MAX_PATH] = {0};// override .dat dir (empty = %LOCALAPPDATA% auto)

// ---- captured state ----
static volatile uint32_t g_savedSlot = 0, g_saveWrites = 0;
static volatile uint32_t g_loadSlot = 0, g_loadReads = 0, g_loadPending = 0;
static volatile uint32_t g_charPoolBase = 0;
static volatile uint32_t g_readySlot = 0, g_readyZone = 0, g_doRestore = 0;
// position-override state: covr / candFlags / ovr cluster restore
static volatile uint32_t g_asPoolBase = 0;
static volatile uint32_t g_covr = 0;
static volatile float g_asOvrX = 0, g_asOvrY = 0, g_asOvrZ = 0;
static uint8_t g_asCandFlags[0x4000];
// save-screen: the save hotkey sets openSaveFlag; the 0x40FA50 cave opens it.
static volatile uint32_t g_openSaveFlag = 0, g_inDoSave = 0;
static volatile uint32_t g_camReset = 0;   // set after a load-teleport; the 0x40FA50 yield cave calls 0x636A20
// save-hotkey pending capture (leader pos + zone at the moment the key is pressed).
static volatile uint32_t g_f8Armed = 0, g_pendingZone = 0;
static volatile float g_pendingX = 0, g_pendingY = 0, g_pendingZ = 0;
// per-slot sidecar: the hotkey position saved for each slot.
typedef struct { int valid; uint32_t zone; float x, y, z; } sidecar_t;
static sidecar_t g_sidecar[256];
// nearest save point chosen at the hotkey press (case B): its checkpoint name + zone area byte.
static char g_pendingCheckpoint[32] = {0};
static volatile unsigned char g_pendingArea = 0;
static volatile short g_pendingRegion = -1;

// [ecx+0x14A] = save/load slot number (movzx byte).
// ---- BgLoader / MAPSET (RE STREAM_ANCHOR_RE.md §9): the layer that streams ground as you walk.
// Resident terrain maps are (loc,map,...) descriptors in a 32-slot array on the manager below.
// This dump is read-only and fully guarded. ----
#define MAPSET_MGR_PTR 0x2331238   // VA 0x2731238 - imagebase 0x400000: MapSet/BgLoader manager pointer
static void dump_mapset(const char* when) {
    if (IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)) return;
    char* mgr = *(char**)(g_base + MAPSET_MGR_PTR);
    if (!mgr || IsBadReadPtr(mgr, 0x614)) { ff13logv("[FF13-SRP] mapset(%s): mgr=%p not ready\n", when, (void*)mgr); return; }
    char* base = mgr + 0x10;                        // 32 slots, stride 0x30
    unsigned mask = *(unsigned*)(base + 0x600);     // occupancy bitmask
    int cnt = 0; for (int i = 0; i < 32; i++) if (mask & (1u << i)) cnt++;
    ff13logv("[FF13-SRP] mapset(%s): mask=%08X (%d resident)\n", when, mask, cnt);
    for (int i = 0; i < 32; i++) {
        if (!(mask & (1u << i))) continue;
        unsigned char* s = (unsigned char*)(base + i * 0x30);
        if (IsBadReadPtr(s, 0x30)) continue;
        char hex[0x30 * 3 + 4]; char asc[0x30 + 1]; int ho = 0;
        for (int b = 0; b < 0x30; b++) {
            ho += snprintf(hex + ho, sizeof hex - ho, "%02X ", s[b]);
            asc[b] = (s[b] >= 32 && s[b] < 127) ? (char)s[b] : '.';
        }
        asc[0x30] = 0;
        ff13logv("[FF13-SRP] mapset(%s) s%02d: %s|%s\n", when, i, hex, asc);
    }
}
// Capture (loc,map) of all resident terrain maps (parsed from the slot+8 name "%07x_%07x"), PER SAVE
// SLOT, and persist to a sidecar so a later session / different spot can replay them. RE §9.
#define MAP_MAX 64
static int g_slotMapN[256];              // per-slot count of saved terrain maps
static int g_slotMap[256][MAP_MAX][2];   // per-slot [i] = { loc, map }
static volatile int g_savedMapN = 0;     // scratch: the list the current restore replays
static int g_savedMap[MAP_MAX][2];
// The MAPSET DESCRIPTOR [mgr+0xa18] (16 bytes): park ([mgr+0xa38]) and LoadMapSet operate on this
// mapset-granular descriptor, not on individual maps.
static unsigned char g_slotA18[256][16];
static int g_slotHasA18[256];
static unsigned char g_savedA18[16];
static int g_savedHasA18 = 0;
// The mapset load target is the NUMERIC id in [mgr+0xa50]&0x7fffffff (bit30=0 = numeric mode),
// NOT the mset_hgcr string (RE §12).
static uint32_t g_slotLoc[256];              // per-slot numeric mapset id ([mgr+0xa50]&0x7fffffff) at save
static int g_slotHasLoc[256];
static uint32_t g_savedLoc = 0;
static int g_savedHasLoc = 0;
static volatile int g_parkDone = 0;          // parked the saved mapset this restore
static volatile int g_parkArmed = 0;         // armed at load-START, before the 0x10 window
static volatile int g_vmArmed = 0;           // armed at on_load; gates the reconcile append
static volatile int g_vmDone  = 0;           // set once the save cells are appended (one-shot per restore)
static volatile DWORD g_vmStickyUntil = 0;   // while set and unexpired, every reconcile re-appends
static void log_desc16(const char* tag, const unsigned char* d) {
    char hex[16 * 3 + 2], asc[17]; int o = 0;
    for (int b = 0; b < 16; b++) { o += snprintf(hex + o, sizeof hex - o, "%02X ", d[b]);
                                   asc[b] = (d[b] >= 32 && d[b] < 127) ? (char)d[b] : '.'; }
    asc[16] = 0;
    ff13logv("[FF13-SRP] %s: %s|%s\n", tag, hex, asc);
}
static void mapsidecar_save(void);
static void capture_mapset_for(int slot) {
    if (slot < 0 || slot >= 256) return;
    if (IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)) return;
    char* mgr = *(char**)(g_base + MAPSET_MGR_PTR);
    if (!mgr || IsBadReadPtr(mgr, 0x614)) return;
    char* base = mgr + 0x10; unsigned mask = *(unsigned*)(base + 0x600);
    int n = 0;
    for (int i = 0; i < 32 && n < MAP_MAX; i++) {
        if (!(mask & (1u << i))) continue;
        char* nm = base + i * 0x30 + 8; if (IsBadReadPtr(nm, 16)) continue;
        char b[17]; memcpy(b, nm, 16); b[16] = 0;
        unsigned lo = 0, mp = 0;
        if (sscanf(b, "%x_%x", &lo, &mp) == 2) { g_slotMap[slot][n][0] = (int)lo; g_slotMap[slot][n][1] = (int)mp; n++; }
    }
    if (n == 0) return;                  // nothing resident yet -- keep any previous capture for this slot
    g_slotMapN[slot] = n;
    if (!IsBadReadPtr(mgr + 0xA18, 16)) { memcpy(g_slotA18[slot], mgr + 0xA18, 16); g_slotHasA18[slot] = 1;
                                          log_desc16("mapset a18 SAVE   ", (unsigned char*)(mgr + 0xA18)); }
    if (!IsBadReadPtr(mgr + 0xA50, 4)) { g_slotLoc[slot] = *(uint32_t*)(mgr + 0xA50) & 0x7FFFFFFF; g_slotHasLoc[slot] = 1;
                                         ff13logv("[FF13-SRP] mapset a50 SAVE: numeric id=%u\n", g_slotLoc[slot]); }
    mapsidecar_save();
    ff13logv("[FF13-SRP] mapset capture: slot=%d stored %d maps\n", slot, n);
}
// Is (loc,map) resident in the manager right now?
static int mapset_resident(char* mgr, int loc, int map) {
    char* base = mgr + 0x10; unsigned mask = *(unsigned*)(base + 0x600);
    for (int i = 0; i < 32; i++) {
        if (!(mask & (1u << i))) continue;
        char* nm = base + i * 0x30 + 8; if (IsBadReadPtr(nm, 16)) continue;
        char b[17]; memcpy(b, nm, 16); b[16] = 0;
        unsigned lo = 0, mp = 0;
        if (sscanf(b, "%x_%x", &lo, &mp) == 2 && (int)lo == loc && (int)mp == map) return 1;
    }
    return 0;
}
static uint32_t read_scene_fsm(void);        // fwd: scene sub-FSM state, defined below
// Reconcile 0x42b9a0 is the single choke point for both the establish path (0x42cb90 EXECUTE) and
// the dispatcher path (public 0x42b970).
static const uint8_t P_42B9A0[] = {0x55,0x8B,0xEC,0x81,0xEC,0x60,0x04,0x00,0x00};   // 0x42b9a0 reconcile prologue
// Called at the reconcile entry with (ecx=mgr, loc, &desired, &count). During the load-restore
// window, append this loc's saved cells that are missing from desired[], swapping desired to our own
// buffer and bumping count; reconcile then loads them (unload precedes load = a slot is free, and it
// runs loader-active so 0x42df60 drains instead of freezing). g_recBuf must outlive the reconcile
// call (it does: single game thread, the next reconcile only overwrites after this one returns).
static uint32_t g_recBuf[0x20];     // union desired buffer (reconcile asserts count <= 0x20)
static volatile int g_recTraceN = 0;         // reset when a new arm wants the trace
static void __cdecl on_reconcile(uint32_t ecx, uint32_t loc, uint32_t* pDesired, uint32_t* pCount) {
    // Traced before any guard: "no appended line" cannot otherwise tell "reconcile never ran"
    // from "everything was already desired".
    if (g_vmArmed && !g_vmDone && g_recTraceN < 12) {
        g_recTraceN++;
        ff13logv("[FF13-SRP] reconcile CALL: loc=%u count=%u mgrOk=%d\n", loc,
                 (pCount && !IsBadReadPtr(pCount, 4)) ? *pCount : 0,
                 !IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)
                     && (char*)(uintptr_t)ecx == *(char**)(g_base + MAPSET_MGR_PTR));
    }
    if (!g_vmArmed || g_vmDone) return;
    if (IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)) return;
    if ((char*)(uintptr_t)ecx != *(char**)(g_base + MAPSET_MGR_PTR)) return;   // mapset mgr only
    if (!pDesired || !pCount) return;
    uint32_t count = *pCount;
    uint32_t* desired = (uint32_t*)(uintptr_t)(*pDesired);
    if (count > 0x1F || !desired || IsBadReadPtr(desired, count * 4)) return;
    for (uint32_t i = 0; i < count; i++) g_recBuf[i] = desired[i];
    uint32_t n = count; int appended = 0;
    for (int i = 0; i < g_savedMapN && n < 0x20; i++) {
        int cloc = g_savedMap[i][0], cmap = g_savedMap[i][1];
        if (cloc != (int)loc || cmap == 0) continue;                 // only this reconcile's loc
        int present = 0;
        for (uint32_t j = 0; j < count; j++) if ((int)desired[j] == cmap) { present = 1; break; }
        if (present) continue;
        g_recBuf[n++] = (uint32_t)cmap;
        appended++;
    }
    if (appended) {
        *pDesired = (uint32_t)(uintptr_t)g_recBuf;
        *pCount = n;
        // One-shot per restore -- EXCEPT while a sticky window is open: a later reconcile computes
        // desired[] from scratch and evicts the appended cells again.
        if (!g_vmStickyUntil || (int)(GetTickCount() - g_vmStickyUntil) >= 0) g_vmDone = 1;
        ff13logv("[FF13-SRP] reconcile: loc=%u appended %d cell(s) (count %u->%u) (STAGE17)\n",
                 loc, appended, count, n);
    }
}
// The picker's way back borrows the STAGE17 machinery: capture what is resident at the pick spot,
// arm the reconcile append before warping back -- same cure as a save restore, same disease (the
// party arriving on ground the streamer never asked for hangs the zone in streaming state 13).
// One line per resident mapset plus the descriptor pair, for comparing the streamer's ledger
// between "standing at the pick spot", "the fight just ended" and "the field came back".
void save_mapset_census(const char* tag) {
    if (IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)) return;
    char* mgr = *(char**)(g_base + MAPSET_MGR_PTR);
    if (!mgr || IsBadReadPtr(mgr, 0xA60)) return;
    char* base = mgr + 0x10; unsigned mask = *(unsigned*)(base + 0x600);
    char line[256]; int o = 0;
    for (int i = 0; i < 32 && o < (int)sizeof line - 24; i++) {
        if (!(mask & (1u << i))) continue;
        char* nm = base + i * 0x30 + 8; if (IsBadReadPtr(nm, 16)) continue;
        char b[17]; memcpy(b, nm, 16); b[16] = 0;
        o += snprintf(line + o, sizeof line - o, " %s", b);
    }
    ff13logv("[FF13-SRP] mapset census (%s): a50=%u resident:%s\n", tag,
             IsBadReadPtr(mgr + 0xA50, 4) ? 0 : (*(uint32_t*)(mgr + 0xA50) & 0x7FFFFFFF),
             o ? line : " (none)");
    if (!IsBadReadPtr(mgr + 0xA18, 16)) log_desc16("mapset census a18   ", (unsigned char*)(mgr + 0xA18));
}
// The mapset the field is REALLY on, read from the descriptor the streamer works off. loadBg is
// not a substitute: an arena fight moves this without one, so the name the game last asked for and
// the name in force disagree after every picked arena fight (btsc06046, measured).
int save_current_mapset(char out[17]) {
    out[0] = 0;
    if (IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)) return 0;
    char* mgr = *(char**)(g_base + MAPSET_MGR_PTR);
    if (!mgr || IsBadReadPtr(mgr + 0xA18, 16)) return 0;
    memcpy(out, mgr + 0xA18, 16); out[16] = 0;
    return out[0] ? 1 : 0;
}

int save_capture_resident_cells(int out[][2], int maxn) {
    if (IsBadReadPtr((void*)(g_base + MAPSET_MGR_PTR), 4)) return 0;
    char* mgr = *(char**)(g_base + MAPSET_MGR_PTR);
    if (!mgr || IsBadReadPtr(mgr, 0x614)) return 0;
    char* base = mgr + 0x10; unsigned mask = *(unsigned*)(base + 0x600);
    int n = 0;
    for (int i = 0; i < 32 && n < maxn; i++) {
        if (!(mask & (1u << i))) continue;
        char* nm = base + i * 0x30 + 8; if (IsBadReadPtr(nm, 16)) continue;
        char b[17]; memcpy(b, nm, 16); b[16] = 0;
        unsigned lo = 0, mp = 0;
        if (sscanf(b, "%x_%x", &lo, &mp) == 2) { out[n][0] = (int)lo; out[n][1] = (int)mp; n++; }
    }
    return n;
}
void save_arm_reconcile(const int cells[][2], int n) {
    if (n <= 0) return;
    if (n > MAP_MAX) n = MAP_MAX;
    for (int i = 0; i < n; i++) { g_savedMap[i][0] = cells[i][0]; g_savedMap[i][1] = cells[i][1]; }
    g_savedMapN = n;
    g_vmArmed = 1; g_vmDone = 0; g_recTraceN = 0; g_vmStickyUntil = 0;
    ff13logv("[FF13-SRP] reconcile: armed with %d cell(s) for the way back\n", n);
}
void save_arm_reconcile_sticky(const int cells[][2], int n, DWORD ms) {
    save_arm_reconcile(cells, n);
    if (n > 0) g_vmStickyUntil = GetTickCount() + ms;
}
static void __cdecl on_save(uint32_t ecx) {
    g_savedSlot = *(uint8_t*)(uintptr_t)(ecx + 0x14A);
    g_saveWrites++;
    capture_mapset_for(g_savedSlot & 0xFF);
}
static void __cdecl on_load(uint32_t ecx) {
    g_loadSlot = *(uint8_t*)(uintptr_t)(ecx + 0x14A);
    g_loadPending = 1;
    g_loadReads++;
    // Arm the park at load START, so on_setpos can catch the fsm==0x10 window that occurs during
    // load-in (the auto-restore is too late). Uses the loading slot's saved mapset desc.
    {
        int slot = g_loadSlot & 0xFF;
        if (g_slotHasA18[slot]) {
            memcpy(g_savedA18, g_slotA18[slot], 16); g_savedHasA18 = 1;
            g_savedHasLoc = g_slotHasLoc[slot];
            if (g_savedHasLoc) g_savedLoc = g_slotLoc[slot];
            g_parkDone = 0; g_parkArmed = 1;
        } else {
            g_parkArmed = 0;
        }
        // Load THIS slot's saved cells now and arm the reconcile append. Establish (reconcile) runs
        // after on_load in the synchronous load pump, so arming here is in time. Disarm at doRestore.
        int m = g_slotMapN[slot]; g_savedMapN = 0;
        if (m > 0) {
            for (int i = 0; i < m; i++) { g_savedMap[i][0] = g_slotMap[slot][i][0]; g_savedMap[i][1] = g_slotMap[slot][i][1]; }
            g_savedMapN = m;
        }
        g_vmDone = 0; g_vmArmed = 1; g_vmStickyUntil = 0;
    }
}
// Zone state transition (0x80F340). newState==3 (FIELD_IDLE) with a pending load
// means the load finished -> request a restore.
static void __cdecl on_state(uint32_t newState) {
    if (newState == 3 && g_loadPending == 1) {
        g_readySlot = g_loadSlot;
        g_readyZone = field_zone_id();
        g_loadPending = 0;
        g_doRestore++;
    }
}
// camera follow-cache snap: armed by cluster_restore after a teleport, applied by on_postcam --
// the absolute values written into the frozen camCtrl follow-caches each frame until the deadline.
static volatile uint32_t g_camWindowUntil = 0;
static volatile uint32_t g_camReps = 0;
static volatile uint32_t g_snapN = 0;
static uint32_t g_snapOff[4];
static float g_snapVal[4][3];

// ---- camera un-stick ([hotkeys] camera_unstick). Distinct from the follow-cache lag snap.
// After a load-teleport the camera-mode FSM ([[camCtrl+0x330]+4]) can stick in state 4 (fixed/held
// framing) instead of state 2 (free-follow). MANUAL, never automatic: at a single instant state 4 is
// indistinguishable from a LEGITIMATE fixed camera (some areas use it by design). Handler selection
// depends solely on the int, so forcing it is a pure write -- no engine call, no scene/stream
// reattach, so it cannot tear the map down the way 0x636A20/event1 would.
#define CAMCTRL_PTR 0x23FF554          // [0x27ff554] FieldCameraController (VA - imagebase)
static volatile int g_camUnstickN = 0; // frames to force the camera-mode FSM to state 2 (armed by hotkey)
static void camera_unstick_tick(void) {
    if (g_camUnstickN <= 0) return;
    g_camUnstickN--;
    if (IsBadReadPtr((void*)(g_base + CAMCTRL_PTR), 4)) return;
    char* cam = *(char**)(g_base + CAMCTRL_PTR);
    if (!cam || IsBadReadPtr(cam + 0x330, 4)) return;
    uint32_t fsm = *(uint32_t*)(cam + 0x330);
    if (!fsm || IsBadReadPtr((void*)(uintptr_t)(fsm + 4), 4)) return;
    int cur = *(int*)(uintptr_t)(fsm + 4);
    if (cur == 4) { *(int*)(uintptr_t)(fsm + 4) = 2;               // free-follow; pure write, no game call
                    ff13log("[FF13-SRP] camera-unstick: FSM state 4 -> 2 (free-follow)\n"); }
    else ff13logv("[FF13-SRP] camera-unstick: FSM state %d (not stuck) -> no change\n", cur);
}

#ifdef FF13_DIAG
// ---- DIAGNOSTICS (diag build only, -DFF13_DIAG; the default release build omits all of this). ----

static uint32_t pool_base(void);                                        // fwd: character (live-actor) pool
static int read_spos(uint32_t pb, int i, float* x, float* y, float* z); // fwd: slot position read

// Read-only camera-state dump (on the camera-unstick key): the camera-mode FSM state index
// ([[camCtrl+0x330]+4], 18 states), the input/mode bit-fields, the follow target and the follow
// caches vs the leader.
static void camera_state_dump(const char* when) {
    uintptr_t pp = g_base + 0x23FF554;                     // [0x27ff554] FieldCameraController
    if (IsBadReadPtr((void*)pp, 4)) { ff13logv("[FF13-SRP][CAMDIAG] %s: camctrl ptr bad\n", when); return; }
    char* cam = *(char**)pp;
    if (!cam || IsBadReadPtr(cam, 0x4D4)) { ff13logv("[FF13-SRP][CAMDIAG] %s: cam=%p not ready\n", when, (void*)cam); return; }
    int state = -1; uint32_t fsm = *(uint32_t*)(cam + 0x330);
    if (fsm && !IsBadReadPtr((void*)(uintptr_t)fsm, 8)) state = *(int*)(uintptr_t)(fsm + 4);
    uint32_t b154 = *(uint32_t*)(cam + 0x154), b438 = *(uint32_t*)(cam + 0x438);
    float* c9c = (float*)(cam + 0x9C); float* c1a8 = (float*)(cam + 0x1A8); float* c1b8 = (float*)(cam + 0x1B8);
    uint32_t pb = pool_base(); float lx = 0, ly = 0, lz = 0;
    if (pb) { if (!read_spos(pb, 2, &lx, &ly, &lz) || (lx == 0 && ly == 0 && lz == 0)) read_spos(pb, 0, &lx, &ly, &lz); }
    ff13logv("[FF13-SRP][CAMDIAG] %s: cam=%p fsmState=%d bits154=%08X bits438=%08X\n", when, (void*)cam, state, b154, b438);
    ff13logv("[FF13-SRP][CAMDIAG] %s: follow9C=%.1f,%.1f,%.1f 1A8=%.1f,%.1f,%.1f 1B8=%.1f,%.1f,%.1f leader=%.1f,%.1f,%.1f\n",
             when, c9c[0], c9c[1], c9c[2], c1a8[0], c1a8[1], c1a8[2], c1b8[0], c1b8[1], c1b8[2], lx, ly, lz);
    uintptr_t sp = g_base + 0x23FF068;                     // [0x27ff068] core scene/app mgr; +0xa4 = cam-req
    if (!IsBadReadPtr((void*)sp, 4)) { char* scn = *(char**)sp;
        if (scn && !IsBadReadPtr(scn + 0xA4, 4)) ff13logv("[FF13-SRP][CAMDIAG] %s: scn+0xA4(req)=%d\n", when, *(int*)(scn + 0xA4)); }
    uintptr_t fpp = g_base + 0x23FF414;                    // [0x27ff414] +0x2d0 = camera request bits (0x618c80)
    if (!IsBadReadPtr((void*)fpp, 4)) { char* fp = *(char**)fpp;
        if (fp && !IsBadReadPtr(fp + 0x2D0, 4)) ff13logv("[FF13-SRP][CAMDIAG] %s: fp+0x2D0(bits)=%08X\n", when, *(uint32_t*)(fp + 0x2D0)); }
}
#endif // FF13_DIAG

// Camera follow-cache snap -- pure position writes, NO engine camera call. cluster_restore arms
// g_snapVal with the 3 frozen camCtrl [0x27ff554] Vector3 (+0x9C/+0x1A8/+0x1B8) shifted by the
// teleport delta; they are written here every frame during the window, on the game thread
// (post-0x62C0A0), so the write races cleanly with the camera update.
static void __cdecl on_postcam(uint32_t ecx) {
    (void)ecx;
    camera_unstick_tick();                                      // manual camera un-stick (game thread)
    // Cell restore is handled by the reconcile hook (on_reconcile); on_postcam does camera only.
    if (!g_camWindowUntil) return;
    uint32_t now = GetTickCount();
    if ((int)(now - g_camWindowUntil) >= 0) { g_camWindowUntil = 0; return; }
    void* cam = IsBadReadPtr((void*)(g_base + 0x23FF554), 4) ? 0 : *(void**)(g_base + 0x23FF554);
    if (cam && !IsBadReadPtr(cam, 0x1C4)) {
        for (uint32_t i = 0; i < g_snapN; i++) {
            float* p = (float*)((char*)cam + g_snapOff[i]);
            p[0] = g_snapVal[i][0]; p[1] = g_snapVal[i][1]; p[2] = g_snapVal[i][2];
        }
    }
    g_camReps++;
}

// charPoolBase capture (0x7982C0).
static void __cdecl on_charpool(uint32_t ecx) {
    if (*(uint32_t*)(uintptr_t)ecx == 0x10C934C) {
        uint32_t obj = *(uint32_t*)(uintptr_t)(ecx + 0x2C);
        if (obj) g_charPoolBase = *(uint32_t*)(uintptr_t)(*(uint32_t*)(uintptr_t)(obj + 0x0C));
    }
}
// Position commit (0xD6D2D0). ecx=object, posVec=Vector4* about to be set; the object's pool slot
// is [ecx+0x08]. When covr is active and this slot is flagged, the outgoing position is overwritten
// with ovrX/Y/Z (cluster teleport). This is the ONLY behavior-changing hook here.
static void __cdecl on_setpos(uint32_t ecx, float* posVec) {
    if (!g_asPoolBase) {
        uint32_t p = *(uint32_t*)(uintptr_t)(ecx + 0x0C);
        if (p) g_asPoolBase = *(uint32_t*)(uintptr_t)p;
    }
    if (g_covr) {
        uint32_t slot = *(uint32_t*)(uintptr_t)(ecx + 0x08);
        if (slot < 0x4000 && g_asCandFlags[slot] && posVec) {
            posVec[0] = g_asOvrX; posVec[1] = g_asOvrY; posVec[2] = g_asOvrZ;
        }
    }
}

// ---- nearest save point (case B): call 0x704a80 in-process to resolve each of the
// current zone's save-point node ids to a world coord, pick the one closest to the
// hotkey position. 0x704a80 self-guards (returns <1 when the scene isn't ready). ----
static int resolve_node_coord(int uid, float* x, float* y, float* z) {
    void* scene = *(void**)(g_base + 0x2401B70);   // [0x2801B70] scene manager
    if (!scene) return 0;
    typedef int (__attribute__((thiscall)) *resolve_t)(void*, int, void*, void*, void*);
    resolve_t fn = (resolve_t)(g_base + 0x304A80);  // VA 0x704A80
    float o0[16] = {0}, o1[16] = {0}, o2[16] = {0};
    int cnt = fn(scene, uid, o0, o1, o2);
    if (cnt < 1) return 0;
    *x = o0[12]; *y = o0[13]; *z = o0[14];   // out0 row3 translation (+0x30/34/38)
    return 1;
}
static const ff13_zone_t* zone_data(int zone) {
    for (int i = 0; i < FF13_ZONES_N; i++) if (FF13_ZONES[i].zone == zone) return &FF13_ZONES[i];
    return 0;
}
static const char* nearest_savepoint(int zone, float px, float py, float pz,
                                     unsigned char* areaOut, short* regionOut) {
    const ff13_zone_t* zd = zone_data(zone);
    if (!zd) return 0;
    const char* best = 0; float bestD2 = 1e30f; short bestReg = -1;
    for (int i = 0; i < zd->n; i++) {
        float x, y, z;
        if (!resolve_node_coord(zd->sp[i].uid, &x, &y, &z)) continue;
        float dx=x-px, dy=y-py, dz=z-pz, d2=dx*dx+dy*dy+dz*dz;
        if (d2 < bestD2) { bestD2 = d2; best = zd->sp[i].name; bestReg = zd->sp[i].region; }
    }
    if (best) { if (areaOut) *areaOut = zd->area; if (regionOut) *regionOut = bestReg; }
    return best;
}

// ---- save-fix (case B): after a hotkey save, rewrite the .dat's checkpoint(0x3028) to the
// nearest save point + area(0x40B3), so the load spawns next to the restore point. ----
#define FF13_NAME_OFF   0x3028
// The checkpoint-name field is 16 bytes (0x3028..0x3037; max real name = 15 chars + NUL).
// 0x3038 holds a SEPARATE "rank_XXXXX" RestartData field that late-game zones (e.g. Eden)
// REQUIRE on load -- zeroing it makes the game reject the save and bounce to the title.
// So clear/write ONLY 16 bytes here; never touch 0x3038. (The old 0x20 width clobbered it.)
#define FF13_NAME_WIDTH 0x10
#define FF13_NAME_FIELD 0x20
#define FF13_AREA_OFF   0x40B3

#define FF13_THUMB_BASE 0x48004000u

// ---- save-file paths and .dat.bak housekeeping ----
// savefix copies the game's own save to "ff13-NN.dat.bak" before rewriting its checkpoint, so a bad
// rewrite can be undone by renaming that copy back. INVARIANT: our .bak exists only as the twin of
// the CURRENT ff13-NN.dat and is dropped as soon as that stops being true -- a .bak left behind by a
// plain save over the slot is an OLDER save, and restoring it would roll that slot's progress back.
static uint8_t g_bakKnown[256];       // slots we believe hold one of our .bak files
static int save_path_for(int slot, char* out, size_t n) {
    if (g_saveDir[0]) { snprintf(out, n, "%s\\ff13-%02d.dat", g_saveDir, slot); return 1; }
    const char* la = getenv("LOCALAPPDATA");
    if (!la) return 0;
    snprintf(out, n, "%s\\SquareEnix\\FinalFantasyXIII\\save\\ff13-%02d.dat", la, slot);
    return 1;
}
static int file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int bak_path_for(int slot, char* out, size_t n) {
    char p[MAX_PATH];
    if (slot < 0 || slot > 255 || !save_path_for(slot, p, sizeof(p))) return 0;
    snprintf(out, n, "%s.bak", p);
    return 1;
}
// Drop our backup of `slot` because it is no longer the twin of that slot's save. `why` goes in the
// log so a vanished .bak is always explained. Clearing g_bakKnown in the keep path is deliberate:
// it stops the periodic check from re-reporting the same file every two seconds.
static void bak_drop(int slot, const char* why) {
    char b[MAX_PATH];
    if (!bak_path_for(slot, b, sizeof(b)) || !file_exists(b)) { if (slot >= 0 && slot < 256) g_bakKnown[slot] = 0; return; }
    if (DeleteFileA(b)) { g_bakKnown[slot] = 0;
        ff13log("[FF13-SRP] bak: removed ff13-%02d.dat.bak (%s)\n", slot, why); }
    else ff13loga("[FF13-SRP] bak: WARNING could not remove ff13-%02d.dat.bak (err %lu)\n", slot, GetLastError());
}
// Startup pass: learn which slots carry one of our .bak files, and drop the ones whose .dat is gone
// (a save deleted from the save screen during an earlier session, before we could see it happen).
static void bak_scan(void) {
    int found = 0;
    for (int s = 0; s < 256; s++) {
        char b[MAX_PATH], p[MAX_PATH];
        if (!bak_path_for(s, b, sizeof(b)) || !save_path_for(s, p, sizeof(p))) return;   // no save dir
        if (!file_exists(b)) continue;
        g_bakKnown[s] = 1; found++;
        if (!file_exists(p)) bak_drop(s, "its save was deleted");
    }
    if (found) ff13log("[FF13-SRP] bak: %d backup file(s) present at startup\n", found);
}

static void do_savefix(int slot, const char* cp, unsigned char area, short region) {
    char path[MAX_PATH];
    if (!save_path_for(slot, path, sizeof(path))) { ff13loga("[FF13-SRP] savefix: no LOCALAPPDATA\n"); return; }
    FILE* f = fopen(path, "rb");
    if (!f) { ff13loga("[FF13-SRP] savefix: open fail %s\n", path); return; }
    static uint8_t buf[FF13_FILE_LEN];
    size_t n = fread(buf, 1, FF13_FILE_LEN, f); fclose(f);
    if (n != FF13_FILE_LEN) { ff13logv("[FF13-SRP] savefix: bad size %u\n", (unsigned)n); return; }
    ff13_decrypt(buf, n);
    char before[FF13_NAME_WIDTH+1]; memcpy(before, buf+FF13_NAME_OFF, FF13_NAME_WIDTH);
    before[FF13_NAME_WIDTH]=0;
    // GUARD: only rewrite a REGULAR savepoint. An event restart point (rp_*), a chapter checkpoint
    // (sp_chpt_*) or anything else is the story-consistent restart the game requires, and replacing
    // it with the distance-nearest savepoint makes the save unloadable (proven on hardware). Leave
    // those untouched; the sidecar teleport still repositions the party at load.
    if (strncmp(before, "s_point_", 8) != 0) {
        ff13log("[FF13-SRP] savefix: slot %d kept as-is -- game checkpoint '%s' is story-critical (not a savepoint)\n", slot, before);
        return;
    }
    size_t L = strlen(cp);
    if (L >= FF13_NAME_WIDTH) { ff13loga("[FF13-SRP] savefix: name too long\n"); return; }
    // Safety net: back up the ORIGINAL save before editing (always loadable; restore the
    // .bak if anything is off -- the sidecar still teleports to the hotkey position).
    { char bp[MAX_PATH]; snprintf(bp, sizeof(bp), "%s.bak", path);
      if (CopyFileA(path, bp, FALSE)) { if (slot >= 0 && slot < 256) g_bakKnown[slot] = 1;
          ff13log("[FF13-SRP] savefix: backed up original -> ff13-%02d.dat.bak\n", slot); }
      else ff13loga("[FF13-SRP] savefix: WARNING backup failed (slot %d)\n", slot); }
    memset(buf + FF13_NAME_OFF, 0, FF13_NAME_WIDTH);   // 16-byte name field only; preserve rank@0x3038
    memcpy(buf + FF13_NAME_OFF, cp, L);
    buf[FF13_AREA_OFF] = area;
    // thumbnail background (plaintext header 0x04, LE): set region, keep leader nibble.
    // id = 0x48004000 | region<<4 | leader; leader OR'd only for field regions (<0x32).
    if (region >= 0) {
        uint32_t oldt = buf[4] | (buf[5]<<8) | (buf[6]<<16) | ((uint32_t)buf[7]<<24);
        uint32_t nt = FF13_THUMB_BASE | ((uint32_t)region << 4);
        if (region < 0x32) nt |= (oldt & 0xF);
        buf[4]=(uint8_t)nt; buf[5]=(uint8_t)(nt>>8); buf[6]=(uint8_t)(nt>>16); buf[7]=(uint8_t)(nt>>24);
    }
    ff13_encrypt(buf, n);
    FILE* o = fopen(path, "wb");
    if (!o) { ff13loga("[FF13-SRP] savefix: write-open fail\n"); return; }
    fwrite(buf, 1, n, o); fclose(o);
    ff13log("[FF13-SRP] savefix: slot %d  '%s' -> '%s'  area=0x%02X region=%d  written\n",
            slot, before, cp, area, region);
}

// ---- mark our saves in the game's own save/load list ----
// The 0x208-byte PLAINTEXT header in front of the encrypted body holds the two lines the browser
// prints; the load path never reads it, so editing this text changes the list and nothing else.
// The marker goes at the END OF THE FIRST COMMA FIELD -- the only part of the line every language
// shows (the Japanese browser cuts the title at the second comma). Nothing has to take it off
// again: the game rewrites the whole header on every save.
#define FF13_LIST_OFF 0x108      // UTF-16LE title line, inside the plaintext header
#define FF13_LIST_CAP 128        // wchar_t (0x108..0x207)
static void mark_save_list(int slot) {
    if (!g_listMark[0]) return;
    char path[MAX_PATH];
    if (!save_path_for(slot, path, sizeof(path))) return;
    FILE* f = fopen(path, "r+b");
    if (!f) { ff13loga("[FF13-SRP] list-mark: open fail %s\n", path); return; }
    wchar_t line[FF13_LIST_CAP];
    if (fseek(f, FF13_LIST_OFF, SEEK_SET) != 0 || fread(line, sizeof(wchar_t), FF13_LIST_CAP, f) != FF13_LIST_CAP) {
        fclose(f); ff13loga("[FF13-SRP] list-mark: slot %d header read fail\n", slot); return;
    }
    size_t len = 0;
    while (len < FF13_LIST_CAP && line[len]) len++;
    if (len == FF13_LIST_CAP || len == 0) {   // no terminator = not the title we expect; leave it alone
        fclose(f); ff13loga("[FF13-SRP] list-mark: slot %d has no readable title -- skipped\n", slot); return;
    }
    size_t at = 0;
    while (at < len && line[at] != L',') at++;     // end of "Save Data NN"
    size_t m = 0; while (g_listMark[m]) m++;
    if (at >= m && memcmp(line + at - m, g_listMark, m * sizeof(wchar_t)) == 0) {
        fclose(f); return;                          // already marked (a re-run over the same save)
    }
    if (len + m + 1 > FF13_LIST_CAP) {
        fclose(f); ff13loga("[FF13-SRP] list-mark: slot %d title too long for the marker\n", slot); return;
    }
    memmove(line + at + m, line + at, (len - at + 1) * sizeof(wchar_t));
    memcpy(line + at, g_listMark, m * sizeof(wchar_t));
    int ok = fseek(f, FF13_LIST_OFF, SEEK_SET) == 0
             && fwrite(line, sizeof(wchar_t), len + m + 1, f) == len + m + 1;
    fclose(f);
    if (ok) ff13log("[FF13-SRP] list-mark: slot %d marked in the save list\n", slot);
    else    ff13loga("[FF13-SRP] list-mark: slot %d write fail\n", slot);
}

// The twin of the block below lives in srp_warp.c, on a different commit hook; kept separate on purpose.
// ---- cluster restore: flag party slots near the leader, then hold covr for ~600ms so 0xD6D2D0
// teleports them to (tx,ty,tz). Blocks: runs on the hotkey thread, never on the game thread. ----
static uint32_t pool_base(void) { return g_charPoolBase ? g_charPoolBase : g_asPoolBase; }

static int read_spos(uint32_t pb, int i, float* x, float* y, float* z) {
    uintptr_t a = pb + (uintptr_t)i * 0x70;
    if (IsBadReadPtr((void*)a, 0x10)) return 0;
    *x = *(float*)a; *y = *(float*)(a+4); *z = *(float*)(a+8);
    return 1;
}

static int cluster_restore(float tx, float ty, float tz) {
    uint32_t pb = pool_base();
    if (!pb) return -1;
    float cx, cy, cz;
    if (!read_spos(pb, 2, &cx, &cy, &cz) || (cx==0 && cy==0 && cz==0))
        if (!read_spos(pb, 0, &cx, &cy, &cz)) return -2;
    if (cx==0 && cy==0 && cz==0) return -2;

    // bound the slot scan to committed memory
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)pb, &mbi, sizeof(mbi))) return -3;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

    memset(g_asCandFlags, 0, sizeof(g_asCandFlags));
    const float tol = 8.0f;
    int n = 0;
    for (uint32_t k = 0; k < 0x4000; k++) {
        uintptr_t a = pb + (uintptr_t)k * 0x70;
        if (a + 0x10 > regionEnd) break;
        float x=*(float*)a, y=*(float*)(a+4), z=*(float*)(a+8), w=*(float*)(a+0xC);
        if (w==1.0f && fabsf(x-cx)<tol && fabsf(y-cy)<tol && fabsf(z-cz)<tol) {
            g_asCandFlags[k] = 1; n++;
        }
    }
    g_asOvrX = tx; g_asOvrY = ty; g_asOvrZ = tz;
    g_covr = 1;
    Sleep(600);
    g_covr = 0;
    memset(g_asCandFlags, 0, sizeof(g_asCandFlags));

    // ---- arm the follow-cache snap: shift the 3 frozen camCtrl Vector3 (+0x9C/+0x1A8/+0x1B8) by
    // the teleport delta and hold them there for ~800ms via on_postcam (pure position writes). ----
    void* cam = IsBadReadPtr((void*)(g_base + 0x23FF554), 4) ? 0 : *(void**)(g_base + 0x23FF554);
    float dx = tx-cx, dy = ty-cy, dz = tz-cz;
    g_snapN = 0;
    if (cam && !IsBadReadPtr(cam, 0x1C4)) {
        static const uint32_t offs[3] = {0x9C, 0x1A8, 0x1B8};
        for (int i = 0; i < 3; i++) {
            float* p = (float*)((char*)cam + offs[i]);
            g_snapOff[i] = offs[i];
            g_snapVal[i][0] = p[0]+dx; g_snapVal[i][1] = p[1]+dy; g_snapVal[i][2] = p[2]+dz;
        }
        g_snapN = 3;
        g_camReps = 0;
        g_camWindowUntil = GetTickCount() + 800;
        ff13logv("[FF13-SRP] cam-snap armed +9C/+1A8/+1B8 shift %.1f,%.1f,%.1f (800ms)\n", dx, dy, dz);
    }
    return n;
}

// original prologues (verified against the binary)
static const uint8_t P_839810[] = {0x55,0x8B,0xEC,0x81,0xEC,0x68,0x0C,0x00,0x00};
static const uint8_t P_839F30[] = {0x55,0x8B,0xEC,0x81,0xEC,0x3C,0x02,0x00,0x00};
static const uint8_t P_80F340[] = {0x55,0x8B,0xEC,0x81,0xEC,0x20,0x06,0x00,0x00};
static const uint8_t P_7982C0[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0};
static const uint8_t P_D6D2D0[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0};   // RVA 0x96D2D0
static const uint8_t P_FA50[]   = {0x55,0x8B,0xEC,0x83,0xEC,0x40};                // RVA 0xFA50 (save screen)
static const uint8_t P_61CBAD[] = {0xA1,0x14,0xF4,0x7F,0x02};             // RVA 0x21CBAD (after call 0x62c0a0)
static const uint8_t P_61CE3E[] = {0x8B,0x15,0xDC,0x80,0x6E,0x02};        // RVA 0x21CE3E (after call 0x62c0a0)
// Scene sub-FSM state. BG-load/reconcile must ONLY be issued when this == 0x10 (field-ready); the
// freeze came from issuing it mid-field (from 0xD6D2D0) while the FSM was running the field. Chain:
// this = [GameMain(0x281c9bc) + 0x36c]; state = *(*(this+0x230) + 4). All derefs guarded.
static uint32_t read_scene_fsm(void) {
    if (IsBadReadPtr((void*)(g_base + 0x241C9BC), 4)) return 0xFFFFFFFF;
    char* gm = *(char**)(g_base + 0x241C9BC);
    if (!gm || IsBadReadPtr(gm + 0x36C, 4)) return 0xFFFFFFFF;
    char* t = *(char**)(gm + 0x36C);
    if (!t || IsBadReadPtr(t + 0x230, 4)) return 0xFFFFFFFF;
    char* s = *(char**)(t + 0x230);
    if (!s || IsBadReadPtr(s + 4, 4)) return 0xFFFFFFFF;
    return *(uint32_t*)(s + 4);
}

// ---- sidecar file persistence: the .dat stores no position, so the restore target lives only in
// our sidecar -- a text file next to the exe, "slot zone x y z" per line. ----
static void sidecar_path_for(char* path, size_t n, const char* base) {
    if (!data_file(path, n, base)) path[0] = 0;     // callers check path[0]
}
static void sidecar_path(char* path, size_t n) { sidecar_path_for(path, n, "ff13-srpractice_slots.dat"); }
static void sidecar_save_file(void) {
    char path[MAX_PATH]; sidecar_path(path, sizeof(path)); if (!path[0]) return;
    FILE* f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < 256; i++)
        if (g_sidecar[i].valid)
            fprintf(f, "%d %u %.3f %.3f %.3f\n", i, g_sidecar[i].zone,
                    g_sidecar[i].x, g_sidecar[i].y, g_sidecar[i].z);
    fclose(f);
}
static void sidecar_load_file(void) {
    char path[MAX_PATH]; sidecar_path(path, sizeof(path)); if (!path[0]) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    int slot; unsigned zone; float x, y, z, cnt = 0;
    while (fscanf(f, "%d %u %f %f %f", &slot, &zone, &x, &y, &z) == 5) {
        if (slot >= 0 && slot < 256) {
            g_sidecar[slot].valid = 1; g_sidecar[slot].zone = zone;
            g_sidecar[slot].x = x; g_sidecar[slot].y = y; g_sidecar[slot].z = z; cnt++;
        }
    }
    fclose(f);
    ff13log("[FF13-SRP] sidecar loaded: %d slots\n", (int)cnt);
}

// Terrain-map sidecar: one line per slot, "slot count loc0 map0 loc1 map1 ...", so a later session
// can replay the maps that were resident when this slot was saved.
static void mapsidecar_save(void) {
    char path[MAX_PATH]; sidecar_path_for(path, sizeof(path), "ff13-srpractice_maps.dat"); if (!path[0]) return;
    FILE* f = fopen(path, "w"); if (!f) return;
    for (int s = 0; s < 256; s++) {
        int m = g_slotMapN[s]; if (m <= 0) continue;
        fprintf(f, "%d %d", s, m);
        for (int i = 0; i < m; i++) fprintf(f, " %d %d", g_slotMap[s][i][0], g_slotMap[s][i][1]);
        fprintf(f, "\n");
    }
    fclose(f);
}
static void mapsidecar_load(void) {
    char path[MAX_PATH]; sidecar_path_for(path, sizeof(path), "ff13-srpractice_maps.dat"); if (!path[0]) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    int slot, m, slots = 0;
    while (fscanf(f, "%d %d", &slot, &m) == 2) {
        if (slot < 0 || slot >= 256 || m < 0) { fclose(f); return; }   // corrupt -> stop
        if (m > MAP_MAX) m = MAP_MAX;
        int n = 0;
        for (int i = 0; i < m; i++) {
            int lo, mp; if (fscanf(f, "%d %d", &lo, &mp) != 2) { m = i; break; }
            g_slotMap[slot][n][0] = lo; g_slotMap[slot][n][1] = mp; n++;
        }
        if (n) { g_slotMapN[slot] = n; slots++; }
    }
    fclose(f);
    ff13log("[FF13-SRP] mapset sidecar loaded: %d slots\n", slots);
}

// ---- the three entry points the merged worker calls ----

// What a previous session left behind: the per-slot positions, the per-slot terrain-map lists, and
// which .bak files are ours.
void save_sidecars_load(void) {
    sidecar_load_file();
    mapsidecar_load();
    bak_scan();            // also drops the .bak files whose save was deleted while we were off
}

void save_install_hooks(void) {
    // SteamStub decrypts regions at slightly different times, so wait for EACH hook's
    // prologue before installing it (fixes intermittent savescreen=FAIL etc.).
    wait_for_bytes(g_base + 0x439810, P_839810, 9, 15000);
    void* c1 = ff13_install_ecx_hook     (g_base + 0x439810, P_839810, 9, (void*)on_save);
    wait_for_bytes(g_base + 0x439F30, P_839F30, 9, 15000);
    void* c3 = ff13_install_ecx_hook     (g_base + 0x439F30, P_839F30, 9, (void*)on_load);
    wait_for_bytes(g_base + 0x40F340, P_80F340, 9, 15000);
    void* c2 = ff13_install_stackarg_hook(g_base + 0x40F340, P_80F340, 9, (void*)on_state, 8);
    wait_for_bytes(g_base + 0x3982C0, P_7982C0, 6, 15000);
    void* cp = ff13_install_ecx_hook     (g_base + 0x3982C0, P_7982C0, 6, (void*)on_charpool);
    wait_for_bytes(g_base + 0x96D2D0, P_D6D2D0, 6, 15000);
    void* cs = ff13_install_ecx_stackarg_hook(g_base + 0x96D2D0, P_D6D2D0, 6, (void*)on_setpos, 4);
    wait_for_bytes(g_base + 0xFA50, P_FA50, 6, 15000);
    void* cf = ff13_install_savescreen_hook(g_base + 0xFA50, g_base + 0x40EA40,
                                            (uintptr_t)&g_openSaveFlag, (uintptr_t)&g_inDoSave,
                                            g_base + 0x236A20, g_base + 0x23FF554, (uintptr_t)&g_camReset);
    wait_for_bytes(g_base + 0x21CBAD, P_61CBAD, 5, 15000);
    void* pc1 = ff13_install_ecx_hook    (g_base + 0x21CBAD, P_61CBAD, 5, (void*)on_postcam);
    wait_for_bytes(g_base + 0x21CE3E, P_61CE3E, 6, 15000);
    void* pc2 = ff13_install_ecx_hook    (g_base + 0x21CE3E, P_61CE3E, 6, (void*)on_postcam);
    // reconcile hook -- append the save cell to desired[] during establish
    wait_for_bytes(g_base + 0x2B9A0, P_42B9A0, 9, 15000);
    void* vh = ff13_install_argptr_hook(g_base + 0x2B9A0, P_42B9A0, 9, (void*)on_reconcile,
                                        /*valOff=loc*/8, /*ptrOff1=desired*/0xC, /*ptrOff2=count*/0x10);
    (c1 && c3 && c2 && cp && cs && cf && pc1 && pc2 && vh ? ff13logv : ff13loga)("[FF13-SRP] hooks: save=%s load=%s state=%s charpool=%s setpos=%s savescreen=%s postcam=%s/%s reconcile=%s\n",
            c1?"OK":"FAIL", c3?"OK":"FAIL", c2?"OK":"FAIL", cp?"OK":"FAIL", cs?"OK":"FAIL", cf?"OK":"FAIL",
            pc1?"OK":"FAIL", pc2?"OK":"FAIL", vh?"OK":"FAIL");
}

// Tick counts, each written as the millisecond value it means divided by the poll period of the
// merged hotkey loop (HK_POLL_MS).
#define SAVE_SETTLE_TICKS           (160 / HK_POLL_MS)     // stable-position gate before a restore
#define SAVE_RESTORE_TIMEOUT_TICKS  (30000 / HK_POLL_MS)   // give the restore up after this
#define SAVE_FIX_DELAY_TICKS        (2000 / HK_POLL_MS)    // let the game flush the .dat first
#define SAVE_BAK_SWEEP_TICKS        (2000 / HK_POLL_MS)    // how often the deleted-save watch looks

// One tick of the save-anywhere loop, called from the hotkey thread's poll. Edge detection is on the
// MSB (currently-down) via hk_edge; GetAsyncKeyState's LSB "recently pressed" bit was not latching.
void save_poll_tick(DWORD now) {
    (void)now;
    static uint32_t lSave = 0, lLoad = 0, lRestore = 0, lCpb = 0;
    static int f8_was = 0;
    // restore state machine: wait for the field to settle after a load, then
    // cluster_restore to the saved position.
    static int restorePending = 0, restoreTicks = 0, stableCnt = 0;
    static float rx = 0, ry = 0, rz = 0, lastLx = 1e9f, lastLy = 0, lastLz = 0;
    static uint32_t rzone = 0;
    // save-fix schedule: ~2s after a hotkey save (so the game has flushed the file).
    static int fixCountdown = 0, fixSlot = 0; static char fixCp[32] = {0};
    static unsigned char fixArea = 0; static short fixRegion = -1;
    static unsigned bakTick = 0;                 // the sweep's own tick count (wraparound harmless)
    // Latched on the first tick: "whatever the hooks had recorded when we started".
    static int inited = 0;
    if (!inited) { inited = 1; lSave = g_saveWrites; lLoad = g_loadReads; lRestore = g_doRestore; }

    if (g_charPoolBase && !lCpb) { lCpb=1;
        ff13logv("[FF13-SRP] charPoolBase=%08X\n", g_charPoolBase); }

    // ---- save committed: persist the hotkey position per slot, or clear on normal save ----
    if (g_saveWrites != lSave) {
        lSave = g_saveWrites;
        uint32_t slot = g_savedSlot & 0xFF;
        uint32_t zNow = field_zone_id();
        // Whatever this save is, the previous backup of this slot now predates it. do_savefix
        // makes a fresh one below if it edits this save; a normal save leaves the slot with none.
        bak_drop((int)slot, "the slot was saved over");
        if (g_f8Armed && (g_pendingZone == 0xFFFFFFFF || zNow == g_pendingZone)) {
            g_sidecar[slot].valid = 1; g_sidecar[slot].zone = g_pendingZone;
            g_sidecar[slot].x = g_pendingX; g_sidecar[slot].y = g_pendingY; g_sidecar[slot].z = g_pendingZ;
            ff13log("[FF13-SRP] hotkey-save slot=%u persist %.1f,%.1f,%.1f zone=%u\n",
                    slot, g_pendingX, g_pendingY, g_pendingZ, g_pendingZone);
            // Deferred file edits (~2s, so the game has flushed the file): the case-B checkpoint
            // fix, and the save-list marker either way. Empty fixCp = no checkpoint, mark only.
            fixSlot = (int)slot; fixCp[0] = 0;
            fixArea = g_pendingArea; fixRegion = g_pendingRegion; fixCountdown = SAVE_FIX_DELAY_TICKS;
            if (g_pendingCheckpoint[0]) { strncpy(fixCp, g_pendingCheckpoint, 31); fixCp[31]=0; }
            sidecar_save_file();
        } else if (g_sidecar[slot].valid) {
            g_sidecar[slot].valid = 0;
            ff13log("[FF13-SRP] normal save slot=%u -> cleared custom pos\n", slot);
            sidecar_save_file();
        }
        g_f8Armed = 0;
    }

    // ---- load complete (doRestore): arm the restore if this slot has a hotkey pos ----
    if (g_doRestore != lRestore) {
        lRestore = g_doRestore;
        uint32_t slot = g_readySlot & 0xFF;
        if (g_sidecar[slot].valid && g_sidecar[slot].zone == g_readyZone) {
            restorePending = 1; restoreTicks = 0; stableCnt = 0; lastLx = 1e9f;
            rx = g_sidecar[slot].x; ry = g_sidecar[slot].y; rz = g_sidecar[slot].z;
            rzone = g_readyZone;
            ff13log("[FF13-SRP] LOAD slot=%u -> restore pending %.1f,%.1f,%.1f zone=%u\n",
                    slot, rx, ry, rz, rzone);
        } else {
            ff13log("[FF13-SRP] LOAD slot=%u -> no restore (valid=%d zone=%u ready=%u)\n",
                    slot, g_sidecar[slot].valid, g_sidecar[slot].zone, g_readyZone);
        }
    }

    // ---- restore state machine: wait for the field to settle, then teleport ----
    if (restorePending) {
        restoreTicks++;
        uint32_t pb = pool_base();
        float lx, ly, lz;
        if (pb && (read_spos(pb,2,&lx,&ly,&lz) || read_spos(pb,0,&lx,&ly,&lz))
            && !(lx==0&&ly==0&&lz==0) && field_zone_id()==rzone) {
            if (lastLx<1e8f && fabsf(lx-lastLx)<0.5f && fabsf(ly-lastLy)<0.5f && fabsf(lz-lastLz)<0.5f)
                stableCnt++;
            else stableCnt = 0;
            lastLx=lx; lastLy=ly; lastLz=lz;
            if (stableCnt >= SAVE_SETTLE_TICKS) {   // stable this long = field ready
                int n = cluster_restore(rx, ry, rz);
                ff13log("[FF13-SRP] auto-restore -> %.1f,%.1f,%.1f (flagged %d)\n", rx, ry, rz, n);
                restorePending = 0;
            }
        }
        if (restoreTicks > SAVE_RESTORE_TIMEOUT_TICKS) { ff13log("[FF13-SRP] restore timeout\n"); restorePending = 0; }
    }

    static uint32_t lReps = 0;
    if (g_camReps != lReps && !g_camWindowUntil) { lReps = g_camReps;
        ff13logv("[FF13-SRP] post-teleport window done: cam-snap %u frames\n", g_camReps); }

    // ---- deferred save-fix (case B checkpoint edit), ~2s after the hotkey save ----
    if (fixCountdown > 0 && --fixCountdown == 0) {
        if (fixCp[0]) do_savefix(fixSlot, fixCp, fixArea, fixRegion);
        // After savefix, which rewrites the whole file from a buffer it read before this point.
        // Runs even when savefix declined the save: the position was recorded either way.
        mark_save_list(fixSlot);
    }
    // ---- a save deleted from the in-game save screen takes our leftovers with it ----
    // Two consecutive misses 2s apart, and never while a save is being written, so that a save
    // that rewrites the file in place is not mistaken for a deletion.
    unsigned tick = bakTick++;
    if ((tick % SAVE_BAK_SWEEP_TICKS) == 0 && !g_inDoSave && fixCountdown == 0) {
        static uint8_t datMiss[256];
        for (int s = 0; s < 256; s++) {
            if (!g_bakKnown[s] && !g_sidecar[s].valid && !g_slotMapN[s]) { datMiss[s] = 0; continue; }
            char p[MAX_PATH];
            if (!save_path_for(s, p, sizeof(p))) break;      // no save dir -> nothing to watch
            if (file_exists(p)) { datMiss[s] = 0; continue; }
            if (++datMiss[s] < 2) continue;                  // one miss = maybe mid-write
            datMiss[s] = 0;
            ff13log("[FF13-SRP] slot %d was deleted in the save screen -> dropping our leftovers\n", s);
            bak_drop(s, "its save was deleted");
            if (g_sidecar[s].valid) { g_sidecar[s].valid = 0; sidecar_save_file(); }
            if (g_slotMapN[s])      { g_slotMapN[s] = 0;      mapsidecar_save(); }
        }
    }
    // ---- save hotkey = open the "save anywhere" screen: capture the leader position + zone, then
    //      set openSaveFlag so the 0x40FA50 cave calls the interactive save entry on the next yield.
    // ---- camera un-stick hotkey (manual): arm the FSM state 4->2 force (consumed on the game thread) ----
    if (g_camUnstickVk) { static int cu_was = 0;
      if (hk_edge(g_camUnstickVk, &cu_was)) {
#ifdef FF13_DIAG
          camera_state_dump("unstick");
#endif
          g_camUnstickN = 3;   // force free-follow for ~3 frames (on_postcam / game thread)
      }
    }
    if (hk_edge(g_saveVk, &f8_was)) {
      int zs = field_zone_state();
      if (zs != 3) {   // not FIELD_IDLE: event/battle/transition -> unresumable save
        flash_say(ui_text(T_SAVE_FIELD_ONLY));
        ff13loga("[FF13-SRP] save-hotkey blocked: zone state=%d (need FIELD_IDLE=3). Saving during an "
                "event/battle/transition makes a save the game can't load -- return to normal "
                "field and retry.\n", zs);
      } else {
        uint32_t pb = pool_base();
        float lx, ly, lz;
        if (pb && (read_spos(pb, 2, &lx, &ly, &lz) || read_spos(pb, 0, &lx, &ly, &lz))
            && !(lx==0 && ly==0 && lz==0)) {
            g_pendingX = lx; g_pendingY = ly; g_pendingZ = lz;
            g_pendingZone = field_zone_id();
            g_f8Armed = 1;
            g_openSaveFlag = 1;
            ff13log("[FF13-SRP] save-hotkey: pending %.1f,%.1f,%.1f zone=%u -> open save screen\n",
                    lx, ly, lz, g_pendingZone);
            // case B: pick the nearest save point (resolved live via 0x704a80)
            unsigned char area = 0; short region = -1;
            const char* np = nearest_savepoint((int)g_pendingZone, lx, ly, lz, &area, &region);
            if (np) {
                strncpy(g_pendingCheckpoint, np, sizeof(g_pendingCheckpoint)-1);
                g_pendingCheckpoint[sizeof(g_pendingCheckpoint)-1] = 0;
                g_pendingArea = area; g_pendingRegion = region;
                ff13logv("[FF13-SRP] nearest save point: %s (area 0x%02X region %d)\n", np, area, region);
            } else {
                g_pendingCheckpoint[0] = 0;
                ff13logv("[FF13-SRP] nearest save point: none for zone %u\n", g_pendingZone);
            }
        } else {
            flash_say(ui_text(T_SAVE_NO_READ));
            ff13log("[FF13-SRP] save-hotkey: no leader position (not in field?)\n");
        }
      }
    }
}
