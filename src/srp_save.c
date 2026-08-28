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
static volatile int   g_vmArmPicker = 0;     // 1 = the arm is the picker/warp way-back, holdable
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
    // A way-back arm belongs to the FIELD the player left: a picked fight's own arena reconcile
    // must not inherit it (measured: the pick spot's terrain stood inside btsc12091). The arm is
    // kept for the real field return. A save-restore arm (on_load) is NEVER held -- its one
    // synchronous load pump is its only chance to append.
    if (g_vmArmPicker && (g_bsStarted || g_bsTouchDue)) {
        if (g_recTraceN < 12)
            ff13logv("[FF13-SRP] reconcile: held -- a picked fight is loading its own ground\n");
        return;
    }
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
    g_vmArmed = 1; g_vmDone = 0; g_recTraceN = 0; g_vmStickyUntil = 0; g_vmArmPicker = 1;
    ff13logv("[FF13-SRP] reconcile: armed with %d cell(s) for the way back\n", n);
}
void save_arm_reconcile_sticky(const int cells[][2], int n, DWORD ms) {
    save_arm_reconcile(cells, n);
    if (n > 0) g_vmStickyUntil = GetTickCount() + ms;
}
static void __cdecl on_save(uint32_t ecx) {
    g_savedSlot = *(uint8_t*)(uintptr_t)(ecx + 0x14A);
    g_saveWrites++;
    // Hotkey saves only: a maps entry on an ordinary save goes stale and arms STAGE17 on every
    // plain load of that slot (measured: load-end hang).
    if (g_f8Armed) capture_mapset_for(g_savedSlot & 0xFF);
}
static void __cdecl on_load(uint32_t ecx) {
    g_loadSlot = *(uint8_t*)(uintptr_t)(ecx + 0x14A);
    g_loadPending = 1;
    g_loadReads++;
    // Arm the park at load START, so on_setpos can catch the fsm==0x10 window that occurs during
    // load-in (the auto-restore is too late). Uses the loading slot's saved mapset desc.
    {
        int slot = g_loadSlot & 0xFF;
        // The sidecar entry, not a maps entry, is what says the hotkey made this save: a stale
        // maps entry armed this on plain loads (measured: load-end hang).
        int anywhere = g_sidecar[slot].valid;
        if (anywhere && g_slotHasA18[slot]) {
            memcpy(g_savedA18, g_slotA18[slot], 16); g_savedHasA18 = 1;
            g_savedHasLoc = g_slotHasLoc[slot];
            if (g_savedHasLoc) g_savedLoc = g_slotLoc[slot];
            g_parkDone = 0; g_parkArmed = 1;
        } else {
            g_parkArmed = 0;
        }
        // Load THIS slot's saved cells now and arm the reconcile append. Establish (reconcile) runs
        // after on_load in the synchronous load pump, so arming here is in time. Disarm at doRestore.
        int m = anywhere ? g_slotMapN[slot] : 0;
        g_savedMapN = 0;
        if (m > 0) {
            for (int i = 0; i < m; i++) { g_savedMap[i][0] = g_slotMap[slot][i][0]; g_savedMap[i][1] = g_slotMap[slot][i][1]; }
            g_savedMapN = m;
        }
        g_vmArmed = (m > 0); g_vmDone = 0; g_vmStickyUntil = 0; g_vmArmPicker = 0;
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
static int save_root_dir(char* out, size_t n) {
    if (g_saveDir[0]) { snprintf(out, n, "%s", g_saveDir); return 1; }
    const char* la = getenv("LOCALAPPDATA");
    if (!la) return 0;
    snprintf(out, n, "%s\\SquareEnix\\FinalFantasyXIII\\save", la);
    return 1;
}
static int save_path_for(int slot, char* out, size_t n) {
    char root[MAX_PATH];
    if (!save_root_dir(root, sizeof root)) return 0;
    snprintf(out, n, "%s\\ff13-%02d.dat", root, slot);
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

// The browser bounds every slot index by 0x63 (0x838B34 / 0x838CCB; the load loop at 0x8387ED
// runs i < 0x63), so ff13-00.dat .. ff13-98.dat only -- a file at index 99 kills its list.
#define SS_SLOTS 99
// Browser list row: the save number, the mtime its qsort compares, the printed date string.
#define OFF_ROW_SLOT   0x14A
#define OFF_ROW_FT     0x150
#define OFF_ROW_STR    0x158
static void sidecar_path_for(char* path, size_t n, const char* base);

// ---- what this mod writes into the game's own save list -------------------------------------
// The 0x208-byte PLAINTEXT header in front of the encrypted body holds the two lines the browser
// prints; the load path never reads it, so editing this text changes the list and nothing else.
// Only the first TWO comma fields are printed (the Japanese browser cuts the title at the second
// comma), so everything this mod adds -- the list_mark right after the number, the label at the
// end of field 2 -- has to stay inside them.
#define FF13_LIST_OFF 0x108      // UTF-16LE title line, inside the plaintext header
#define FF13_LIST_CAP 128        // wchar_t (0x108..0x207)

// The mtime is the save list's sort key, the date the row prints, and what a dates-sidecar row
// is matched by, so every in-place header edit keeps it: take it before, put it back after.
static int mtime_keep(const char* path, WIN32_FILE_ATTRIBUTE_DATA* fa) {
    return GetFileAttributesExA(path, GetFileExInfoStandard, fa) != 0;
}
static void mtime_put_back(const char* path, const WIN32_FILE_ATTRIBUTE_DATA* fa) {
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, NULL, NULL, &fa->ftLastWriteTime);
    CloseHandle(h);
}
static void title_refresh(int slot) {
    char path[MAX_PATH];
    if (slot < 0 || slot >= SS_SLOTS || !save_path_for(slot, path, sizeof(path))) return;
    FILE* f = fopen(path, "r+b");
    if (!f) return;
    wchar_t line[FF13_LIST_CAP], out[FF13_LIST_CAP];
    if (fseek(f, FF13_LIST_OFF, SEEK_SET) != 0 ||
        fread(line, sizeof(wchar_t), FF13_LIST_CAP, f) != FF13_LIST_CAP) {
        fclose(f); ff13loga("[FF13-SRP] title: slot %d header read fail\n", slot); return;
    }
    size_t len = 0; while (len < FF13_LIST_CAP && line[len]) len++;
    if (!len || len == FF13_LIST_CAP) {
        fclose(f); ff13loga("[FF13-SRP] title: slot %d has no readable title -- skipped\n", slot);
        return;
    }
    size_t comma = 0; while (comma < len && line[comma] != L',') comma++;
    size_t a = 0; while (a < comma && (line[a] < L'0' || line[a] > L'9')) a++;
    if (a == comma) { fclose(f); return; }
    size_t head = a; while (head < comma && line[head] >= L'0' && line[head] <= L'9') head++;

    size_t n = head, cap = FF13_LIST_CAP - 1;
    memcpy(out, line, head * sizeof(wchar_t));
    // The mark means "made by the save hotkey", and the sidecar entry is that record.
    if (g_sidecar[slot].valid)
        for (int i = 0; g_listMark[i] && n < cap; i++) out[n++] = g_listMark[i];
    size_t tail = len - comma;
    if (n + tail + 1 > FF13_LIST_CAP) {
        fclose(f); ff13loga("[FF13-SRP] title: slot %d would not fit -- left as it was\n", slot);
        return;
    }
    memcpy(out + n, line + comma, (tail + 1) * sizeof(wchar_t));
    n += tail;
    if (n == len && memcmp(out, line, (len + 1) * sizeof(wchar_t)) == 0) { fclose(f); return; }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int haveTime = mtime_keep(path, &fa);
    int ok = fseek(f, FF13_LIST_OFF, SEEK_SET) == 0
             && fwrite(out, sizeof(wchar_t), n + 1, f) == n + 1;
    fclose(f);
    if (!ok) { ff13loga("[FF13-SRP] title: slot %d write fail\n", slot); return; }
    if (haveTime) mtime_put_back(path, &fa);
}
static void mark_save_list(int slot) { title_refresh(slot); }

// ---- labels: a note of your own on a save, shown in the game's list ---------------------------
// The text lives in the save's own title line, at the end of the second field, and NOWHERE ELSE:
// there is no sidecar for it.
#define LABEL_MAX 25
#define LABEL_SEP0 L' '
#define LABEL_SEP1 L'-'

// The comma separates the fields this line is made of, so a label may not contain one.
static int label_ok_char(wchar_t c) {
    return !(c == L',' || c < L' ');
}

// [out] = the label's first character, [end] = one past field 2; on 0 both are the field end.
static int title_label_span(const wchar_t* line, size_t len, size_t* out, size_t* end) {
    size_t comma = 0; while (comma < len && line[comma] != L',') comma++;
    if (comma == len) { *out = *end = len; return 0; }   /* the contract holds on a foreign title */
    size_t f2end = comma + 1;
    while (f2end < len && line[f2end] != L',') f2end++;
    for (size_t i = comma; i + 2 < f2end; i++)
        if (line[i] == LABEL_SEP0 && line[i+1] == LABEL_SEP1 && line[i+2] == LABEL_SEP0) {
            *out = i + 3; *end = f2end; return 1;
        }
    *out = f2end; *end = f2end;
    return 0;
}

static int title_read(int slot, wchar_t* line, size_t* len) {
    char path[MAX_PATH];
    if (slot < 0 || slot >= SS_SLOTS || !save_path_for(slot, path, sizeof(path))) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fseek(f, FF13_LIST_OFF, SEEK_SET) == 0
             && fread(line, sizeof(wchar_t), FF13_LIST_CAP, f) == FF13_LIST_CAP;
    fclose(f);
    if (!ok) return 0;
    size_t n = 0; while (n < FF13_LIST_CAP && line[n]) n++;
    if (!n || n == FF13_LIST_CAP) return 0;
    *len = n;
    return 1;
}

int label_get(int slot, wchar_t* out, int cap) {
    out[0] = 0;
    wchar_t line[FF13_LIST_CAP]; size_t len;
    if (!title_read(slot, line, &len)) return 0;
    size_t a, e;
    if (!title_label_span(line, len, &a, &e)) return 0;
    int n = 0;
    while (a < e && n < cap - 1) out[n++] = line[a++];
    out[n] = 0;
    return n;
}

static int label_set(int slot, const wchar_t* text) {
    char path[MAX_PATH];
    if (slot < 0 || slot >= SS_SLOTS || !save_path_for(slot, path, sizeof(path))) return 0;
    wchar_t line[FF13_LIST_CAP], out[FF13_LIST_CAP]; size_t len;
    if (!title_read(slot, line, &len)) return 0;
    size_t a, e;
    // Back over the " - " only when there IS one: doing it unconditionally cut three characters
    // off the game's own text (measured).
    size_t cut = title_label_span(line, len, &a, &e) ? (a >= 3 ? a - 3 : e) : e;
    if (cut > e) cut = e;
    size_t n = cut, cap = FF13_LIST_CAP - 1;
    memcpy(out, line, cut * sizeof(wchar_t));
    if (text && text[0] && n + 3 < cap) {
        out[n++] = LABEL_SEP0; out[n++] = LABEL_SEP1; out[n++] = LABEL_SEP0;
        for (int i = 0; text[i] && n < cap; i++) out[n++] = text[i];
    }
    size_t tail = len - e;
    if (n + tail + 1 > FF13_LIST_CAP) return 0;
    memcpy(out + n, line + e, (tail + 1) * sizeof(wchar_t));
    n += tail;
    if (n == len && memcmp(out, line, (len + 1) * sizeof(wchar_t)) == 0) return 1;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int haveTime = mtime_keep(path, &fa);
    FILE* f = fopen(path, "r+b");
    if (!f) return 0;
    int ok = fseek(f, FF13_LIST_OFF, SEEK_SET) == 0
             && fwrite(out, sizeof(wchar_t), n + 1, f) == n + 1;
    fclose(f);
    if (ok && haveTime) mtime_put_back(path, &fa);
    return ok;
}

// ---- paste a label onto the save you are looking at -------------------------------------------
// 0x83AF40 hands the UI the date string of the row it is showing, out of [this+4]: the row it was
// last asked about is the one under the cursor, and it is only asked while the list is on screen.
#define RVA_SAVEROW_SHOWN 0x43AF40     // VA 0x83AF40
static const uint8_t P_83AF40[] = {0x55,0x8B,0xEC,0x83,0xEC,0x0C};
// The getter fires on open and on cursor moves, not per frame, so this must outlast a pause.
#define LABEL_FRESH_MS 8000
// The title screen's load list is STATE_LOAD; the save screen inside a running game stays
// STATE_ZONE_IDLE (common/docs/MAINLOOP_HOOK.md), and the title's top menu is STATE_TITLE.
#define MAIN_STATE_TITLE 4
#define MAIN_STATE_LOAD 15
static volatile int   g_shownSlot = -1;
static volatile DWORD g_shownAt = 0;
static volatile int   g_shownState = -1;

// [fsm+4] is the live state, the same shape as the camera FSM (CAMERA_MAP_TEARDOWN_RE.md).
#define RVA_APP        0x241C9BC   // VA 0x0281C9BC
#define OFF_APP_MAINSM 0x36C
#define OFF_MAINSM_FSM 0x230
static int main_state(void) {
    if (IsBadReadPtr((void*)(g_base + RVA_APP), 4)) return -1;
    uint32_t app = *(uint32_t*)(g_base + RVA_APP);
    if (!app || IsBadReadPtr((void*)(uintptr_t)(app + OFF_APP_MAINSM), 4)) return -1;
    uint32_t sm = *(uint32_t*)(uintptr_t)(app + OFF_APP_MAINSM);
    if (!sm || IsBadReadPtr((void*)(uintptr_t)(sm + OFF_MAINSM_FSM), 4)) return -1;
    uint32_t fsm = *(uint32_t*)(uintptr_t)(sm + OFF_MAINSM_FSM);
    if (!fsm || IsBadReadPtr((void*)(uintptr_t)(fsm + 4), 4)) return -1;
    return *(int*)(uintptr_t)(fsm + 4);
}

static void __cdecl on_row_shown(uint32_t ui) {
    if (!ui || IsBadReadPtr((void*)(uintptr_t)(ui + 4), 4)) return;
    uint32_t row = *(uint32_t*)(uintptr_t)(ui + 4);
    if (!row || IsBadReadPtr((void*)(uintptr_t)(row + OFF_ROW_SLOT), 1)) return;
    int slot = *(unsigned char*)(uintptr_t)(row + OFF_ROW_SLOT);
    if (slot < 0 || slot >= SS_SLOTS) return;
    g_shownSlot = slot;
    g_shownAt = GetTickCount();
    g_shownState = main_state();
}

static int clipboard_text(wchar_t* out, int cap) {
    int opened = 0;
    for (int i = 0; i < 6 && !(opened = OpenClipboard(NULL)); i++) Sleep(15);
    if (!opened) return 0;
    int got = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* w = (const wchar_t*)GlobalLock(h);
        if (w) {
            int n = 0;
            while (w[n] && w[n] != L'\r' && w[n] != L'\n' && n < cap - 1) { out[n] = w[n]; n++; }
            out[n] = 0;
            got = 1;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return got;
}

// ---- the editor ------------------------------------------------------------------------------
// The window procedure runs on the game's message thread and only edits the buffer; the file work
// happens on the hotkey thread when it sees the commit flag (label_edit_tick / cat_edit_tick).
volatile int g_labelEdit = 0;
volatile int g_labelEditSlot = -1;
#define EDIT_BUF_MAX (LABEL_MAX > CATNAME_MAX + 1 ? LABEL_MAX : CATNAME_MAX + 1)
static wchar_t g_editBuf[EDIT_BUF_MAX];
static volatile int g_editLen = 0;
static volatile int g_editDone = 0;        // 1 = commit, -1 = cancel
static volatile int g_editBad = 0;
static volatile int g_editMode = EDIT_LABEL;
static WNDPROC g_prevProc = NULL;
static HWND    g_gameWnd = NULL;

const wchar_t* label_edit_text(void) { return g_editBuf; }
int label_edit_bad(void) { return g_editBad; }
int edit_mode(void) { return g_editMode; }

// The comma is refused on top of the filesystem's set: this mod's own files are comma-separated.
static int catname_ok_char(wchar_t c) {
    static const wchar_t kBad[] = L"\\/:*?\"<>|,";
    if (c < L' ') return 0;
    for (int i = 0; kBad[i]; i++) if (c == kBad[i]) return 0;
    return 1;
}
static int edit_ok_char(wchar_t c) {
    return g_editMode == EDIT_LABEL ? label_ok_char(c) : catname_ok_char(c);
}
static int edit_cap(void) { return g_editMode == EDIT_LABEL ? LABEL_MAX : CATNAME_MAX + 1; }

static void edit_insert(const wchar_t* w) {
    int cap = edit_cap();
    for (int i = 0; w[i]; i++) {
        if (g_editLen >= cap - 1) { g_editBad = 1; return; }
        if (!edit_ok_char(w[i])) { g_editBad = 1; continue; }
        g_editBuf[g_editLen++] = w[i];
    }
    g_editBuf[g_editLen] = 0;
}

static LRESULT CALLBACK srp_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (g_labelEdit) {
        // Keys are swallowed so typing does not walk the game's menus. Esc is the deliberate
        // exception -- it is what opens the game's quit dialog.
        if ((m == WM_KEYDOWN || m == WM_KEYUP) && wp != VK_ESCAPE) return 0;
        if (m == WM_CHAR) {
            wchar_t c = (wchar_t)wp;
            // No Enter case on purpose: an Enter reaching the save list behind the editor would
            // start loading a save.
            if (c == 0x08) { if (g_editLen) g_editBuf[--g_editLen] = 0; }        // Backspace
            else if (c == 0x16) {                            // Ctrl+V
                wchar_t w[256];
                if (clipboard_text(w, 256)) edit_insert(w); else g_editBad = 1;
            }
            else if (c >= L' ') { g_editBad = 0; wchar_t one[2] = { c, 0 }; edit_insert(one); }
            return 0;
        }
    }
    return CallWindowProcW(g_prevProc, h, m, wp, lp);
}

static BOOL CALLBACK find_game_wnd(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

static int edit_hook_window(void) {
    if (g_prevProc) return 1;
    HWND h = NULL;
    EnumWindows(find_game_wnd, (LPARAM)&h);
    if (!h) { ff13loga("[FF13-SRP] labels: the game window was not found\n"); return 0; }
    g_prevProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)srp_wndproc);
    if (!g_prevProc) { ff13loga("[FF13-SRP] labels: cannot take the window (err %lu)\n", GetLastError()); return 0; }
    g_gameWnd = h;
    ff13log("[FF13-SRP] labels: editor ready on the game window\n");
    return 1;
}

void label_edit_finish(int commit) { g_editDone = commit ? 1 : -1; }

// STATE_LOAD deliberately skips the freshness test: the row watch only fires when the game redraws
// the row, so a menu of ours holding the input gate lets the window lapse with that row still on
// screen (measured). The state check catches backing out of the list to the title.
int label_edit_ready(void) {
    if (g_shownSlot < 0) return 0;
    int now = main_state();
    if (now != g_shownState) {
        ff13logv("[FF13-SRP] labels: the save list has closed (main state %d -> %d) -- the key is"
                 " not the list's any more\n", g_shownState, now);
        return 0;
    }
    if (now == MAIN_STATE_LOAD) return 1;
    return (int)(GetTickCount() - g_shownAt) <= LABEL_FRESH_MS;
}

static int label_edit_open_slot(int slot) {
    if (g_labelEdit) return g_labelEditSlot + 1;
    if (!edit_hook_window()) return LABEL_PASTE_NO_WINDOW;
    g_editMode = EDIT_LABEL;
    g_labelEditSlot = slot;
    label_get(g_labelEditSlot, g_editBuf, LABEL_MAX);
    g_editLen = (int)wcslen(g_editBuf);
    g_editDone = 0; g_editBad = 0;
    g_labelEdit = 1;
    return g_labelEditSlot + 1;
}
// The save screen's "make a new save" row is reported by the row watch with the slot it would
// take, so only the file's existence tells that row from a real save (measured).
static int slot_has_file(int slot) {
    char p[MAX_PATH];
    return slot >= 0 && slot < SS_SLOTS && save_path_for(slot, p, sizeof p) && file_exists(p);
}
int label_edit_open(void) {
    if (!g_labelEdit && !label_edit_ready()) return LABEL_PASTE_NO_ROW;
    if (!g_labelEdit && !slot_has_file(g_shownSlot)) {
        ff13log("[FF13-SRP] labels: save %d has no file -- the new-save row takes no note\n",
                g_shownSlot + 1);
        return LABEL_PASTE_NO_SAVE;
    }
    return label_edit_open_slot(g_shownSlot);
}

// Runs on the hotkey thread, never the message thread: the file work for a finished edit.
int label_edit_tick(char* what, size_t n) {
    if (!g_labelEdit || g_editMode != EDIT_LABEL || !g_editDone) return 0;
    int slot = g_labelEditSlot, commit = g_editDone;
    g_labelEdit = 0; g_editDone = 0;
    if (commit < 0) return 0;
    int a = 0; while (g_editBuf[a] == L' ') a++;
    int b = g_editLen; while (b > a && g_editBuf[b-1] == L' ') b--;
    g_editBuf[b] = 0;
    if (!label_set(slot, g_editBuf + a)) {
        ff13loga("[FF13-SRP] labels: save %d has no file to write into -- nothing written\n",
                 slot + 1);
        return LABEL_PASTE_NO_SAVE;
    }
    if (what && n) WideCharToMultiByte(CP_UTF8, 0, g_editBuf + a, -1, what, (int)n, NULL, NULL);
    ff13log("[FF13-SRP] labels: save %d set from the editor\n", slot + 1);
    return slot + 1;
}


// ---- Steam Cloud ----
// Read out of Steam's own config file, never steam_api.dll: the cloud switches are vtable slots on
// a version-specific interface, and a wrong slot called in a live game is a crash.
#define STEAM_APPID      "292120"     // FF13 on Steam (white_data\prog\win\bin\steam_appid.txt)
#define VDF_MAX          (4u << 20)   // sharedconfig.vdf runs to tens of KB; anything huge is not it
#define VDF_DEPTH        16
#define VDF_TOK          64
#define CLOUD_RECHECK_MS 3000

static int reg_str(HKEY root, const char* sub, const char* val, char* out, DWORD n) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, sz = n;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE*)out, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ || !sz) return 0;
    out[(sz < n) ? sz : n - 1] = 0;             // the value can come back without its terminator
    return out[0] != 0;
}
static DWORD reg_dword(HKEY root, const char* sub, const char* val) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, v = 0, sz = sizeof v;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE*)&v, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_DWORD) ? v : 0;
}

// Depth is counted past the stack's end so braces still pair when nesting exceeds VDF_DEPTH.
static void vdf_cloud(const char* buf, int* acct, int* app) {
    char stack[VDF_DEPTH][VDF_TOK], pend[VDF_TOK] = "";
    int depth = 0, havePend = 0;
    for (const char* p = buf; *p; ) {
        if (*p == '"') {
            char tok[VDF_TOK]; size_t n = 0;
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1]) p++;
                if (n + 1 < sizeof tok) tok[n++] = (char)tolower((unsigned char)*p);
            }
            tok[n] = 0;
            if (*p == '"') p++;
            if (!havePend) { memcpy(pend, tok, n + 1); havePend = 1; continue; }
            if (!strcmp(pend, "cloudenabled") && depth > 0 && depth <= VDF_DEPTH) {
                int on = strcmp(tok, "0") ? 1 : 0;
                if (!strcmp(stack[depth - 1], "steam")) *acct = on;
                else if (depth > 1 && !strcmp(stack[depth - 1], STEAM_APPID)
                                   && !strcmp(stack[depth - 2], "apps")) *app = on;
            }
            havePend = 0;
        } else if (*p == '{') {
            if (depth < VDF_DEPTH) snprintf(stack[depth], VDF_TOK, "%s", havePend ? pend : "");
            depth++; havePend = 0; p++;
        } else if (*p == '}') {
            if (depth) depth--;
            havePend = 0; p++;
        } else if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
        } else p++;
    }
}
static int vdf_read_cloud(const char* path, int* acct, int* app) {
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    char* buf = (sz != INVALID_FILE_SIZE && sz < VDF_MAX) ? (char*)malloc((size_t)sz + 1) : NULL;
    int ok = buf && ReadFile(h, buf, sz, &got, NULL) && got == sz;
    CloseHandle(h);
    if (ok) { buf[sz] = 0; vdf_cloud(buf, acct, app); }
    free(buf);
    return ok;
}
// userdata\<id> is per Steam account: the wrong one answers about somebody else's switches.
static int steam_user_dir(const char* root, char* out, size_t n) {
    DWORD id = reg_dword(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser");
    if (id) { snprintf(out, n, "%s\\userdata\\%lu", root, (unsigned long)id); return 1; }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\userdata\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0; char only[64] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;   // "." ".." and "0"
        if (++found == 1) snprintf(only, sizeof only, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (found != 1) return 0;
    snprintf(out, n, "%s\\userdata\\%s", root, only);
    return 1;
}
// 1 = this game's saves are synced, 0 = they are not, -1 = Steam would not say.
static int steam_cloud_query(char* why, size_t n) {
    char root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    if (!reg_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", root, sizeof root)
     && !reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", root, sizeof root)) {
        snprintf(why, n, "Steam is not in the registry -- its cloud switch is unknown");
        return -1;
    }
    for (char* s = root; *s; s++) if (*s == '/') *s = '\\';   // SteamPath is written with slashes
    if (!steam_user_dir(root, dir, sizeof dir)) {
        snprintf(why, n, "no single Steam account under %s\\userdata -- the cloud switch is "
                 "unknown", root);
        return -1;
    }
    snprintf(path, sizeof path, "%s\\7\\remote\\sharedconfig.vdf", dir);
    int acct = -1, app = -1;
    if (!vdf_read_cloud(path, &acct, &app)) {
        snprintf(why, n, "%s could not be read -- the cloud switch is unknown", path);
        return -1;
    }
    int on = !(acct == 0 || app == 0);           // unset counts as on; either 0 means not synced
    snprintf(why, n, "sync is %s for this game (account switch %s, game switch %s, from %s)",
             on ? "ON" : "off",
             acct < 0 ? "unset" : acct ? "on" : "off",
             app  < 0 ? "unset" : app  ? "on" : "off", path);
    return on;
}
int steam_cloud_on(void) {
    static int ans = -2, said = -2;
    static DWORD at = 0;
    DWORD now = GetTickCount();
    if (ans == -2 || (int)(now - at) >= CLOUD_RECHECK_MS) {
        char why[MAX_PATH + 128] = "";
        ans = steam_cloud_query(why, sizeof why);
        at = now;
        if (ans != said) { said = ans; ff13loga("[FF13-SRP] cloud: %s\n", why); }
    }
    return ans;
}
// ---- the saves a chapter change leaves behind ----
// The game's own between-cutscene save: its title carries the OLD chapter's number, but loading it
// starts the NEW one. The twelve names are the game's own list (clbs
// sys/script_decomp/fld/com.java, sfSetLocationSimpleNameChapterSave) -- a list, not a prefix
// test: sp_chpt_pmpm_01 sits in the middle of chapter 7.
static const struct { const char* name; unsigned char chapter; } kChapterSave[] = {
    { "sp_chpt_hgin_00",  2 }, { "sp_chpt_hgcr_00",  3 }, { "sp_chpt_vpek_00",  4 },
    { "sp_chpt_gapr_00",  5 }, { "sp_chpt_snls_00",  6 }, { "sp_chpt_pmpm_00",  7 },
    { "sp_chpt_nati_00",  8 }, { "sp_chpt_hiku_00",  9 }, { "sp_chpt_fark_00", 10 },
    { "sp_chpt_gp_00",   11 }, { "sp_chpt_eden_00", 12 }, { "sp_chpt_lasd_01", 13 },
};
// 0 = an ordinary save.
static int chapter_save_leads_to(const char* cp) {
    for (unsigned i = 0; i < sizeof kChapterSave / sizeof kChapterSave[0]; i++)
        if (!strcmp(cp, kChapterSave[i].name)) return kChapterSave[i].chapter;
    return 0;
}
// Window decrypt: the cipher's blocks are independent and this field sits on their grid
// (ff13_crypt.c). Leaves the file position where it lands.
_Static_assert(((FF13_NAME_OFF - FF13_HEADER_LEN) % 8) == 0 && (FF13_NAME_WIDTH % 8) == 0,
               "the checkpoint field must sit on the cipher's 8-byte block grid");
static int checkpoint_read(FILE* f, char out[FF13_NAME_WIDTH + 1]) {
    uint8_t win[FF13_NAME_WIDTH];
    if (fseek(f, FF13_NAME_OFF, SEEK_SET) != 0 || fread(win, 1, sizeof win, f) != sizeof win)
        return 0;
    ff13_decrypt_window(win, FF13_NAME_OFF, sizeof win);
    memcpy(out, win, FF13_NAME_WIDTH);
    out[FF13_NAME_WIDTH] = 0;
    return 1;
}

// ---- sort the game's save list ----
// The browser orders by file mtime, newest first, re-reads the directory every time it opens, and
// prints that mtime as the save's date (all three measured), so sorting is only re-stamping mtimes.
#define SS_OFF_K1 0x4560A     // u16 LE: coarse story step high, a finer step low
#define SS_OFF_K2 0x306A      // u8: splits ties inside one K1 value
volatile int g_ssOpen = 0;
volatile int g_ssSel  = 0;

typedef struct { int slot; uint32_t key; ULONGLONG real; unsigned char leads; } ss_ent;
static int g_ssDescend = 0;
static int ss_cmp(const void* A, const void* B) {
    const ss_ent* a = (const ss_ent*)A; const ss_ent* b = (const ss_ent*)B;
    if (a->key != b->key) {
        int r = (a->key < b->key) ? -1 : 1;
        return g_ssDescend ? -r : r;
    }
    // A chapter-change save shares its story step with the first saves of the chapter it opens.
    if ((a->leads != 0) != (b->leads != 0)) {
        int r = a->leads ? -1 : 1;
        return g_ssDescend ? -r : r;
    }
    if (a->real != b->real) return (a->real > b->real) ? -1 : 1;
    return a->slot - b->slot;
}

// ---- keep the REAL save dates on screen while the stamps own the order ----
// One builder fills both +0x150 (local FILETIME, the only thing the qsort compares) and +0x158
// (the printed string) from the same mtime; hooking its EXIT is what separates the two.
// RE and the offsets: common/docs/SAVE_CRYPT_RE.md 9.
#define RVA_DATE_RET   0x439626      // VA 0x839626, the builder's epilogue
#define RVA_DATE_FMT   0x439630      // VA 0x839630, __cdecl(dest, cap, const SYSTEMTIME*)
#define DATE_STR_CAP   0x18
static const uint8_t P_839626[] = {0x8B,0xE5,0x5D,0xC2,0x04,0x00};
static void sidecar_save_file(void);

// Stamps sit on whole minutes counting back from the sort (truncation-proof) and are matched
// against the sidecar EXACTLY. Only the pre-release 2001 grid is recognisable on sight -- a real
// date that passed second-precision storage sits on a minute about 1 in 60 times.
#define SS_STEP 600000000ULL
#define SS_2001_BASE (116444736000000000ULL + 978307200ULL * 10000000ULL)
static int stamp_2001(ULONGLONG t) {
    return t >= SS_2001_BASE && t < SS_2001_BASE + 100 * SS_STEP && t % SS_STEP == 0;
}

static ULONGLONG ft_u64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static ULONGLONG ft_local(ULONGLONG utc) {
    FILETIME a, b;
    a.dwLowDateTime = (DWORD)utc; a.dwHighDateTime = (DWORD)(utc >> 32);
    return FileTimeToLocalFileTime(&a, &b) ? ft_u64(b) : 0;
}

// stampLocal is the stamp as it reaches +0x150 (local), and the hook matches on that -- so a slot
// the game has saved over since matches nothing and is left alone.
typedef struct { ULONGLONG stamp, stampLocal, real; unsigned char valid; } date_row;
static date_row g_dateRow[100];
static volatile int g_dateRows = 0;
static volatile int g_dateHooked = 0;

static void __cdecl on_date_built(uint32_t esp) {
    if (!g_dateRows) return;
    // esp is the builder's own, at its epilogue: [esp] is the row it just filled (its `this`,
    // parked at [ebp-0x14] and never moved).
    if (IsBadReadPtr((void*)(uintptr_t)esp, 4)) return;
    uint32_t row = *(uint32_t*)(uintptr_t)esp;
    if (!row || IsBadReadPtr((void*)(uintptr_t)(row + OFF_ROW_FT), 8)) return;
    ULONGLONG shown = *(ULONGLONG*)(uintptr_t)(row + OFF_ROW_FT);
    if (!shown) return;
    for (int i = 0; i < 100; i++) {
        if (!g_dateRow[i].valid || g_dateRow[i].stampLocal != shown) continue;
        FILETIME ut, lt; SYSTEMTIME st;
        ut.dwLowDateTime = (DWORD)g_dateRow[i].real;
        ut.dwHighDateTime = (DWORD)(g_dateRow[i].real >> 32);
        if (!FileTimeToLocalFileTime(&ut, &lt) || !FileTimeToSystemTime(&lt, &st)) return;
        if (IsBadWritePtr((void*)(uintptr_t)(row + OFF_ROW_STR), DATE_STR_CAP)) return;
        ((void (__cdecl*)(char*, unsigned, const SYSTEMTIME*))(uintptr_t)(g_base + RVA_DATE_FMT))
            ((char*)(uintptr_t)(row + OFF_ROW_STR), DATE_STR_CAP, &st);
        return;
    }
}

static void date_hook_install(void) {
    if (g_dateHooked) return;
    if (wait_for_bytes(g_base + RVA_DATE_RET, P_839626, (int)sizeof P_839626, 15000) < 0) {
        ff13loga("[FF13-SRP] save sort: the date builder never appeared -- the list will show the "
                 "sort order in its date column\n");
        return;
    }
    void* c = ff13_install_esp_hook(g_base + RVA_DATE_RET, P_839626, (int)sizeof P_839626,
                                    (void*)on_date_built);
    if (c) g_dateHooked = 1;
    (c ? ff13log : ff13loga)("[FF13-SRP] save sort: real-date hook %s\n", c ? "installed" : "FAILED");
}

static void datesidecar_save(void) {
    char path[MAX_PATH]; sidecar_path_for(path, sizeof(path), "ff13-srpractice_dates.dat");
    if (!path[0]) return;
    // A save's true date lives in nothing but its mtime, and a sort writes over that: this file is
    // the only copy of it, hence a generation of backup on every write.
    char bak[MAX_PATH]; sidecar_path_for(bak, sizeof bak, "ff13-srpractice_dates.bak");
    if (bak[0]) CopyFileA(path, bak, FALSE);
    FILE* f = fopen(path, "w"); if (!f) return;
    // Halves rather than one 64-bit conversion: this CRT's printf is not relied on for %ll.
    for (int s = 0; s < 100; s++)
        if (g_dateRow[s].valid)
            fprintf(f, "%d %u %u %u %u\n", s,
                    (unsigned)g_dateRow[s].stamp, (unsigned)(g_dateRow[s].stamp >> 32),
                    (unsigned)g_dateRow[s].real,  (unsigned)(g_dateRow[s].real  >> 32));
    fclose(f);
}
static void datesidecar_load(void) {
    char path[MAX_PATH]; sidecar_path_for(path, sizeof(path), "ff13-srpractice_dates.dat");
    if (!path[0]) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    int slot, n = 0, junk = 0; unsigned sl, sh, rl, rh;
    while (fscanf(f, "%d %u %u %u %u", &slot, &sl, &sh, &rl, &rh) == 5) {
        if (slot < 0 || slot >= 100) continue;
        ULONGLONG real = ((ULONGLONG)rh << 32) | rl;
        if (stamp_2001(real)) { junk++; continue; }
        g_dateRow[slot].stamp = ((ULONGLONG)sh << 32) | sl;
        g_dateRow[slot].real  = real;
        g_dateRow[slot].stampLocal = ft_local(g_dateRow[slot].stamp);
        g_dateRow[slot].valid = 1; n++;
    }
    fclose(f);
    g_dateRows = n;
    if (n) ff13log("[FF13-SRP] save sort: %d real save dates carried over\n", n);
    if (junk) {
        ff13loga("[FF13-SRP] save sort: %d row(s) held a sort stamp instead of a real date and were "
                 "dropped -- those saves show the sort's own date until they are saved over\n", junk);
        datesidecar_save();
    }
}

static int ss_collect(ss_ent* e, int mode) {
    static uint8_t buf[FF13_FILE_LEN];
    int n = 0;
    for (int slot = 0; slot < SS_SLOTS; slot++) {
        char path[MAX_PATH];
        if (!save_path_for(slot, path, sizeof(path))) return 0;
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        size_t rd = fread(buf, 1, FF13_FILE_LEN, f); fclose(f);
        if (rd != FF13_FILE_LEN) {
            ff13logv("[FF13-SRP] save sort: slot %d bad size %u -- left where it is\n",
                     slot, (unsigned)rd);
            continue;
        }
        uint32_t key;
        int leads = 0;
        if (mode == 2) {
            key = (uint32_t)slot;
        } else {
            ff13_decrypt(buf, rd);
            key = ((((uint32_t)buf[SS_OFF_K1]) | ((uint32_t)buf[SS_OFF_K1 + 1] << 8)) << 8)
                  | buf[SS_OFF_K2];
            char cp[FF13_NAME_WIDTH + 1];
            memcpy(cp, buf + FF13_NAME_OFF, FF13_NAME_WIDTH); cp[FF13_NAME_WIDTH] = 0;
            leads = chapter_save_leads_to(cp);
        }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        ULONGLONG cur = 0;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) cur = ft_u64(fa.ftLastWriteTime);
        // Only the 2001 grid is refused on sight: a minute-aligned mtime the sidecar cannot
        // vouch for is far more often a coarse-storage REAL date than an orphaned stamp, and
        // adopting it keeps that date; refusing it would hand the save the sort's own.
        ULONGLONG real = (g_dateRow[slot].valid && g_dateRow[slot].stamp == cur) ? g_dateRow[slot].real
                       : stamp_2001(cur)                                         ? 0
                       : cur;
        if (!real)
            ff13log("[FF13-SRP] save sort: slot %d wears a stamp this session cannot place -- its "
                    "real date is unknown and the list will show the sort's own\n", slot);
        e[n].slot = slot; e[n].key = key; e[n].real = real;
        e[n].leads = (unsigned char)leads; n++;
    }
    return n;
}

// Stamps count back from the sort's own minute, so the list wears the date the sort ran -- and
// a save written after it still lands on top as newest.
static int ss_stamp(ss_ent* e, int n, int descend) {
    g_ssDescend = descend;
    qsort(e, (size_t)n, sizeof e[0], ss_cmp);
    int done = 0;
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG top = ft_u64(nowFt) / SS_STEP * SS_STEP;
    g_dateRows = 0;                       // the hook stands down while the table is rebuilt
    memset(g_dateRow, 0, sizeof g_dateRow);
    for (int i = 0; i < n; i++) {
        unsigned long long t = top - (unsigned long long)i * SS_STEP;
        FILETIME ft; ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
        char path[MAX_PATH];
        save_path_for(e[i].slot, path, sizeof(path));
        HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            ff13loga("[FF13-SRP] save sort: slot %d open fail (err %lu)\n",
                     e[i].slot, GetLastError());
            continue;
        }
        if (SetFileTime(h, NULL, NULL, &ft)) {
            done++;
            if (e[i].real) {          // no real date -> no row: the list shows the stamp instead
                g_dateRow[e[i].slot].stamp      = t;
                g_dateRow[e[i].slot].stampLocal = ft_local(t);
                g_dateRow[e[i].slot].real       = e[i].real;
                g_dateRow[e[i].slot].valid      = 1;
            }
        } else ff13loga("[FF13-SRP] save sort: slot %d stamp fail (err %lu)\n",
                        e[i].slot, GetLastError());
        CloseHandle(h);
    }
    g_dateRows = done;                    // published last: the rows are in place before it reads
    datesidecar_save();
    date_hook_install();
    return done;
}

void save_sort_apply(void) {
    static ss_ent e[100];
    int mode = g_ssSel - SS_STORY_ASC;    // menu row -> the sort mode: 0 asc, 1 desc, 2 by number
    int n = ss_collect(e, mode);
    if (!n) { ff13loga("[FF13-SRP] save sort: no saves found\n"); return; }
    int done = ss_stamp(e, n, mode == 1);
    char msg[96];
    snprintf(msg, sizeof msg, ui_text(T_SS_DONE), done);
    flash_say(msg);
    ff13log("[FF13-SRP] save sort: mode %d, %d/%d saves re-stamped\n", mode, done, n);
}

// ---- filter: show one chapter's saves in the game's own list -------------------------------
// Rows are dropped from the array right before the game sorts it (0x838EA0). Do NOT hide the files
// instead: the browser's free-number set (list+0x18, seeded 0..98 at 0x8387B0) only loses a number
// to a save it manages to LOAD (0x838A70), so the next save would land straight on a hidden one.
#define RVA_SAVELIST_SORT 0x438EA0   // VA 0x838EA0: qsort(base=[this+8], [this+0x14], 0x274, cmp)
#define OFF_LIST_BASE  0x08
#define OFF_LIST_COUNT 0x14          // signed byte
#define OFF_LIST_CUR   0x15          // signed byte, indexes the same array
#define ROW_BYTES      0x274
static const uint8_t P_838EA0[] = {0x55,0x8B,0xEC,0x83,0xEC,0x08};

volatile int g_filterChapter = 0;            // 0 = show everything, else the chapter to keep
static unsigned char g_slotChapter[SS_SLOTS];  // 0 = unknown -> always shown
static volatile int g_filterHooked = 0;

// Only the list you pick a save FROM (STATE_LOAD), never the one you pick a slot to overwrite.

static void __cdecl on_savelist_sort(uint32_t list) {
    int want = g_filterChapter;
    if (!want || !list || IsBadReadPtr((void*)(uintptr_t)(list + OFF_LIST_CUR), 1)) return;
    if (main_state() != MAIN_STATE_LOAD) return;
    uint32_t base = *(uint32_t*)(uintptr_t)(list + OFF_LIST_BASE);
    int count = *(signed char*)(uintptr_t)(list + OFF_LIST_COUNT);
    if (!base || count <= 0) return;
    if (IsBadWritePtr((void*)(uintptr_t)base, (size_t)count * ROW_BYTES)) return;
    int w = 0;
    for (int r = 0; r < count; r++) {
        uint32_t row = base + (uint32_t)r * ROW_BYTES;
        int slot = *(unsigned char*)(uintptr_t)(row + OFF_ROW_SLOT);
        int keep = (slot < 0 || slot >= SS_SLOTS) || !g_slotChapter[slot]
                   || g_slotChapter[slot] == (unsigned char)want;
        if (!keep) continue;
        if (w != r) memcpy((void*)(uintptr_t)(base + (uint32_t)w * ROW_BYTES),
                           (const void*)(uintptr_t)row, ROW_BYTES);
        w++;
    }
    if (w == count) return;
    *(signed char*)(uintptr_t)(list + OFF_LIST_COUNT) = (signed char)w;
    signed char cur = *(signed char*)(uintptr_t)(list + OFF_LIST_CUR);
    if (cur >= w) *(signed char*)(uintptr_t)(list + OFF_LIST_CUR) = (signed char)(w ? w - 1 : 0);
}

static void filter_hook_install(void) {
    if (g_filterHooked) return;
    if (wait_for_bytes(g_base + RVA_SAVELIST_SORT, P_838EA0, (int)sizeof P_838EA0, 15000) < 0) {
        ff13loga("[FF13-SRP] filter: the save list's sort never appeared -- filtering is off\n");
        return;
    }
    void* c = ff13_install_ecx_hook(g_base + RVA_SAVELIST_SORT, P_838EA0, (int)sizeof P_838EA0,
                                    (void*)on_savelist_sort);
    if (c) g_filterHooked = 1;
    (c ? ff13log : ff13loga)("[FF13-SRP] filter: save-list hook %s\n", c ? "installed" : "FAILED");
}

// The chapter is the title's SECOND run of digits, in both languages
// ("セーブデータ 90, チャプター 4" / "Save Data 90, Chapter 4").
void filter_scan(int* counts, int n) {
    memset(g_slotChapter, 0, sizeof g_slotChapter);
    if (counts) memset(counts, 0, sizeof(int) * (size_t)n);
    for (int slot = 0; slot < SS_SLOTS; slot++) {
        char path[MAX_PATH];
        if (!save_path_for(slot, path, sizeof(path))) return;
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        wchar_t line[FF13_LIST_CAP];
        char cp[FF13_NAME_WIDTH + 1];
        int ok = fseek(f, FF13_LIST_OFF, SEEK_SET) == 0
                 && fread(line, sizeof(wchar_t), FF13_LIST_CAP, f) == FF13_LIST_CAP;
        int okCp = checkpoint_read(f, cp);
        fclose(f);
        if (!ok) continue;
        size_t len = 0; while (len < FF13_LIST_CAP && line[len]) len++;
        int runs = 0, ch = 0;
        for (size_t i = 0; i < len; ) {
            if (line[i] < L'0' || line[i] > L'9') { i++; continue; }
            int v = 0;
            while (i < len && line[i] >= L'0' && line[i] <= L'9') v = v * 10 + (line[i++] - L'0');
            if (++runs == 2) { ch = v; break; }
        }
        int leads = okCp ? chapter_save_leads_to(cp) : 0;
        if (leads) ch = leads;
        if (ch > 0 && ch < 256) {
            g_slotChapter[slot] = (unsigned char)ch;
            if (counts && ch < n) counts[ch]++;
        }
    }
}

void filter_apply(int chapter) {
    g_filterChapter = chapter;
    if (chapter) filter_hook_install();
    ff13log("[FF13-SRP] filter: %s\n", chapter ? "chapter set" : "off (every save shown)");
}

// A chapter with no saves is never offered: an empty list is not a state to hand the game.
#define FILTER_CH_MAX 32
static int g_chCount[FILTER_CH_MAX];
void filter_menu_refresh(void) { filter_scan(g_chCount, FILTER_CH_MAX); }
int  filter_count_of(int chapter) {
    return (chapter > 0 && chapter < FILTER_CH_MAX) ? g_chCount[chapter] : 0;
}
// The scan has to come first: g_slotChapter is what the hook reads, and an unscanned save is never
// hidden. A chapter holding no save leaves the hook UNINSTALLED rather than zeroing the setting, so
// a briefly unreadable save folder does not forget what was chosen.
void filter_restore(void) {
    int want = g_filterChapter;
    if (want <= 0 || want >= FILTER_CH_MAX) { g_filterChapter = 0; return; }
    filter_menu_refresh();
    if (filter_count_of(want) <= 0) {
        ff13log("[FF13-SRP] filter: chapter %d holds no save now -- the whole list is shown\n", want);
        return;
    }
    filter_hook_install();
    ff13log("[FF13-SRP] filter: chapter %d restored (%d save(s))\n", want, filter_count_of(want));
}

int filter_cycle(int dir) {
    int list[FILTER_CH_MAX + 1], n = 0, at = 0;
    list[n++] = 0;
    for (int c = 1; c < FILTER_CH_MAX; c++) if (g_chCount[c] > 0) list[n++] = c;
    for (int i = 0; i < n; i++) if (list[i] == g_filterChapter) at = i;
    at = (at + (dir > 0 ? 1 : n - 1)) % n;
    filter_apply(list[at]);
    return list[at];
}

// ---- renumber: give the saves numbers that follow the story -------------------------------
// Between the two rename passes every save sits under a temp name and the game would show an EMPTY
// list, so the journal is written before the first rename and dropped only once the last one lands.
// Sidecars and header numbers come after that, where a failure costs a restore point, not a save.
#define RENUM_TMP ".renum"

static int renum_journal_path(char* p, size_t n) {
    sidecar_path_for(p, n, "ff13-srpractice_renum.log");
    return p[0] != 0;
}
static int renum_tmp_for(int slot, int bak, char* out, size_t n) {
    char base[MAX_PATH];
    if (bak ? !bak_path_for(slot, base, sizeof(base)) : !save_path_for(slot, base, sizeof(base)))
        return 0;
    snprintf(out, n, "%s" RENUM_TMP, base);
    return 1;
}

// One swap for every slot-keyed table in this file: a table added later is added HERE.
static void renum_slot_swap(int a, int b) {
    { sidecar_t t = g_sidecar[a];   g_sidecar[a] = g_sidecar[b];     g_sidecar[b] = t; }
    { int t = g_slotMapN[a];        g_slotMapN[a] = g_slotMapN[b];   g_slotMapN[b] = t; }
    for (int i = 0; i < MAP_MAX; i++)
        for (int j = 0; j < 2; j++)
            { int t = g_slotMap[a][i][j]; g_slotMap[a][i][j] = g_slotMap[b][i][j]; g_slotMap[b][i][j] = t; }
    for (int i = 0; i < 16; i++)
        { unsigned char t = g_slotA18[a][i]; g_slotA18[a][i] = g_slotA18[b][i]; g_slotA18[b][i] = t; }
    { int t = g_slotHasA18[a];      g_slotHasA18[a] = g_slotHasA18[b];   g_slotHasA18[b] = t; }
    { uint32_t t = g_slotLoc[a];    g_slotLoc[a] = g_slotLoc[b];         g_slotLoc[b] = t; }
    { int t = g_slotHasLoc[a];      g_slotHasLoc[a] = g_slotHasLoc[b];   g_slotHasLoc[b] = t; }
    { uint8_t t = g_bakKnown[a];    g_bakKnown[a] = g_bakKnown[b];       g_bakKnown[b] = t; }
    { date_row t = g_dateRow[a];    g_dateRow[a] = g_dateRow[b];         g_dateRow[b] = t; }
}

// perm[i] = where slot i's data has to end up; perm is consumed in the process.
static void renum_permute(unsigned char* perm) {
    for (int i = 0; i < 100; i++) {
        while (perm[i] != i) {
            int j = perm[i];
            renum_slot_swap(i, j);
            unsigned char t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
    }
    // Bound by SS_SLOTS, never by the pair count: the old bound wiped every table above it, and
    // recovery has no ss_stamp rebuild to mask that (measured).
    for (int s = 0; s < SS_SLOTS; s++) {
        if (slot_has_file(s)) continue;
        memset(&g_sidecar[s], 0, sizeof g_sidecar[s]);
        g_slotMapN[s] = 0; g_slotHasA18[s] = 0; g_slotHasLoc[s] = 0;
        g_bakKnown[s] = 0;
        memset(&g_dateRow[s], 0, sizeof g_dateRow[s]);
    }
    sidecar_save_file(); mapsidecar_save(); datesidecar_save();
}

// The printed number is the title's FIRST run of digits. The list_mark lands right behind it,
// which is why config_load refuses a mark containing a digit -- that is what keeps this run the
// number and nothing else.
static void header_renumber_at(const char* path, int slot) {
    // The rewrite must not touch the mtime: it is the list order and the save's real date, and
    // recovery has no re-stamp behind it to hide a change (measured).
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int haveTime = mtime_keep(path, &fa);
    FILE* f = fopen(path, "r+b");
    if (!f) { ff13loga("[FF13-SRP] renumber: slot %d header open fail\n", slot); return; }
    wchar_t line[FF13_LIST_CAP];
    if (fseek(f, FF13_LIST_OFF, SEEK_SET) != 0 ||
        fread(line, sizeof(wchar_t), FF13_LIST_CAP, f) != FF13_LIST_CAP) { fclose(f); return; }
    size_t len = 0; while (len < FF13_LIST_CAP && line[len]) len++;
    if (!len || len == FF13_LIST_CAP) {
        fclose(f); ff13loga("[FF13-SRP] renumber: slot %d has no readable title\n", slot); return;
    }
    size_t a = 0; while (a < len && (line[a] < L'0' || line[a] > L'9')) a++;
    if (a == len) { fclose(f); return; }
    size_t b = a; while (b < len && line[b] >= L'0' && line[b] <= L'9') b++;
    char nb[8]; snprintf(nb, sizeof nb, "%02d", slot + 1);   // the game's own width
    wchar_t num[8]; size_t nl = 0;
    for (const char* q = nb; *q; q++) num[nl++] = (wchar_t)*q;
    size_t nlen = len - (b - a) + nl;
    if (nlen + 1 > FF13_LIST_CAP) { fclose(f); return; }
    memmove(line + a + nl, line + b, (len - b + 1) * sizeof(wchar_t));
    memcpy(line + a, num, nl * sizeof(wchar_t));
    if (fseek(f, FF13_LIST_OFF, SEEK_SET) != 0 ||
        fwrite(line, sizeof(wchar_t), nlen + 1, f) != nlen + 1)
        ff13loga("[FF13-SRP] renumber: slot %d header write fail\n", slot);
    fclose(f);
    if (haveTime) mtime_put_back(path, &fa);
}
static void header_renumber(int slot) {
    char path[MAX_PATH];
    if (save_path_for(slot, path, sizeof(path))) header_renumber_at(path, slot);
}

static int renum_tmps_left(void) {
    for (int s = 0; s < 100; s++) {
        char t[MAX_PATH];
        if (renum_tmp_for(s, 0, t, sizeof t) && file_exists(t)) return 1;
    }
    return 0;
}
static int renum_pending(void) {
    char p[MAX_PATH];
    if (renum_journal_path(p, sizeof p) && file_exists(p)) return 1;
    return renum_tmps_left();
}
// Every slot-keyed table in one sweep; the caller decides what (if anything) reloads them.
static void slot_tables_clear(void) {
    memset(g_sidecar, 0, sizeof g_sidecar);
    memset(g_slotMapN, 0, sizeof g_slotMapN);
    memset(g_slotMap, 0, sizeof g_slotMap);
    memset(g_slotA18, 0, sizeof g_slotA18);
    memset(g_slotHasA18, 0, sizeof g_slotHasA18);
    memset(g_slotLoc, 0, sizeof g_slotLoc);
    memset(g_slotHasLoc, 0, sizeof g_slotHasLoc);
    memset(g_bakKnown, 0, sizeof g_bakKnown);
    memset(g_dateRow, 0, sizeof g_dateRow);
    g_dateRows = 0;
}

// Undo pass 1: every surviving temp goes back under its own old name. Needs no journal -- the
// temp's name carries the slot it came from -- so even a torn journal can be rolled back.
static int renum_rollback_tmps(void) {
    int back = 0;
    for (int s = 0; s < 100; s++) {
        char t[MAX_PATH], p[MAX_PATH];
        if (renum_tmp_for(s, 0, t, sizeof t) && file_exists(t)
            && save_path_for(s, p, sizeof p) && MoveFileA(t, p)) back++;
        if (renum_tmp_for(s, 1, t, sizeof t) && file_exists(t)
            && bak_path_for(s, p, sizeof p)) MoveFileA(t, p);
    }
    return back;
}
// Appended once every source sits under a temp name. A journal without this line means pass 1
// was cut short: the only safe forward from there is BACK -- landing would be guesswork.
static int renum_mark_lifted(void) {
    char jp[MAX_PATH];
    if (!renum_journal_path(jp, sizeof jp)) return 0;
    FILE* f = fopen(jp, "a"); if (!f) return 0;
    int ok = fputs("lifted\n", f) >= 0 && fflush(f) == 0;
    fclose(f);
    return ok;
}

// Pass 2: every save is under its temp name; put each one down as its new number.
static int renum_land(const unsigned char* from, const unsigned char* to, int n) {
    int moved = 0;
    for (int i = 0; i < n; i++) {
        char tmp[MAX_PATH], dst[MAX_PATH];
        if (renum_tmp_for(from[i], 0, tmp, sizeof tmp) && save_path_for(to[i], dst, sizeof dst)) {
            if (file_exists(tmp)) {
                if (MoveFileA(tmp, dst)) moved++;
                else ff13loga("[FF13-SRP] renumber: %d -> %d failed (err %lu)\n",
                              from[i], to[i], GetLastError());
            }
        }
        if (renum_tmp_for(from[i], 1, tmp, sizeof tmp) && bak_path_for(to[i], dst, sizeof dst)) {
            if (file_exists(tmp) && !MoveFileA(tmp, dst))
                ff13loga("[FF13-SRP] renumber: .bak %d -> %d failed (err %lu)\n",
                         from[i], to[i], GetLastError());
        }
    }
    return moved;
}

static int renum_finish(const unsigned char* from, const unsigned char* to, int n) {
    renum_land(from, to, n);
    // A temp that survived the landing is a save the game cannot see: keep the journal for the
    // next start rather than permute sidecars around files that did not move.
    for (int i = 0; i < n; i++) {
        char t[MAX_PATH];
        if (renum_tmp_for(from[i], 0, t, sizeof t) && file_exists(t)) {
            ff13loga("[FF13-SRP] renumber: slot %d could not be put down -- kept for the next "
                     "start\n", from[i]);
            return 0;
        }
    }
    unsigned char perm[100];
    for (int i = 0; i < 100; i++) perm[i] = (unsigned char)i;
    for (int i = 0; i < n; i++) perm[from[i]] = to[i];
    // renum_permute's cycle walk needs a true permutation: a gap in the numbers makes some slot
    // receive without sending, and the walk never terminates (measured: hung the worker thread).
    // The leftovers are paired up to close it -- those slots held no save, so where they land
    // does not matter.
    {
        unsigned char isSrc[100] = {0}, isDst[100] = {0};
        for (int i = 0; i < n; i++) { isSrc[from[i]] = 1; isDst[to[i]] = 1; }
        int d = 0;
        for (int s = 0; s < 100; s++) {
            if (!isDst[s] || isSrc[s]) continue;
            while (d < 100 && (isDst[d] || !isSrc[d])) d++;
            if (d >= 100) break;
            perm[s] = (unsigned char)d;
            isSrc[s] = 1; isDst[d] = 1;
        }
    }
    renum_permute(perm);
    // Deleted BEFORE the header loop: replaying the permutation squares it, while a header the
    // crash left stale is idempotent to rewrite and sidecar_align_check reports it.
    char jp[MAX_PATH];
    if (renum_journal_path(jp, sizeof jp)) DeleteFileA(jp);
    for (int i = 0; i < n; i++) header_renumber(to[i]);
    return 1;
}

// A leftover journal means the renames were cut short: finish them before the game draws a list.
static void renum_recover(void) {
    char jp[MAX_PATH];
    if (!renum_journal_path(jp, sizeof jp)) return;
    FILE* f = fopen(jp, "r"); if (!f) return;
    unsigned char from[100], to[100];
    char w[16] = {0};
    int n = 0, a, b, wasLifted = 0;
    while (n < 100 && fscanf(f, "%d %d", &a, &b) == 2) {
        if (a < 0 || a > 99 || b < 0 || b > 99) { n = 0; break; }
        from[n] = (unsigned char)a; to[n] = (unsigned char)b; n++;
    }
    if (n && fscanf(f, "%15s", w) == 1 && strcmp(w, "lifted") == 0) wasLifted = 1;
    fclose(f);
    // No "lifted" line = pass 1 (or the journal itself) was cut short. Landing then would move
    // temps onto slots that were never emptied; rolling back never guesses.
    if (!wasLifted) {
        int back = renum_rollback_tmps();
        if (renum_tmps_left()) {
            ff13loga("[FF13-SRP] renumber: the rollback is still short (%d put back) -- retried "
                     "on the next start\n", back);
            return;
        }
        DeleteFileA(jp);
        ff13loga("[FF13-SRP] renumber: an interrupted run was rolled back (%d save(s) put back) "
                 "-- run it again\n", back);
        return;
    }
    ff13log("[FF13-SRP] renumber: finishing an interrupted run (%d saves)\n", n);
    if (!renum_finish(from, to, n)) flash_say_c(ui_text(T_SS_RENUM_PART), OVL_FLASH_OFF);
}

// The printed number lives INSIDE the file and survives a rename, so it is the one witness that a
// restore point keyed by the old number no longer belongs to the save sitting there.
static void sidecar_align_check(void) {
    int bad = 0;
    for (int slot = 0; slot < SS_SLOTS; slot++) {
        if (!g_sidecar[slot].valid) continue;
        wchar_t line[FF13_LIST_CAP]; size_t len;
        if (!title_read(slot, line, &len)) {
            ff13loga("[FF13-SRP] sidecar: slot %d holds a restore point but no save\n", slot);
            bad++; continue;
        }
        size_t a = 0; while (a < len && (line[a] < L'0' || line[a] > L'9')) a++;
        if (a == len) continue;
        int n = 0;
        while (a < len && line[a] >= L'0' && line[a] <= L'9') n = n * 10 + (int)(line[a++] - L'0');
        if (n == slot + 1) continue;
        ff13loga("[FF13-SRP] sidecar: slot %d holds a restore point, but the save there calls itself "
                 "%d -- it was renumbered and this entry was not, so loading it would put you where "
                 "save %d used to be\n", slot, n, slot + 1);
        bad++;
    }
    if (bad)
        ff13loga("[FF13-SRP] sidecar: %d restore point(s) out of step with the saves -- the save "
                 "hotkey on those slots writes a correct one again\n", bad);
}

void save_renumber_apply(void) {
    // The open list's rows keep their slot numbers; renaming under it hands the cursor another
    // save without any refusal from the game (the sort is safe there -- it only re-stamps).
    if (main_state() == MAIN_STATE_LOAD) {
        ff13log("[FF13-SRP] renumber: refused -- the load list is open\n");
        flash_say_c(ui_text(T_SS_RENUM_LIST), OVL_FLASH_OFF);
        return;
    }
    // Re-running would truncate the kept journal over live temps (its pairs may differ).
    if (renum_pending()) {
        ff13log("[FF13-SRP] renumber: refused -- an earlier run still has to finish\n");
        flash_say_c(ui_text(T_SS_RENUM_PART), OVL_FLASH_OFF);
        return;
    }
    static ss_ent e[100];
    int n = ss_collect(e, 0);
    if (!n) { ff13loga("[FF13-SRP] renumber: no saves found\n"); return; }
    g_ssDescend = 0;
    qsort(e, (size_t)n, sizeof e[0], ss_cmp);
    unsigned char from[100], to[100];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (e[i].slot != i) { from[m] = (unsigned char)e[i].slot; to[m] = (unsigned char)i; m++; }
    if (!m) {
        ff13log("[FF13-SRP] renumber: the numbers already follow the story\n");
    } else {
        char jp[MAX_PATH];
        if (!renum_journal_path(jp, sizeof jp)) { ff13loga("[FF13-SRP] renumber: no data dir\n"); return; }
        FILE* f = fopen(jp, "w");
        if (!f) { ff13loga("[FF13-SRP] renumber: cannot write the journal -- not starting\n"); return; }
        for (int i = 0; i < m; i++) fprintf(f, "%d %d\n", from[i], to[i]);
        fflush(f); fclose(f);
        // Pass 1: everything that moves goes to a temp name first, so no rename can land on a
        // save that has not been picked up yet.
        for (int i = 0; i < m; i++) {
            char src[MAX_PATH], tmp[MAX_PATH];
            if (save_path_for(from[i], src, sizeof src) && renum_tmp_for(from[i], 0, tmp, sizeof tmp)
                && MoveFileA(src, tmp)) {
                char bsrc[MAX_PATH], btmp[MAX_PATH];
                if (bak_path_for(from[i], bsrc, sizeof bsrc) && file_exists(bsrc)
                    && renum_tmp_for(from[i], 1, btmp, sizeof btmp)) MoveFileA(bsrc, btmp);
                continue;
            }
            ff13loga("[FF13-SRP] renumber: slot %d could not be lifted (err %lu) -- rolling back\n",
                     from[i], GetLastError());
            renum_rollback_tmps();
            if (renum_tmps_left()) {          /* no "lifted" line: the next start rolls back too */
                flash_say_c(ui_text(T_SS_RENUM_PART), OVL_FLASH_OFF);
            } else {
                DeleteFileA(jp);
                flash_say_c(ui_text(T_SS_RENUM_FAIL), OVL_FLASH_OFF);
            }
            return;
        }
        if (!renum_mark_lifted()) {
            renum_rollback_tmps();
            if (renum_tmps_left()) {
                flash_say_c(ui_text(T_SS_RENUM_PART), OVL_FLASH_OFF);
            } else {
                DeleteFileA(jp);
                flash_say_c(ui_text(T_SS_RENUM_FAIL), OVL_FLASH_OFF);
            }
            return;
        }
        if (!renum_finish(from, to, m)) {
            // Some files landed under new numbers while every slot-keyed table still says the
            // old ones: serve nothing stale until the next start finishes the run.
            slot_tables_clear();
            flash_say_c(ui_text(T_SS_RENUM_PART), OVL_FLASH_OFF);
            return;
        }
    }
    // e[] was collected before the renames, so each entry still carries that save's real date.
    for (int i = 0; i < n; i++) e[i].slot = i;
    ss_stamp(e, n, 0);
    char msg[96];
    snprintf(msg, sizeof msg, ui_text(T_SS_RENUM_DONE), m);
    flash_say(msg);
    ff13log("[FF13-SRP] renumber: %d of %d saves renumbered into story order\n", m, n);
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
    int slot, m, slots = 0, stale = 0;
    while (fscanf(f, "%d %d", &slot, &m) == 2) {
        if (slot < 0 || slot >= 256 || m < 0) { fclose(f); return; }   // corrupt -> stop
        if (m > MAP_MAX) m = MAP_MAX;
        int n = 0;
        for (int i = 0; i < m; i++) {
            int lo, mp; if (fscanf(f, "%d %d", &lo, &mp) != 2) { m = i; break; }
            g_slotMap[slot][n][0] = lo; g_slotMap[slot][n][1] = mp; n++;
        }
        // A row without a live hotkey pos is a leftover of a save that was saved over: dropped.
        if (n && g_sidecar[slot].valid) { g_slotMapN[slot] = n; slots++; }
        else if (n) stale++;
    }
    fclose(f);
    ff13log("[FF13-SRP] mapset sidecar loaded: %d slots (%d stale row(s) dropped)\n", slots, stale);
}

// ---- save categories: more than one save directory, swapped without restarting the game -------
// The category IN USE is the game's own save folder; every other one is parked under
// <save dir>\categories\<name>\ with copies of this mod's three per-slot sidecars.
#define CATS_DIR "categories"
#define CATNAME_BYTES 64            // CATNAME_MAX characters at CP_ACP's worst, and the NUL
// A switch runs on the title's top menu only: the save list keeps rows for the files it read when
// it opened, and moving those out from under it leaves the game offering saves that are gone.

static char g_catCur[CATNAME_BYTES] = "main";
static char g_catCurU8[CATNAME_BYTES * 3];
static char g_catList[CAT_MAX][CATNAME_BYTES];
static char g_catListU8[CAT_MAX][CATNAME_BYTES * 3];
static int  g_catListN = 0;
static volatile int g_catSel = -1;   // index into the list; -1 = no switch target picked

// Paths here are CP_ACP and the overlay draws UTF-8, so each name is kept in both forms rather
// than converted on the drawing thread.
static void catname_to_u8(char* out, size_t n, const char* ansi) {
    wchar_t w[CATNAME_BYTES];
    if (MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, (int)(sizeof w / sizeof w[0])) > 0
        && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) > 0) return;
    snprintf(out, n, "%s", ansi);
}
static void catname_trim(char* b) {
    size_t n = strlen(b);
    while (n && (unsigned char)b[n - 1] <= ' ') b[--n] = 0;
}
static void catname_set_cur(const char* ansi) {
    snprintf(g_catCur, sizeof g_catCur, "%s", ansi);
    catname_to_u8(g_catCurU8, sizeof g_catCurU8, g_catCur);
}

int cat_count(void) { return g_catListN; }
const char* cat_name_cur(void) { return g_catCurU8; }
const char* cat_name_sel(void) {
    int i = g_catSel;
    return (i >= 0 && i < g_catListN) ? g_catListU8[i] : NULL;
}
const char* cat_pick(void) {
    int i = g_catSel;
    return (i >= 0 && i < g_catListN) ? g_catList[i] : NULL;
}

static int catname_file(char* p, size_t n) {
    sidecar_path_for(p, n, "ff13-srpractice_category.txt");
    return p[0] != 0;
}
static void catname_load(void) {
    catname_to_u8(g_catCurU8, sizeof g_catCurU8, g_catCur);
    char p[MAX_PATH]; if (!catname_file(p, sizeof p)) return;
    FILE* f = fopen(p, "r"); if (!f) return;
    char b[CATNAME_BYTES];
    if (fgets(b, sizeof b, f)) { catname_trim(b); if (b[0]) catname_set_cur(b); }
    fclose(f);
}
static void catname_save(void) {
    char p[MAX_PATH]; if (!catname_file(p, sizeof p)) return;
    FILE* f = fopen(p, "w");
    if (!f) { ff13loga("[FF13-SRP] category: cannot write %s\n", p); return; }
    fprintf(f, "%s\n", g_catCur);
    fclose(f);
}

static int cats_root(char* out, size_t n) {
    char root[MAX_PATH];
    if (!save_root_dir(root, sizeof root)) return 0;
    snprintf(out, n, "%s\\" CATS_DIR, root);
    return 1;
}
static int cat_dir_of(char* out, size_t n, const char* name) {
    char cats[MAX_PATH];
    out[0] = 0;
    if (!name || !name[0] || !cats_root(cats, sizeof cats)) return 0;
    snprintf(out, n, "%s\\%s", cats, name);
    return 1;
}

static const char* const kCatSidecars[] = { "ff13-srpractice_slots.dat",
                                            "ff13-srpractice_maps.dat",
                                            "ff13-srpractice_dates.dat" };
#define CAT_SIDECARS ((int)(sizeof kCatSidecars / sizeof kCatSidecars[0]))
#define CAT_FILES (SS_SLOTS * 2 + CAT_SIDECARS)

// Two paths per index: the sidecars live beside the exe, not with the saves, yet a parked category
// keeps its copies next to the saves they belong to.
static int cat_file_pair(int i, const char* dir, char* act, size_t an, char* in, size_t bn) {
    if (i < SS_SLOTS) {
        if (!save_path_for(i, act, an)) return 0;
        snprintf(in, bn, "%s\\ff13-%02d.dat", dir, i);
        return 1;
    }
    if (i < SS_SLOTS * 2) {
        int s = i - SS_SLOTS;
        if (!bak_path_for(s, act, an)) return 0;
        snprintf(in, bn, "%s\\ff13-%02d.dat.bak", dir, s);
        return 1;
    }
    const char* nm = kCatSidecars[i - SS_SLOTS * 2];
    sidecar_path_for(act, an, nm);
    if (!act[0]) return 0;
    snprintf(in, bn, "%s\\%s", dir, nm);
    return 1;
}

// MOVEFILE_REPLACE_EXISTING is deliberately absent: a move onto a file that is already there fails
// and is logged, rather than writing over someone's save.
static int cat_move_all(const char* dir, int intoSet) {
    int moved = 0;
    for (int i = 0; i < CAT_FILES; i++) {
        char a[MAX_PATH], b[MAX_PATH];
        if (!cat_file_pair(i, dir, a, sizeof a, b, sizeof b)) continue;
        const char* src = intoSet ? a : b;
        const char* dst = intoSet ? b : a;
        if (!file_exists(src)) continue;
        if (MoveFileExA(src, dst, MOVEFILE_COPY_ALLOWED)) moved++;
        else ff13loga("[FF13-SRP] category: %s -> %s failed (err %lu)\n", src, dst, GetLastError());
    }
    return moved;
}

// How many of the files a park moves are still on the given side (0 = active dir, 1 = parked
// under `dir`); savesOnly narrows it to the ff13-NN.dat files themselves.
static int cat_files_at(const char* dir, int parkedSide, int savesOnly) {
    int n = 0, top = savesOnly ? SS_SLOTS : CAT_FILES;
    for (int i = 0; i < top; i++) {
        char a[MAX_PATH], b[MAX_PATH];
        if (cat_file_pair(i, dir, a, sizeof a, b, sizeof b) && file_exists(parkedSide ? b : a)) n++;
    }
    return n;
}

// phase 1 = the category in use is not parked yet; phase 2 = it is, and the target's files are on
// their way in. Replaying phase 1 over a half-finished phase 2 would park the TARGET's files under
// the old category's name.
static int cat_journal_path(char* p, size_t n) {
    sidecar_path_for(p, n, "ff13-srpractice_categories.jnl");
    return p[0] != 0;
}
static int cat_journal_write(int phase, const char* cur, const char* target) {
    char p[MAX_PATH], tmp[MAX_PATH];
    if (!cat_journal_path(p, sizeof p)) return 0;
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE* f = fopen(tmp, "w"); if (!f) return 0;
    int ok = fprintf(f, "phase %d\n%s\n%s\n", phase, cur, target) > 0 && fflush(f) == 0;
    fclose(f);
    // Swapped in whole: the phase-2 rewrite happens with every save parked, and a torn journal
    // there is a record the recovery would drop -- with the saves still parked.
    return ok && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING) != 0;
}
static void cat_journal_drop(void) {
    char p[MAX_PATH]; if (cat_journal_path(p, sizeof p)) DeleteFileA(p);
}

// Everything keyed by slot has just been replaced on disk. Same table list as renum_slot_swap.
static void cat_tables_reload(void) {
    slot_tables_clear();
    sidecar_load_file();
    mapsidecar_load();
    datesidecar_load();
    bak_scan();
    if (g_dateRows) date_hook_install();
    filter_menu_refresh();
    // An empty list is not a state to hand the game (the same guard filter_restore applies).
    if (g_filterChapter && filter_count_of(g_filterChapter) <= 0) {
        ff13log("[FF13-SRP] category: chapter %d holds no save here -- the whole list is shown\n",
                g_filterChapter);
        filter_apply(0);
        state_save();
    } else if (g_filterChapter) {
        // The setting may have been restored with no hook to serve it (its chapter was empty at
        // boot); this category can show it, so the hook has to be live now.
        filter_hook_install();
    }
}

static int cat_name_cmp(const void* A, const void* B) {
    const unsigned char* a = (const unsigned char*)A;
    const unsigned char* b = (const unsigned char*)B;
    for (; *a && *b; a++, b++) {
        int x = tolower(*a), y = tolower(*b);
        if (x != y) return x - y;
    }
    return (int)*a - (int)*b;
}

void cat_list_refresh(void) {
    g_catListN = 0;
    g_catSel = -1;
    char cats[MAX_PATH], pat[MAX_PATH];
    if (!cats_root(cats, sizeof cats)) return;
    snprintf(pat, sizeof pat, "%s\\*", cats);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (strlen(fd.cFileName) >= CATNAME_BYTES) continue;
        if (cfg_ieq(fd.cFileName, g_catCur)) continue;
        if (g_catListN >= CAT_MAX) break;
        snprintf(g_catList[g_catListN], CATNAME_BYTES, "%s", fd.cFileName);
        g_catListN++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_catList, (size_t)g_catListN, CATNAME_BYTES, cat_name_cmp);
    for (int i = 0; i < g_catListN; i++) {
        catname_to_u8(g_catListU8[i], sizeof g_catListU8[i], g_catList[i]);
        char dir[MAX_PATH], glob[MAX_PATH];
        int saves = 0;
        if (cat_dir_of(dir, sizeof dir, g_catList[i])) {
            snprintf(glob, sizeof glob, "%s\\ff13-*.dat", dir);
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(glob, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    // The glob also matches the parked sidecars, so only the exact ff13-NN.dat
                    // shape counts.
                    const char* n = fd.cFileName;
                    if (strlen(n) == 11 && n[5] >= '0' && n[5] <= '9' && n[6] >= '0' && n[6] <= '9'
                        && !_stricmp(n + 7, ".dat")) saves++;
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        size_t l = strlen(g_catListU8[i]);
        snprintf(g_catListU8[i] + l, sizeof g_catListU8[i] - l, ui_text(T_CAT_COUNT), saves);
    }
}

int cat_cycle(int dir) {
    if (g_catListN <= 0) return -1;
    int n = g_catListN + 1;                    // "nothing picked" is one of the stops
    int at = ((g_catSel + 1) + (dir > 0 ? 1 : n - 1)) % n;
    g_catSel = at - 1;
    return g_catSel;
}

int cat_switch(const char* target) {
    if (!target || !target[0] || cfg_ieq(target, g_catCur)) return 0;
    int st = main_state();
    if (st != MAIN_STATE_TITLE) {
        ff13log("[FF13-SRP] category: refused -- main state %d, not the title screen\n", st);
        flash_say_c(ui_text(T_SS_CAT_NOTITLE), OVL_FLASH_OFF);
        return 0;
    }
    // A half-finished renumber owns these files: its journal and temps do not travel with a
    // category, so switching would land one category's saves in another's folder next start.
    if (renum_pending()) {
        ff13loga("[FF13-SRP] category: refused -- a renumber is waiting to finish\n");
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    sidecar_save_file(); mapsidecar_save(); datesidecar_save();
    char cats[MAX_PATH], curDir[MAX_PATH], tgtDir[MAX_PATH];
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(curDir, sizeof curDir, g_catCur)
        || !cat_dir_of(tgtDir, sizeof tgtDir, target)) {
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    CreateDirectoryA(cats, NULL);
    if (!CreateDirectoryA(curDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ff13loga("[FF13-SRP] category: cannot make %s (err %lu)\n", curDir, GetLastError());
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    char prev[CATNAME_BYTES]; snprintf(prev, sizeof prev, "%s", g_catCur);
    if (!cat_journal_write(1, prev, target)) {
        ff13loga("[FF13-SRP] category: cannot write the journal -- not starting\n");
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    int out = cat_move_all(curDir, 1);
    // A file the park could not lift would leave two categories sharing the folder once the
    // target moves in: bring the parked ones back and stand down instead.
    int stuck = cat_files_at(curDir, 0, 0);
    if (stuck) {
        ff13loga("[FF13-SRP] category: %d file(s) would not park -- switching back\n", stuck);
        cat_move_all(curDir, 0);
        // Only a COMPLETE way back may drop the journal: with files still parked, it is the one
        // record recovery can act on.
        if (cat_files_at(curDir, 1, 0))
            ff13loga("[FF13-SRP] category: the way back is short too -- left for the next start\n");
        else
            cat_journal_drop();
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    if (!cat_journal_write(2, prev, target)) {
        ff13loga("[FF13-SRP] category: the journal would not advance -- putting '%s' back\n", prev);
        cat_move_all(curDir, 0);
        if (cat_files_at(curDir, 1, 0))
            ff13loga("[FF13-SRP] category: the way back is short too -- left for the next start\n");
        else
            cat_journal_drop();
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    int in = cat_move_all(tgtDir, 0);
    RemoveDirectoryA(tgtDir);
    catname_set_cur(target);
    catname_save();
    cat_journal_drop();
    cat_tables_reload();
    ff13log("[FF13-SRP] category: '%s' -> '%s' (%d out, %d in)\n", prev, g_catCur, out, in);
    cat_list_refresh();
    char msg[192];
    snprintf(msg, sizeof msg, ui_text(T_SS_CAT_DONE), g_catCurU8, out, in);
    flash_say(msg);
    return 1;
}

// A leftover journal means the moves were cut short: finish them before anything reads a save
// directory that is half one category and half another. Re-running a phase is safe.
static void cat_switch_recover(void) {
    char jp[MAX_PATH];
    if (!cat_journal_path(jp, sizeof jp)) return;
    FILE* f = fopen(jp, "r"); if (!f) return;
    char head[64] = {0}, cur[CATNAME_BYTES + 2] = {0}, tgt[CATNAME_BYTES + 2] = {0};
    int phase = 0;
    int ok = fgets(head, sizeof head, f) && sscanf(head, "phase %d", &phase) == 1
             && fgets(cur, sizeof cur, f) && fgets(tgt, sizeof tgt, f);
    fclose(f);
    catname_trim(cur); catname_trim(tgt);
    if (!ok || (phase != 1 && phase != 2) || !cur[0] || !tgt[0]) {
        // An unreadable journal over an empty save folder means the park happened and the record
        // died with it: the active category's own files come back before the journal goes.
        char backDir[MAX_PATH];
        if (cat_dir_of(backDir, sizeof backDir, g_catCur)
            && cat_files_at(backDir, 0, 1) == 0 && cat_files_at(backDir, 1, 1) > 0) {
            int back = cat_move_all(backDir, 0);
            RemoveDirectoryA(backDir);
            ff13loga("[FF13-SRP] category: '%s' was parked with no readable journal -- %d file(s) "
                     "brought back\n", g_catCur, back);
        }
        DeleteFileA(jp);
        ff13loga("[FF13-SRP] category: unreadable journal dropped\n");
        return;
    }
    char cats[MAX_PATH], curDir[MAX_PATH], tgtDir[MAX_PATH];
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(curDir, sizeof curDir, cur)
        || !cat_dir_of(tgtDir, sizeof tgtDir, tgt)) return;
    ff13log("[FF13-SRP] category: finishing an interrupted switch '%s' -> '%s' (phase %d)\n",
            cur, tgt, phase);
    if (phase == 1) {
        CreateDirectoryA(cats, NULL);
        if (!CreateDirectoryA(curDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            ff13loga("[FF13-SRP] category: cannot make %s (err %lu) -- left unfinished\n",
                     curDir, GetLastError());
            return;
        }
        cat_move_all(curDir, 1);
        if (cat_files_at(curDir, 0, 0)) {
            ff13loga("[FF13-SRP] category: the park is still short -- left unfinished\n");
            return;
        }
        if (!cat_journal_write(2, cur, tgt)) {
            ff13loga("[FF13-SRP] category: the journal would not advance -- left unfinished\n");
            return;
        }
    }
    cat_move_all(tgtDir, 0);
    RemoveDirectoryA(tgtDir);
    catname_set_cur(tgt);
    catname_save();
    cat_journal_drop();
}

// ---- naming a category -----------------------------------------------------------------------
// The DOS device names: a directory called any of these cannot be reached by the paths built here.
static int catname_reserved(const char* n) {
    static const char* const kDev[] = { "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    for (int i = 0; i < (int)(sizeof kDev / sizeof kDev[0]); i++)
        if (cfg_ieq(n, kDev[i])) return 1;
    return 0;
}

// 0 = usable, 1 = the name itself is refused, 2 = a category of that name already exists.
static int catname_commit_check(char* out, size_t n, int rename) {
    int a = 0, b = g_editLen;
    while (a < b && g_editBuf[a] == L' ') a++;
    while (b > a && g_editBuf[b - 1] == L' ') b--;
    if (b <= a) return 1;
    wchar_t w[EDIT_BUF_MAX];
    int m = 0;
    for (int i = a; i < b && m < EDIT_BUF_MAX - 1; i++) w[m++] = g_editBuf[i];
    w[m] = 0;
    for (int i = 0; i < m; i++) if (!catname_ok_char(w[i])) return 1;
    // Every path below is char, so a name CP_ACP cannot spell is a name this cannot use. Under a
    // UTF-8 ACP (the Windows "beta" option) the flag and the default-char probe are rejected
    // outright (documented) -- and nothing is unspellable there, so ask plainly.
    BOOL usedDefault = FALSE;
    int acpUtf8 = GetACP() == CP_UTF8;
    if (WideCharToMultiByte(CP_ACP, acpUtf8 ? 0 : WC_NO_BEST_FIT_CHARS, w, -1, out, (int)n, NULL,
                            acpUtf8 ? NULL : &usedDefault) <= 0 || usedDefault) return 1;
    size_t L = strlen(out);
    if (!L || out[L - 1] == '.' || out[L - 1] == ' ') return 1;
    if (!strcmp(out, ".") || !strcmp(out, "..") || catname_reserved(out)) return 1;
    if (!rename && cfg_ieq(out, g_catCur)) return 2;
    char dir[MAX_PATH];
    if (!cat_dir_of(dir, sizeof dir, out)) return 1;
    if (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES) return 2;
    return 0;
}

static int catname_edit_open(int mode) {
    if (g_labelEdit) return 0;
    if (!edit_hook_window()) return 0;
    g_editMode = mode;
    g_editBuf[0] = 0;
    if (mode == EDIT_CAT_RENAME
        && MultiByteToWideChar(CP_ACP, 0, g_catCur, -1, g_editBuf, EDIT_BUF_MAX) <= 0)
        g_editBuf[0] = 0;
    g_editBuf[EDIT_BUF_MAX - 1] = 0;
    g_editLen = (int)wcslen(g_editBuf);
    if (g_editLen > CATNAME_MAX) { g_editLen = CATNAME_MAX; g_editBuf[g_editLen] = 0; }
    g_labelEditSlot = -1;
    g_editDone = 0; g_editBad = 0;
    g_labelEdit = 1;
    return 1;
}
int cat_edit_open(int rename) {
    return catname_edit_open(rename ? EDIT_CAT_RENAME : EDIT_CAT_NEW);
}

// ---- send the save under the cursor to another category ---------------------------------------
// The list keeps the rows it read when it opened, so a save moved away is still offered by the list
// on screen; loading that ghost row only raises the game's own "load failed" box (measured).
volatile int g_sendDest = 0;
static int g_sendPendMove = 0;

static const char* send_dest_pick(void) {
    int i = g_sendDest;
    return (i >= 0 && i < g_catListN) ? g_catList[i] : NULL;
}
const char* send_dest_name(void) {
    int i = g_sendDest;
    return (i >= 0 && i < g_catListN) ? g_catListU8[i] : NULL;
}
int send_cycle(int dir) {
    int n = g_catListN + 1;                    // ...the last stop being a category not made yet
    g_sendDest = (g_sendDest + (dir > 0 ? 1 : n - 1)) % n;
    return g_sendDest;
}


// ---- the save list's little menu: what can be done with the save under the cursor -------------
// The slot is latched at open: the DirectInput gate keeps the game's cursor still while this is
// up, so the row cannot change underneath it.
static int send_apply_from(int src, const char* dstName, int move, int quiet);

volatile int g_lselOpen = 0;
volatile int g_lselSel  = 0;
volatile int g_lselSlot = -1;
// The title screen's load list and nowhere else: the ghost row a move leaves behind is only known
// harmless there, and the game's own save screen can write into this category while this is up.
int lsel_open(void) {
    if (g_lselOpen) return 1;
    if (!label_edit_ready() || g_shownState != MAIN_STATE_LOAD) return 0;
    cat_list_refresh();
    g_lselSlot = g_shownSlot;
    g_lselSel = LSEL_NOTE;
    g_sendDest = 0;
    g_lselOpen = 1;
    ff13log("[FF13-SRP] save list: menu opened on save %d\n", g_lselSlot + 1);
    return 1;
}
void lsel_close(void) { g_lselOpen = 0; }

// Title screen only: every row here re-stamps, renames or moves save files, and a running game
// holds the ones it loaded from. STATE_LOAD is the title's own load list (MAINLOOP_HOOK.md); a
// running game's save screen never leaves its zone state.
void ss_menu_open(void) {
    int st = main_state();
    if (st >= 0 && st != MAIN_STATE_TITLE && st != MAIN_STATE_LOAD) {
        ff13log("[FF13-SRP] save menu: refused -- main state %d, not the title screen\n", st);
        flash_say_c(ui_text(T_SS_NOTITLE), OVL_FLASH_OFF);
        return;
    }
    filter_menu_refresh();
    cat_list_refresh();
    g_ssOpen = 1;
    ff13log("[FF13-SRP] save menu opened\n");
}

// Called every worker tick, ahead of the editor taking the loop over, so an editor is ended by the
// cancel this sets on that same tick. Known gap: the title's attract movie plays inside
// STATE_TITLE and moves nothing a probe could see (measured), so it is not caught here.
void menu_screen_tick(void) {
    static int at = -2;
    int st = main_state();
    if (st < 0 || st == at) return;
    int had = (g_ssOpen || g_lselOpen || g_labelEdit);
    if (at != -2)
        ff13logv("[FF13-SRP] main state %d -> %d%s\n", at, st,
                 had ? " -- the menu on screen closes with it" : "");
    at = st;
    if (!had) return;
    if (g_labelEdit) label_edit_finish(0);
    g_ssOpen = 0;
    lsel_close();
    di_gate_lift();
}
// 1 = the row did something. An editor it opens draws above this menu, which stays open behind it.
int lsel_go(void) {
    if (!g_lselOpen) return 0;
    if (g_lselSel == LSEL_NOTE) {
        if (label_edit_open_slot(g_lselSlot) >= 0) return 1;
        flash_say_c(ui_text(T_LABEL_NO_WINDOW), OVL_FLASH_OFF);
        return 0;
    }
    int move = (g_lselSel == LSEL_MOVE);
    const char* pick = send_dest_pick();
    if (pick) {
        int ok = send_apply_from(g_lselSlot, pick, move, 0);
        if (ok) g_lselOpen = 0;
        return ok;
    }
    g_sendPendMove = move;              // the name is typed, and the editor's commit does the send
    if (catname_edit_open(EDIT_CAT_SEND)) return 1;
    flash_say_c(ui_text(T_LABEL_NO_WINDOW), OVL_FLASH_OFF);
    return 0;
}

static int send_free_slot(const char* dir) {
    for (int s = 0; s < SS_SLOTS; s++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\ff13-%02d.dat", dir, s);
        if (!file_exists(p)) return s;
    }
    return -1;
}

// All three sidecars are one line per save with the number first, so the same edit fits each.
// `row` is already in that file's own format, or empty when there is nothing to carry over.
#define SEND_ROW_BYTES (MAP_MAX * 26 + 64)   // the maps line, the long one, at MAP_MAX pairs
static int send_sidecar_graft(const char* dir, const char* name, int dstSlot, const char* row) {
    char path[MAX_PATH], tmp[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE* in = fopen(path, "r");
    // A file that exists but will not open must abort: rewriting from nothing would shrink the
    // category's sidecar to this one row.
    if (!in && file_exists(path)) {
        ff13loga("[FF13-SRP] send: cannot read %s -- left as it was\n", path);
        return 0;
    }
    FILE* o = fopen(tmp, "w");
    if (!o) {
        if (in) fclose(in);
        ff13loga("[FF13-SRP] send: cannot write %s\n", tmp);
        return 0;
    }
    if (in) {
        char line[SEND_ROW_BYTES];
        while (fgets(line, sizeof line, in)) {
            int s;
            if (sscanf(line, "%d", &s) == 1 && s == dstSlot) continue;
            fputs(line, o);
        }
        fclose(in);
    }
    if (row && row[0]) fputs(row, o);
    int wok = !ferror(o);
    if (fclose(o) != 0) wok = 0;
    // The original is never deleted first: a failed rewrite must leave the category's sidecar
    // standing, not gone.
    if (!wok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        ff13loga("[FF13-SRP] send: cannot put %s in place (err %lu) -- left as it was\n",
                 path, GetLastError());
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

static int send_apply_from(int src, const char* dstName, int move, int quiet) {
    char dir[MAX_PATH], sp[MAX_PATH], dp[MAX_PATH], u8[CATNAME_BYTES * 3], msg[256];
    if (src < 0 || src >= SS_SLOTS || !dstName || !dstName[0]
        || !cat_dir_of(dir, sizeof dir, dstName) || !save_path_for(src, sp, sizeof sp)
        || !file_exists(sp)) {
        ff13loga("[FF13-SRP] send: save %d cannot be sent to '%s'\n",
                 src + 1, dstName ? dstName : "");
        if (!quiet) flash_say_c(ui_text(T_SEND_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    catname_to_u8(u8, sizeof u8, dstName);
    int m = send_free_slot(dir);
    if (m < 0) {
        if (!quiet) {
            snprintf(msg, sizeof msg, ui_text(T_SEND_FULL), u8);
            flash_say_c(msg, OVL_FLASH_OFF);
        }
        return 0;
    }
    snprintf(dp, sizeof dp, "%s\\ff13-%02d.dat", dir, m);
    // Copy then delete, never a move: a send cut short in the middle leaves the save where it was
    // rather than nowhere.
    if (!CopyFileA(sp, dp, TRUE)) {
        ff13loga("[FF13-SRP] send: %s -> %s failed (err %lu)\n", sp, dp, GetLastError());
        if (!quiet) flash_say_c(ui_text(T_SEND_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    {
        char sb[MAX_PATH], db[MAX_PATH];
        if (bak_path_for(src, sb, sizeof sb) && file_exists(sb)) {
            snprintf(db, sizeof db, "%s.bak", dp);
            if (!CopyFileA(sb, db, TRUE))
                ff13loga("[FF13-SRP] send: .bak %s -> %s failed (err %lu)\n",
                         sb, db, GetLastError());
        }
    }
    // CopyFileA carried the mtime over; header_renumber_at keeps it through the edit.
    header_renumber_at(dp, m);
    int gok = 1;
    {
        char row[SEND_ROW_BYTES];
        row[0] = 0;
        if (g_sidecar[src].valid)
            snprintf(row, sizeof row, "%d %u %.3f %.3f %.3f\n", m, g_sidecar[src].zone,
                     g_sidecar[src].x, g_sidecar[src].y, g_sidecar[src].z);
        gok &= send_sidecar_graft(dir, kCatSidecars[0], m, row);
        row[0] = 0;
        if (g_slotMapN[src] > 0) {
            int n = snprintf(row, sizeof row, "%d %d", m, g_slotMapN[src]);
            for (int i = 0; i < g_slotMapN[src] && n > 0 && n < (int)sizeof row - 2; i++)
                n += snprintf(row + n, sizeof row - n, " %d %d",
                              g_slotMap[src][i][0], g_slotMap[src][i][1]);
            if (n > 0 && n < (int)sizeof row - 2) { row[n] = '\n'; row[n + 1] = 0; }
            else row[0] = 0;
        }
        gok &= send_sidecar_graft(dir, kCatSidecars[1], m, row);
        row[0] = 0;
        if (g_dateRow[src].valid)
            snprintf(row, sizeof row, "%d %u %u %u %u\n", m,
                     (unsigned)g_dateRow[src].stamp, (unsigned)(g_dateRow[src].stamp >> 32),
                     (unsigned)g_dateRow[src].real,  (unsigned)(g_dateRow[src].real  >> 32));
        gok &= send_sidecar_graft(dir, kCatSidecars[2], m, row);
    }
    // A refused graft downgrades a move to a copy: the source rows are the only surviving pair.
    if (move && !gok) {
        ff13loga("[FF13-SRP] send: a sidecar would not take save %d's row -- copied, not moved\n",
                 src + 1);
        if (!quiet) flash_say_c(ui_text(T_SEND_FAIL), OVL_FLASH_OFF);
        return 0;
    }
    if (move) {
        char sb[MAX_PATH];
        DeleteFileA(sp);
        if (bak_path_for(src, sb, sizeof sb)) DeleteFileA(sb);
        // The same tables renum_slot_swap carries: one added there is cleared here too.
        memset(&g_sidecar[src], 0, sizeof g_sidecar[src]);
        g_slotMapN[src] = 0;
        memset(g_slotMap[src], 0, sizeof g_slotMap[src]);
        memset(g_slotA18[src], 0, sizeof g_slotA18[src]);
        g_slotHasA18[src] = 0;
        g_slotLoc[src] = 0; g_slotHasLoc[src] = 0;
        g_bakKnown[src] = 0;
        memset(&g_dateRow[src], 0, sizeof g_dateRow[src]);
        sidecar_save_file(); mapsidecar_save(); datesidecar_save();
        filter_menu_refresh();
        // The last save of the filtered chapter may just have left (same guard as a switch).
        if (g_filterChapter && filter_count_of(g_filterChapter) <= 0) {
            ff13log("[FF13-SRP] send: chapter %d has no save left -- the whole list is shown\n",
                    g_filterChapter);
            filter_apply(0);
            state_save();
        }
    }
    ff13log("[FF13-SRP] send: save %d -> '%s' save %d (%s)\n",
            src + 1, dstName, m + 1, move ? "move" : "copy");
    if (!quiet) {
        snprintf(msg, sizeof msg, ui_text(move ? T_SEND_MOVED : T_SEND_COPIED), src + 1, u8, m + 1);
        flash_say(msg);
    }
    return 1;
}
// The most recently written save; where a sort has faked the mtime, the dates table's real one.
static int newest_active_slot(void) {
    int best = -1; ULONGLONG bestT = 0;
    for (int slot = 0; slot < SS_SLOTS; slot++) {
        char p[MAX_PATH];
        if (!save_path_for(slot, p, sizeof p)) return -1;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) continue;
        // RAW mtime, as ss_collect compares it: .stamp is raw UTC, and ft_local here kept the
        // equality from ever holding -- the newest STAMP then won instead of the newest date
        // (measured).
        ULONGLONG t = ft_u64(fa.ftLastWriteTime);
        if (slot < 100 && g_dateRow[slot].valid && g_dateRow[slot].stamp == t)
            t = g_dateRow[slot].real;
        if (t > bestT) { bestT = t; best = slot; }
    }
    return best;
}

static void seed_remove(const char* dir) {
    char p[MAX_PATH];
    for (int slot = 0; slot < SS_SLOTS; slot++) {
        snprintf(p, sizeof p, "%s\\ff13-%02d.dat", dir, slot); DeleteFileA(p);
        snprintf(p, sizeof p, "%s\\ff13-%02d.dat.bak", dir, slot); DeleteFileA(p);
    }
    for (int i = 0; i < CAT_SIDECARS; i++) {
        snprintf(p, sizeof p, "%s\\%s", dir, kCatSidecars[i]); DeleteFileA(p);
    }
}

// Off the message thread, like label_edit_tick: the file work for a finished category name.
void cat_edit_tick(void) {
    if (!g_labelEdit || g_editMode == EDIT_LABEL || !g_editDone) return;
    int commit = g_editDone, rename = (g_editMode == EDIT_CAT_RENAME),
        send = (g_editMode == EDIT_CAT_SEND);
    g_labelEdit = 0; g_editDone = 0; g_editMode = EDIT_LABEL;
    if (commit < 0) return;
    char name[CATNAME_BYTES];
    int bad = catname_commit_check(name, sizeof name, rename);
    if (bad) {
        flash_say_c(ui_text(bad == 2 ? T_SS_CAT_EXISTS : T_SS_CAT_BADNAME), OVL_FLASH_OFF);
        return;
    }
    if (send) {
        char sendCats[MAX_PATH], sendDir[MAX_PATH];
        if (!cats_root(sendCats, sizeof sendCats) || !cat_dir_of(sendDir, sizeof sendDir, name)) {
            flash_say_c(ui_text(T_SEND_FAIL), OVL_FLASH_OFF);
            return;
        }
        CreateDirectoryA(sendCats, NULL);   // the parent: this may be the very first category
        if (!CreateDirectoryA(sendDir, NULL)) {
            ff13loga("[FF13-SRP] send: cannot make %s (err %lu)\n", sendDir, GetLastError());
            flash_say_c(ui_text(T_SEND_FAIL), OVL_FLASH_OFF);
            return;
        }
        if (send_apply_from(g_lselSlot, name, g_sendPendMove, 0)) { g_lselOpen = 0; cat_list_refresh(); }
        else RemoveDirectoryA(sendDir);
        return;
    }
    if (rename) {
        // The category in use has no folder of its own, so its name lives entirely in that file.
        ff13log("[FF13-SRP] category: '%s' renamed to '%s'\n", g_catCur, name);
        catname_set_cur(name);
        catname_save();
        cat_list_refresh();
        char msg[192]; snprintf(msg, sizeof msg, ui_text(T_SS_CAT_RENAMED), g_catCurU8);
        flash_say(msg);
        return;
    }
    char cats[MAX_PATH], dir[MAX_PATH];
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(dir, sizeof dir, name)) {
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return;
    }
    CreateDirectoryA(cats, NULL);       // the parent: this may be the very first category
    if (!CreateDirectoryA(dir, NULL)) {
        ff13loga("[FF13-SRP] category: cannot make %s (err %lu)\n", dir, GetLastError());
        flash_say_c(ui_text(T_SS_CAT_FAIL), OVL_FLASH_OFF);
        return;
    }
    // Seeded BEFORE the switch, so the switch carries it in like any parked file: at zero saves the
    // title drops its LOAD GAME entry and only re-grows it when the title is re-entered (measured).
    int seed = newest_active_slot();
    if (seed >= 0) send_apply_from(seed, name, 0, 1);
    if (cat_switch(name)) g_ssOpen = 0;
    else { seed_remove(dir); RemoveDirectoryA(dir); }
}

// ---- the three entry points the merged worker calls ----

// What a previous session left behind: the per-slot positions, the per-slot terrain-map lists, and
// which .bak files are ours.
void save_sidecars_load(void) {
    catname_load();
    cat_switch_recover();
    sidecar_load_file();
    mapsidecar_load();
    datesidecar_load();
    renum_recover();       // before the game can draw a list that a cut-short renumber emptied
    sidecar_align_check();
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
    wait_for_bytes(g_base + RVA_SAVEROW_SHOWN, P_83AF40, (int)sizeof P_83AF40, 15000);
    void* rw = ff13_install_ecx_hook(g_base + RVA_SAVEROW_SHOWN, P_83AF40, (int)sizeof P_83AF40,
                                     (void*)on_row_shown);
    (rw ? ff13logv : ff13loga)("[FF13-SRP] labels: row watch %s\n", rw ? "OK" : "FAIL");
    if (g_dateRows) date_hook_install();
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
    static int titleCountdown = 0, titleSlot = 0;
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
        // Any save at all, ours or a save point's: the game has just written its own title over
        // whatever was there, so this slot's mark goes back on once the file has settled (a note
        // does not: it described the save that was overwritten).
        titleSlot = (int)slot; titleCountdown = SAVE_FIX_DELAY_TICKS + (1000 / HK_POLL_MS);
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
        } else {
            if (g_sidecar[slot].valid) {
                g_sidecar[slot].valid = 0;
                ff13log("[FF13-SRP] normal save slot=%u -> cleared custom pos\n", slot);
                sidecar_save_file();
            }
            // A snapshot kept here would arm STAGE17 on every plain load of this slot.
            if (g_slotMapN[slot] || g_slotHasA18[slot] || g_slotHasLoc[slot]) {
                g_slotMapN[slot] = 0; g_slotHasA18[slot] = 0; g_slotHasLoc[slot] = 0;
                mapsidecar_save();
                ff13log("[FF13-SRP] normal save slot=%u -> cleared terrain snapshot\n", slot);
            }
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
    if (titleCountdown > 0 && --titleCountdown == 0) title_refresh(titleSlot);
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
