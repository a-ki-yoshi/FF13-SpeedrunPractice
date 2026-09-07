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
#ifdef FF13_DIAG
static void trk_audit(const char* when);
#endif
static void btres_clear_for_pick(uint32_t btsc);

// ============================ BATTLE PICKER (call up any fight in this zone) ==================
// Threading: everything that touches game state runs from bs_pump() on the GAME thread (called from
// the GetWorldPos hook); the hotkeys only set a request flag.
// Starting a fight: do NOT replay doEncountCheck's own calls (0xB500A0(id), then the trigger
// 0x80D3C0) -- that produced a silent battle with no encounter presentation. Instead set the field
// manager's PENDING touched-object id and let the game run doEncountCheck (0x5CEE00) itself on the
// next frame, so the path is bit-for-bit the normal one:
//     [fieldMgr + 0x14648] = touched object id    (the game clears it back to -1 in 0x5B3920)
// Read out of 0x5B3840 / 0x5CB5E0, so no real encounter is needed first:
//     field manager  = [0x27FF124]
//     object array   = mgr + 0x10, 564 x 0x90 (0x13D40 bytes)
//     in-use bitmap  = mgr + 0x10 + 0x13D40    (bit per index; that index IS the touch id)
// The band a zone uses is the one thing only a real encounter reveals; it is learned once and
// persisted, and from then on the picker works from a cold start.
#define OFF_FOBJ_ARRAY   0x10
#define FOBJ_COUNT       0x234
#define OFF_FOBJ_BITMAP  (OFF_FOBJ_ARRAY + 0x13D40)
// Zone -> btsc band. A real encounter in a zone overrides its entry (zb_learn).
static const struct { uint32_t zone, band; } kZoneBand[] = {
    {  2,  3000 },   // hgcr  Lake Bresha           (verified)
    {  3,  9000 },   // hiku  Palamecia (fleet battles)
    {  4,  4000 },   // vpek  Vile Peaks
    {  6,  8000 },   // nati  Nautilus              (verified)
    {  8,  7000 },   // pmpm  Palumpolum            (verified)
    { 10,  5000 },   // snls  Sunleth Waterscape
    { 15,  6000 },   // gapr  Gapra Whitewood        (verified)
    { 16,  1000 },   // hang  Hanging Edge
    { 17,  2000 },   // hgin  the Fal'Cie / lower world
    { 18, 10000 },   // fark  Fifth Ark
    { 19, 12000 },   // eden  Eden
    { 20, 11000 },   // gpst  |
    { 21, 11000 },   // gpcg  | Gran Pulse locations all live in the 11xxx band, which is split
    { 22, 11000 },   // gpyu  | further (110xx Yaschas, 112xx Steppe, 114xx Cradle, 115xx Sulyya,
    { 23, 11000 },   // gpda  | 116xx Taejin, 117xx Oerba, 118xx Ochu, 119xx missions) -- scanning
    { 24, 11000 },   // gptk  | the whole thousand covers them all.
    { 26, 11000 },   // gptt  |
    { 27, 11000 },   // gpwo  |
    { 30, 11000 },   // gpsr  |
    { 29, 12000 },   // lasd  Orphan's Cradle
};
#define ZB_MAX 64
// Per zone: the scene band, plus a touch id that REALLY worked here. doEncountCheck wants the id of
// an ENEMY SYMBOL -- any other live object out of the bitmap makes it do nothing -- and the two
// cannot be told apart statically, so the id seen in a real encounter is stored and reused.
// `sel` is the row this zone was last left on.
static struct { uint32_t zone, band, touch, sel, selMode; } g_zb[ZB_MAX];
static volatile int g_zbN = 0;
// Scene data is resident for the whole game but enemy MODELS are per-zone, so a scene this zone does
// not use fields nothing and there is no way to ask in advance. Recorded as "N of M turned up",
// counted from the game's own per-enemy build / activate-failure messages.
#define EMPTY_MAX 192
// Bumped whenever a change makes previously-learned answers wrong; older records are dropped once.
#define ZB_EMPTY_GEN 3
static struct { uint16_t btsc, got, want; } g_fieldRec[EMPTY_MAX];
static volatile int g_emptyN = 0;
// Character-set preload (see charaset_needed): the touch is held back while the models load, and the
// set that was there before is put back once the fight is over. Always on.
// NOT a wait -- the fight starts as soon as scene_resident answers; this is only the backstop for
// when it cannot (Resource.isLoadedChara reports that a charaspec is DEFINED, not that its model is
// resident). Two seconds over the heaviest load ever measured.
// Do NOT shorten this by polling a resource-queue predicate every frame: one of the three managers
// it locks is the sound manager, and it silenced every sound in the game. The way in is a passive
// hook on load completion, on the game's own thread.
#define BS_PRELOAD_CAP_MS 8000
// A cap that expires while the probe still says "not resident" starts a fight whose models are
// missing; the extension is bounded so BS_GIVEUP_MS stays the outer wall. Measured: CODE_NOTES.
#define BS_PRELOAD_STEP_MS      1000
// Long enough that a load which is only just starting reads as "not read" on the next poll.
#define BS_PROBE_SETTLE_MS 400
#define BS_PRELOAD_EXTRA_MAX_MS 8000
static const int         g_bsPreloadMs = BS_PRELOAD_CAP_MS;
// Only an arming that ASKED to wait may be extended. Two of them hand the touch over at once (the
// arena door, the second pass), and on those the probe says "no" as a matter of course -- judging
// them by it would sit on a fight that has nothing left to load. Both are set at every arm site.
static volatile int      g_bsTouchCap = 0;     // ms this arming asked for; 0 = hand over at once
static volatile int      g_bsTouchExtra = 0;
// The touch id and the "one is pending" fact must stay separate: object id 0 is a perfectly valid
// live object, so 0 cannot double as "none".
volatile int             g_bsTouchDue = 0;
static volatile uint32_t g_bsTouchPending = 0;
static volatile DWORD    g_bsTouchAt = 0;
static volatile int      g_bsTouchReady = 0;
static volatile int      g_bsProbeMoved = 0;   // the probe has been seen to say no
static volatile int      g_bsTouchFull = 0;    // this wait cannot be cut short by the probe
static volatile int      g_bsProbeLogged = 0;
static char g_csRestore[40];                   // set to put back when the fight ends
// ...and the deferred half of putting it back: sending the chrset command the moment the fight is
// over can freeze the AsyncLoader thread for the rest of the session (see cs_restore_watch).
static volatile uint32_t g_decksRestoreFor = 0;  // fight whose party the picker set, decks pending
static volatile int   g_selBefore = -1;          // paradigm in hand when the pick was made
static volatile int   g_selLast = -1;            // ...the last one seen in hand inside the fight
static volatile int   g_selWant = -1;            // ...and the one the field is owed, chosen at fight-over
static volatile int   g_selRestoreDue = 0;       // ...and whether the field still owes it back
static volatile DWORD g_selRestoreFrom = 0;
// One pass over a fight's drop rows, run at pick time and again at the touch hand-off. The
// second pass is insurance, not a cure: the 2026-08-27 residual objects were the STAGE17 append,
// not a failed deactivate (CODE_NOTES, the 12091 addenda).
static int field_drops_apply(uint32_t btsc) {
    int dn = 0;
    const ff13_fielddrop* fd = field_drops(btsc, &dn);
    for (int i = 0; i < dn; i++) {
        if (fd[i].kind == FDROP_HIDE)         vm_layout_show(fd[i].name, 0);
        else if (fd[i].kind == FDROP_AREA_EX) vm_deactivate_area_ex(fd[i].name);
        else                                  vm_deactivate_area(fd[i].name);
        ff13logv("[FF13-SRP] battle picker: field object \"%s\" %s\n", fd[i].name,
                 fd[i].kind == FDROP_HIDE    ? "hidden"
               : fd[i].kind == FDROP_AREA_EX ? "deactivate requested (async)"
                                             : "deactivated");
    }
    return dn;
}
static volatile uint32_t g_fieldDropFor = 0;     // fight whose field objects were taken down
static volatile uint32_t g_fieldDropZone = 0;    // ...and the zone they belong to
static volatile int   g_fieldDropDue = 0;        // the field is owed them back
static volatile DWORD g_fieldDropFrom = 0;
static char           g_csRestorePend[40];
static volatile uint32_t g_csRestoreZone = 0;    // ...and the zone it was the set OF
static volatile DWORD g_csRestoreFrom = 0;
static volatile DWORD g_csRestoreStable = 0;
static volatile DWORD g_csRestoreEarliest = 0;   // a short fight holds the set for its retry
static volatile int   g_csRestoreHold = 0;       // ...timed from the field coming back, not the win
static volatile int   g_csRestoreLate = 0;       // the conclusion outlasted the cap; said once
static volatile DWORD g_csFieldBackAt = 0;       // ...and when the field came back, to time it from
volatile uint32_t g_bsStarted = 0;      // scene the picker last started
static volatile int   g_openLogN = 0;   // opening transforms logged for this fight
static volatile int   g_swapLogN = 0;   // ...and charaspec swaps
static volatile int   g_specFixN = 0;   // ...and shapes put back to what the scene names
static volatile DWORD g_openLogAt = 0;  // ...and when, so the next fight starts over
#define BLACKOUT_MAX_MS 25000
static volatile DWORD g_blackUntil = 0;
// How long to keep covering AFTER the fight has started, so the fight's own first frames do not show
// through. [battle_swap] arena_blackout_extra overrides it.
static volatile int   g_blackExtra = 150;
static volatile DWORD g_blackBegan = 0;

static void blackout_begin(void) {
    g_blackUntil = GetTickCount() + BLACKOUT_MAX_MS;
    g_blackBegan = GetTickCount();
}
static void blackout_end(void) {
    if (g_blackUntil && g_blackBegan)
        ff13logv("[FF13-SRP] blackout: lifted %ums after it went up\n",
                 (unsigned)(GetTickCount() - g_blackBegan));
    g_blackUntil = 0;
}
void blackout_hold_on(void) {
    if (!g_blackUntil) return;
    if (g_blackExtra <= 0) { blackout_end(); return; }
    DWORD want = GetTickCount() + (DWORD)g_blackExtra;
    if ((int)(want - g_blackUntil) < 0) g_blackUntil = want;    // never extend past the deadline
}
int  blackout_on(void) { return g_blackUntil && (int)(GetTickCount() - g_blackUntil) < 0; }
static volatile DWORD    g_bsStartedAt = 0;
static volatile unsigned char g_bsInFight = 0;
static volatile unsigned char g_bsSawEnemy = 0;   // an enemy was targetable during that fight
static volatile unsigned char g_bsArena = 0;      // and it was staged on an arena (see restart_watch)

// Scene numbers seen in SCRIPTED (story boss) fights. The field path never resolves these; the only
// way to learn one is to watch startBattle_l -- fight the boss once and its number is yours.
#define BOSS_MAX 64
static uint32_t g_bossList[BOSS_MAX];
// ...and the arena name that came with it, stripped to "<area>_<nnn>". That is the join to the
// fight's music: arena "bt_<area>_<nnn>_cb" <-> event "ev_<area>_<nnn>" <-> kBgmEvent.
static char g_bossArena[BOSS_MAX][16];
static volatile int g_bossN = 0;
// The story's auto-music-off bit (evQuiet). A checkpoint restart re-runs the field scripts, which
// clear it themselves; the lift below is for every way OUT that skips that.
static volatile int g_evQuietOn = 0;
static volatile int g_evFlagWas = 0;       // an evNoFight pick's pre-gate scenario flag...
static volatile int g_evFlagRestore = 0;   // ...and whether fight-over owes it back
static void evquiet_lift(void) {
    if (!g_evQuietOn) return;
    g_evQuietOn = 0;
    int w = vm_get_work(1);
    if (w & 1) vm_set_work(1, w & ~1);
    ff13logv("[FF13-SRP] battle picker: auto music back on\n");
}

int is_boss_scene(uint32_t btsc) {
    for (int i = 0; i < g_bossN; i++) if (g_bossList[i] == btsc) return 1;
    return 0;
}
// "bt_gapr_200_cb" -> "gapr_200". Anything of another shape is dropped rather than half-parsed, so a
// lookup miss means "not learned", never "learned wrong".
static void arena_key(char* out, size_t n, const char* arena) {
    out[0] = 0;
    if (!arena || strncmp(arena, "bt_", 3) != 0) return;
    const char* s = arena + 3;
    size_t len = strlen(s);
    if (len > 3 && strcmp(s + len - 3, "_cb") == 0) len -= 3;
    if (len == 0 || len >= n) return;
    memcpy(out, s, len);
    out[len] = 0;
}
void boss_learn(uint32_t btsc, const char* arena) {
    char key[16];
    arena_key(key, sizeof key, arena);
    for (int i = 0; i < g_bossN; i++) {
        if (g_bossList[i] != btsc) continue;
        if (key[0] && !g_bossArena[i][0]) {          // fill in a name a previous session lacked
            strncpy(g_bossArena[i], key, sizeof(g_bossArena[i]) - 1);
            zb_save();
        }
        return;
    }
    if (!btsc || g_bossN >= BOSS_MAX) return;
    g_bossList[g_bossN] = btsc;
    strncpy(g_bossArena[g_bossN], key, sizeof(g_bossArena[0]) - 1);
    g_bossArena[g_bossN][sizeof(g_bossArena[0]) - 1] = 0;
    g_bossN++;
    ff13logv("[FF13-SRP] battle picker: btsc%05u is a story-fight scene, arena \"%.15s\" (remembered)\n",
             btsc, key[0] ? key : "?");
    zb_save();
}
const char* bgm_event_track(uint32_t btsc) {
    for (int i = 0; i < g_bossN; i++) {
        if (g_bossList[i] != btsc || !g_bossArena[i][0]) continue;
        for (int j = 0; j < FF13_BGM_EVENT_N; j++)
            if (strcmp(kBgmEvent[j].ev, g_bossArena[i]) == 0) return kBgmEvent[j].track;
        return NULL;
    }
    return NULL;
}
const char* bgm_zone_track(uint32_t zone) {
    for (int i = 0; i < FF13_BGM_ZONE_N; i++)
        if (kBgmZone[i].zone == zone) return kBgmZone[i].track;
    return NULL;
}

// What a scene fields. kind low bits: 0 ordinary, 1 all-unique enemies, 2 script-placed -- the last
// names no enemy at all, the fight's script puts one in the placement point (story bosses, battles
// out of cutscenes), and picking one really does spawn its enemy.
// Bit 4 records that the enemy slots do not start at enemy0. Kept because it is a real property of
// the data, but NOT shown: it does not predict what a scene fields.
const ff13_btsc_info* btsc_info(uint32_t btsc) {
    int lo = 0, hi = FF13_BTSC_TABLE_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t v = kBtscTable[mid].btsc;
        if (v == btsc) return &kBtscTable[mid];
        if (v < btsc) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

// Everything btsc_picker.tsv says about one fight. Any output may be NULL if you do not want it;
// each comes back empty (name NULL, sort UNSET, tag "") when the TSV is silent about this fight.
// `mode` is which row of a fight that has more than one: 0 for the fight itself, 1..n for a fight
// the picker offers once per form (see kAiForms). The TSV carries a row per form and the header is
// sorted on the pair, so one search key covers both.
void btsc_hand(uint32_t btsc, int mode, const unsigned short** jp, const char** en,
                      unsigned* order, const unsigned short** tag, int* kind) {
    if (jp) *jp = NULL;
    if (en) *en = NULL;
    if (order) *order = FF13_BTSC_ORDER_LAST;
    if (tag) *tag = NULL;
    if (kind) *kind = 0;
#ifdef FF13_HAVE_BTSC_NAMES
    unsigned key = (btsc << 8) | (unsigned)(mode & 0xFF);
    int lo = 0, hi = FF13_BTSC_NAMES_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned v = ((unsigned)kBtscNames[mid].btsc << 8) | kBtscNames[mid].mode;
        if (v < key) { lo = mid + 1; continue; }
        if (v > key) { hi = mid - 1; continue; }
        const ff13_btsc_name* r = &kBtscNames[mid];
        if (jp && r->off != FF13_BTSC_NO_TEXT)     *jp = &kBtscNamePool[r->off];
        if (en && r->offEn != FF13_BTSC_NO_TEXT)   *en = &kBtscNamePoolEn[r->offEn];
        if (order) *order = r->order;
        if (tag && r->offTag != FF13_BTSC_NO_TEXT) *tag = &kBtscNamePool[r->offTag];
        if (kind) *kind = r->kind;
        return;
    }
#else
    (void)btsc; (void)mode; (void)kind;
#endif
}

// The most forms one fight can offer.
#define FORMS_MAX 6
static int aimode_count(uint32_t btsc);   // how many forms this fight offers; defined with the pin
static volatile DWORD g_loadDownAt;       // when the loading indicator last went down (see on_load_hide)

static const void* field_rec(uint32_t btsc, int* got, int* want) {
    for (int i = 0; i < g_emptyN; i++) {
        if (g_fieldRec[i].btsc != (uint16_t)btsc) continue;
        if (got) *got = g_fieldRec[i].got;
        if (want) *want = g_fieldRec[i].want;
        return &g_fieldRec[i];
    }
    return NULL;
}
static int is_empty_scene(uint32_t btsc) {
    int got = 1, want = 1;
    return field_rec(btsc, &got, &want) && got == 0;
}

static uint32_t field_mgr(void) {
    if (g_fieldMgr) return g_fieldMgr;                 // captured from a real encounter
    if (!g_base) return 0;
    uintptr_t p = g_base + RVA_FIELDMGR_PTR;
    if (IsBadReadPtr((void*)p, 4)) return 0;
    uint32_t m = *(uint32_t*)p;
    if (m && IsBadReadPtr((void*)(uintptr_t)(m + OFF_TOUCHED_ID), 4)) return 0;
    return m;
}

// A touch id the game will accept: prefer one we actually saw, else the first live slot.
// Returns 1 and writes *out on success. Object id 0 is a REAL id -- doEncountCheck only treats a
// NEGATIVE pending id as "nothing touched" -- so it cannot be signalled by returning 0.
static uint32_t zb_touch(uint32_t zone);
static int pick_touch_id(uint32_t mgr, uint32_t* out) {
    if (g_lastTouchedId) { *out = g_lastTouchedId; return 1; }   // seen this session: certain
    uint32_t saved = zb_touch(field_zone_id());        // seen in this zone before: reliable
    if (saved) { *out = saved; return 1; }
    if (!mgr) return 0;
    // Last resort: any live slot. doEncountCheck may ignore it (it wants an enemy symbol, and
    // nothing here tells props apart), in which case the key appears to do nothing -- so say so.
    // Only walking into an ordinary field enemy teaches the real id; a story battle does not.
    ff13loga("[FF13-SRP] battle picker: no touch id learned for this zone; falling back to the first "
             "live field object, which may well not be an enemy symbol. Walk into one ordinary "
             "field enemy here once (a story battle does not count).\n");
    uintptr_t bm = (uintptr_t)mgr + OFF_FOBJ_BITMAP;
    int bad = IsBadReadPtr((void*)bm, FOBJ_COUNT / 8) ? 1 : 0;
    uint32_t live = 0, first = 0;
    if (!bad) {
        for (uint32_t id = 0; id < FOBJ_COUNT; id++) {
            if (!((*(uint32_t*)(bm + (id >> 5) * 4) >> (id & 31)) & 1)) continue;
            if (!live) first = id;
            live++;
        }
    }
    ff13logv("[FF13-SRP] battle picker: object bitmap mgr=%08X bm=%08X readable=%d live=%u first=%u"
             " words=%08X %08X %08X %08X\n",
             mgr, (unsigned)bm, !bad, live, first,
             bad ? 0 : *(uint32_t*)(bm + 0), bad ? 0 : *(uint32_t*)(bm + 4),
             bad ? 0 : *(uint32_t*)(bm + 8), bad ? 0 : *(uint32_t*)(bm + 12));
    if (!live) return 0;
    *out = first;
    return 1;
}

static uint32_t zb_touch(uint32_t zone) {
    for (int i = 0; i < g_zbN; i++) if (g_zb[i].zone == zone) return g_zb[i].touch;
    return 0;
}
static uint32_t zb_lookup(uint32_t zone) {
    for (int i = 0; i < g_zbN; i++) if (g_zb[i].zone == zone) return g_zb[i].band;
    for (size_t i = 0; i < sizeof(kZoneBand)/sizeof(kZoneBand[0]); i++)
        if (kZoneBand[i].zone == zone) return kZoneBand[i].band / 1000;
    return 0xFFFFFFFFu;
}
void zb_save(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice_zones.dat")) return;
    FILE* f = fopen(path, "w"); if (!f) return;
    fprintf(f, "empties %d\n", ZB_EMPTY_GEN);
    for (int i = 0; i < g_zbN; i++)
        fprintf(f, "zone %u %u %u %u %u\n", g_zb[i].zone, g_zb[i].band, g_zb[i].touch,
                g_zb[i].sel, g_zb[i].selMode);
    for (int i = 0; i < g_emptyN; i++)
        fprintf(f, "field %u %u %u\n", g_fieldRec[i].btsc, g_fieldRec[i].got, g_fieldRec[i].want);
    for (int i = 0; i < g_bossN; i++)
        fprintf(f, "story %u %s\n", g_bossList[i], g_bossArena[i][0] ? g_bossArena[i] : "-");
    fclose(f);
}
// File what the game reported for this fight: `want` enemies built, `got` of them alive. Applies to
// picked fights and to ones walked into alike.
static void mark_field_result(uint32_t btsc, int got, int want) {
    if (!btsc || want <= 0) return;
    for (int i = 0; i < g_emptyN; i++) {
        if (g_fieldRec[i].btsc != (uint16_t)btsc) continue;
        // Keep the best showing: a boss that is already dead in this save fields nothing on a
        // re-run, and that says something about the save rather than about the zone.
        if (got < g_fieldRec[i].got)
            ff13log("[FF13-SRP] battle picker: btsc%05u fielded %d of %d this time (best here is "
                     "%d of %d, kept)\n", btsc, got, want, g_fieldRec[i].got, g_fieldRec[i].want);
        if (got > g_fieldRec[i].got || want > g_fieldRec[i].want) {
            ff13log("[FF13-SRP] battle picker: btsc%05u fielded %d of %d here (was %d of %d)\n",
                     btsc, got, want, g_fieldRec[i].got, g_fieldRec[i].want);
            g_fieldRec[i].got = (uint16_t)got; g_fieldRec[i].want = (uint16_t)want;
            zb_save();
        }
        return;
    }
    if (g_emptyN >= EMPTY_MAX) return;
    g_fieldRec[g_emptyN].btsc = (uint16_t)btsc;
    g_fieldRec[g_emptyN].got  = (uint16_t)got;
    g_fieldRec[g_emptyN].want = (uint16_t)want;
    g_emptyN++;
    ff13log("[FF13-SRP] battle picker: btsc%05u fielded %d of %d here%s%s\n", btsc, got, want,
             g_btlBad[0] ? " -- first missing: " : "", g_btlBad[0] ? g_btlBad : "");
    zb_save();
}
// Fallback for when the game said nothing: the "it was over before it began" guess. Never applied to
// a script-placed or story fight -- some of those draw their enemy from the field and go empty once
// it is dead in this save.
static void mark_empty_scene(uint32_t btsc) {
    if (!btsc || is_empty_scene(btsc)) return;
    const ff13_btsc_info* bi = btsc_info(btsc);
    if (is_boss_scene(btsc) || (bi && (bi->kind & 3) == 2)) {
        ff13logv("[FF13-SRP] battle picker: btsc%05u spawned nothing -- its enemies are already "
                 "defeated in this save (script-placed fight, not marked)\n", btsc);
        return;
    }
    mark_field_result(btsc, 0, bi && bi->enemies ? bi->enemies : 1);
}
void zb_load(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice_zones.dat")) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    char kw[16]; unsigned z, b;
    int gen = 0;                       // no marker at all = written before generations existed
    int dropped = 0;
    while (fscanf(f, "%15s", kw) == 1) {
        unsigned t = 0;
        if (!strcmp(kw, "empties") && fscanf(f, "%u", &z) == 1) {
            gen = (int)z;
        } else if (!strcmp(kw, "zone") && fscanf(f, "%u %u %u", &z, &b, &t) >= 2) {
            unsigned sel = 0, selMode = 0;
            if (fscanf(f, "%u", &sel) != 1) sel = 0;   // absent in files written before this field
            else if (fscanf(f, "%u", &selMode) != 1) selMode = 0;   // ...and likewise for the form
            // A band above 30 was never a real one; an older build could learn 4294967 from a
            // scene id of -1 and then list nothing in that zone forever. Drop it and let the zone
            // fall back to the static table.
            if (b > 30) {
                ff13logv("[FF13-SRP] battle picker: zone %u had an impossible band %u -> dropped\n",
                         z, b);
            } else if (g_zbN < ZB_MAX) {
                g_zb[g_zbN].zone = z; g_zb[g_zbN].band = b;
                g_zb[g_zbN].touch = t; g_zb[g_zbN].sel = sel;
                g_zb[g_zbN].selMode = selMode; g_zbN++;
            }
        } else if (!strcmp(kw, "empty") && fscanf(f, "%u", &z) == 1) {
            dropped++;                     // pre-generation-3 shape: a bare "it was empty" guess
        } else if (!strcmp(kw, "field") && fscanf(f, "%u", &z) == 1) {
            // Records written under an older ZB_EMPTY_GEN are dropped once and re-learned: a mod
            // change (the character-set preload) can make what a scene was seen to field wrong.
            unsigned got = 0, want = 0;
            if (fscanf(f, "%u %u", &got, &want) != 2) { got = 0; want = 1; }
            if (gen < ZB_EMPTY_GEN) dropped++;
            else if (g_emptyN < EMPTY_MAX) {
                g_fieldRec[g_emptyN].btsc = (uint16_t)z;
                g_fieldRec[g_emptyN].got  = (uint16_t)got;
                g_fieldRec[g_emptyN].want = (uint16_t)want;
                g_emptyN++;
            }
        } else if (!strcmp(kw, "bgm")) {
            // Written by the battle-track learner this build no longer carries: still parsed so an
            // older sidecar reads cleanly, then dropped.
            char tr[BGM_NAME_MAX + 1] = "";
            (void)(fscanf(f, "%u %16s", &z, tr) == 2);   // consumed and dropped
        } else if (!strcmp(kw, "story") && fscanf(f, "%u", &z) == 1) {
            // The arena name is optional: files written before it existed have none, and "-"
            // stands for "this fight was seen but its name was not readable".
            char arena[16] = "";
            int c = fgetc(f);
            while (c == ' ' || c == '\t') c = fgetc(f);
            if (c != EOF && c != '\n' && c != '\r') { ungetc(c, f); if (fscanf(f, "%15s", arena) != 1) arena[0] = 0; }
            else if (c != EOF) ungetc(c, f);
            if (arena[0] == '-' && !arena[1]) arena[0] = 0;
            if (g_bossN < BOSS_MAX) {
                g_bossList[g_bossN] = z;
                strncpy(g_bossArena[g_bossN], arena, sizeof(g_bossArena[0]) - 1);
                g_bossArena[g_bossN][sizeof(g_bossArena[0]) - 1] = 0;
                g_bossN++;
            }
        } else break;
    }
    fclose(f);
    ff13loga("[FF13-SRP] battle picker: %d zone->band pairs, %d empty, %d story scenes restored%s\n",
             g_zbN, g_emptyN, g_bossN, dropped ? " (older empty marks dropped -- retry those)" : "");
    if (dropped) zb_save();            // stamp the new generation so this happens once
}
void zb_learn(uint32_t zone, uint32_t band, uint32_t touch) {
    if (zone == (uint32_t)-1) return;
    for (int i = 0; i < g_zbN; i++) {
        if (g_zb[i].zone != zone) continue;
        if (g_zb[i].band == band && (!touch || g_zb[i].touch == touch)) return;
        g_zb[i].band = band;
        if (touch) g_zb[i].touch = touch;
        zb_save();
        return;
    }
    if (g_zbN >= ZB_MAX) return;
    g_zb[g_zbN].zone = zone; g_zb[g_zbN].band = band; g_zb[g_zbN].touch = touch; g_zbN++;
    ff13logv("[FF13-SRP] battle picker: zone %u uses btsc%02u000, touch id %u (remembered)\n",
             zone, band, touch);
    zb_save();
}

volatile int      g_bsOpen  = 0;      // list is on screen
volatile int      g_bsReq   = 0;      // 1 = open+scan, 2 = start the selected fight
// Raised by the hotkey thread when it closes the list: remembering the row reads a game pointer
// chain and rewrites the sidecar, so the work itself belongs on the game thread.
volatile int      g_bsRememberDue = 0;
volatile int      g_bsCount = 0;
volatile int      g_bsSel   = 0;
uint32_t          g_bsList[BS_MENU_MAX];
// Which mode of that fight the row stands for: 0 = the fight as the game rolls it, 1..n = one of the
// modes named in kAiForms below.
uint8_t           g_bsVar[BS_MENU_MAX];
UINT g_bsVk     = VK_F11;              // open/close the picker
UINT g_bsUpVk    = VK_UP;
UINT g_bsDownVk  = VK_DOWN;
UINT g_bsLeftVk  = VK_LEFT;
UINT g_bsRightVk = VK_RIGHT;

static uint32_t zb_sel(uint32_t zone, int* mode) {
    if (mode) *mode = 0;
    for (int i = 0; i < g_zbN; i++) {
        if (g_zb[i].zone != zone) continue;
        if (mode) *mode = (int)g_zb[i].selMode;
        return g_zb[i].sel;
    }
    return 0;
}
void zb_remember_sel(uint32_t zone, uint32_t btsc, int mode) {
    if (zone == (uint32_t)-1 || !btsc) return;
    for (int i = 0; i < g_zbN; i++) {
        if (g_zb[i].zone != zone) continue;
        if (g_zb[i].sel == btsc && g_zb[i].selMode == (uint32_t)mode) return;
        g_zb[i].sel = btsc;
        g_zb[i].selMode = (uint32_t)mode;
        zb_save();
        return;
    }
    // No entry to update: a zone whose fights all go through an arena door never has the ordinary
    // encounter that creates one, so make it here. The touch id stays 0 until a real encounter
    // fills it in (zb_learn upgrades an existing entry); pick_touch_id has its own fallback.
    if (g_zbN >= ZB_MAX) return;
    g_zb[g_zbN].zone = zone; g_zb[g_zbN].band = btsc / 1000;
    g_zb[g_zbN].touch = 0;   g_zb[g_zbN].sel  = btsc;
    g_zb[g_zbN].selMode = (uint32_t)mode;
    g_zbN++;
    zb_save();
}

// One row's place in the list. `order` is where the row sits in btsc_picker.tsv, and that file's row
// order IS the list's order -- there is no sort key to keep in step with it. `rank` only decides
// between fights the TSV does not mention at all.
typedef struct { uint16_t order; uint8_t rank; uint16_t btsc; } bs_key;
static int bs_key_cmp(const void* a, const void* b) {
    const bs_key* x = (const bs_key*)a;
    const bs_key* y = (const bs_key*)b;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    return x->btsc < y->btsc ? -1 : (x->btsc > y->btsc);
}

// Fill the list with every loaded scene in the current region band (btsc06xxx for Gapra, ...).
static void bs_scan(void) {
    // Built from the extracted scene table; do NOT go back to asking the game per scene. A census
    // of every band showed every scene resident at all times, and 1000 lookups took long enough
    // that the picker had not opened when the key was pressed again.
    uint32_t zone = field_zone_id();
    uint32_t b = zb_lookup(zone);
    uint32_t band = (b != 0xFFFFFFFFu) ? b * 1000
                  : (g_lastBtscId ? (g_lastBtscId / 1000) * 1000 : 0);
    // Ordered by hand first (btsc_picker.tsv's sort column), then by kind, then by number.
    // A band is not a zone: Gran Pulse's nine zones share btsc11xxx, so a fight is kept only when a
    // character set of THIS zone holds its models (scene_belongs_here). Skipped when the zone is
    // unknown or has no sets in the table, since then the answer would be "none of them".
    int filterZone = zone != 0xFFFFFFFFu && zone_has_charasets(zone);
    int dropped = 0;
    int n = 0;
    // Every candidate, not just the first BS_MENU_MAX of them: the cap is applied AFTER sorting, so
    // it drops the least important rows. Static rather than ~10KB of stack.
    static bs_key keys[FF13_BTSC_TABLE_N];
    for (int i = 0; i < FF13_BTSC_TABLE_N; i++) {
        uint32_t id = kBtscTable[i].btsc;
        if (!band_listed_here(zone, band, id)) continue;
        if (filterZone && !scene_belongs_here(zone, id)) { dropped++; continue; }
        unsigned order = FF13_BTSC_ORDER_LAST;
        btsc_hand(id, 0, NULL, NULL, &order, NULL, NULL);
#ifdef FF13_HAVE_BTSC_NAMES
        // The TSV IS the list: a fight it does not mention is not shown, which is how the test
        // battles and dev-only bands are kept out. Without the header there is no file to consult,
        // so everything the zone can field is listed.
        if (order == FF13_BTSC_ORDER_LAST) continue;
#endif
        keys[n].order = (uint16_t)order;
        keys[n].rank = (uint8_t)((kBtscTable[i].kind & FF13_BTSC_BOSS)    ? 0 :
                                 (kBtscTable[i].kind & FF13_BTSC_EVENT)   ? 1 :
                                 is_boss_scene(id)                        ? 2 :
                                 (kBtscTable[i].kind & FF13_BTSC_MISSION) ? 3 : 4);
        keys[n].btsc = (uint16_t)id;
        n++;
    }
    qsort(keys, n, sizeof keys[0], bs_key_cmp);
    // A fight that offers a form takes one row per form after its plain row, so the whole set sits
    // together. The cap counts ROWS, and a fight whose set does not fit whole is left out whole.
    int over = 0;
    int rows = 0;
    for (int i = 0; i < n; i++) {
        int want = 1 + aimode_count(keys[i].btsc);
        if (want > 1 + FORMS_MAX) want = 1 + FORMS_MAX;
        if (rows + want > BS_MENU_MAX) { over += want; continue; }
        // Inside the set, the TSV's row order decides too: mode N indexes kAiForms, and every form
        // row carries its own `order`. A form the file never names sorts last, keeping its mode
        // order among the other unnamed ones.
        uint8_t var[1 + FORMS_MAX];
        unsigned ord[1 + FORMS_MAX];
        for (int v = 0; v < want; v++) {
            unsigned o = FF13_BTSC_ORDER_LAST;
            btsc_hand(keys[i].btsc, v, NULL, NULL, &o, NULL, NULL);
            int at = v;                          // insertion sort: `want` is at most 7
            while (at > 0 && ord[at - 1] > o) { ord[at] = ord[at - 1]; var[at] = var[at - 1]; at--; }
            ord[at] = o;
            var[at] = (uint8_t)v;
        }
        for (int v = 0; v < want; v++) {
            g_bsList[rows] = keys[i].btsc;
            g_bsVar[rows]  = var[v];
            rows++;
        }
    }
    n = rows;
    g_bsCount = n;
    if (over)
        ff13logv("[FF13-SRP] battle picker: %d more rows match here than the list holds (%d); "
                 "the least important fights -- last by hand order, then by kind -- are not shown\n",
                 over, BS_MENU_MAX);
    if (ui_english()) {
        int missing = 0;
        for (int i = 0; i < n; i++) {
            const char* en = NULL;
            btsc_hand(g_bsList[i], g_bsVar[i], NULL, &en, NULL, NULL, NULL);
            if (!en) missing++;
        }
        if (missing)
            ff13logv("[FF13-SRP] battle picker: %d of %d fights here have no English name in "
                     "sr_practice/data/btsc_picker.tsv -- those rows show the Japanese one\n",
                     missing, n);
    }
    int wantMode = 0;
    uint32_t want = zb_sel(zone, &wantMode);
    int sel = -1;
    for (int i = 0; i < n && sel < 0; i++)
        if (g_bsList[i] == want && g_bsVar[i] == (uint8_t)wantMode) sel = i;
    for (int i = 0; i < n && sel < 0; i++) if (g_bsList[i] == want) sel = i;
    if (!want) for (int i = 0; i < n && sel < 0; i++) if (g_bsList[i] == g_lastBtscId) sel = i;
    if (sel < 0) sel = 0;
    g_bsSel = sel;
    char note[72];
    if (!filterZone) snprintf(note, sizeof note, " -- zone filter off (zone unknown to the table)");
    else if (dropped) snprintf(note, sizeof note, " (%d in this band belong to other zones, hidden)",
                               dropped);
    else note[0] = 0;
    uint32_t extra = 0;
    for (int i = 0; i < FF13_ZONE_EXTRA_N; i++)
        if (kZoneExtraBand[i].zone == zone) extra = kZoneExtraBand[i].band;
    if (extra)
        ff13log("[FF13-SRP] battle picker: %d scenes in bands btsc%05u.. and btsc%05u.. "
                 "(on btsc%05u)%s  [scenario flag %d]\n", n, band, extra,
                 n ? g_bsList[sel] : 0, note, vm_get_work(0));
    else
        ff13log("[FF13-SRP] battle picker: %d scenes in band btsc%05u..btsc%05u (on btsc%05u)%s"
                 "  [scenario flag %d]\n",
                 n, band, band + 999, n ? g_bsList[sel] : 0, note, vm_get_work(0));
}

// ---- form pin: which form a shape-changing boss takes first ----------------------------------
// A form change IS a charaspec swap: bt_chara_spec.wdb carries one variant per form. Every swap in
// the game, script-driven or ability-driven, goes out through
//   Battle.apiCharaReplaceCharaSpec = 0xBC0340   the script door (BattleChara.replaceCharaSpec)
//     -> 0x471140(this = actor, &name, kind)     the swap itself
//   0x48DD10 + 0x48E16C                          the ability door (the damage-source effect a
//                                                mode-change action carries)
// `name` is a FIXED 16-byte NUL-padded buffer on the CALLER's frame, so a hook at the entry of
// 0x471140 can write the wanted form into it before the game reads it. The roll is left alone.
// Do NOT steer the roll instead: the pin only has to look like one of its outcomes. What the roll
// draws from still has to be kept straight with it -- see on_ai_afterexec.
const uint8_t P_SPECREPL[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x20,0x02,0x00,0x00};
#define SPEC_NAME_LEN 16             // the game's own fixed key width, NUL-padded

// The last charaspec put on each SCENE OBJECT (the id at actor+8). The object outlives the fight
// and the next one binds a fresh actor to it, so the shape it was left in is what carries over --
// neither the actor (recycled) nor the scene (two objects of one enemy end in different forms) is
// the key (measured; CODE_NOTES).
#define OBJFORM_N 16
static struct { uint32_t obj; char nm[SPEC_NAME_LEN + 1]; DWORD at; } g_objForm[OBJFORM_N];
#define OFF_ACTOR_OBJ 0x08           // the actor's scene object id; -1 = none

static void obj_form_note(uint32_t actor, const char* nm) {
    if (!actor || IsBadReadPtr((void*)(uintptr_t)(actor + OFF_ACTOR_OBJ), 4)
        || IsBadReadPtr((void*)(uintptr_t)(actor + OFF_GUARD324), 4)) return;
    uint32_t obj = *(uint32_t*)(uintptr_t)(actor + OFF_ACTOR_OBJ);
    if (obj == 0xFFFFFFFFu) return;
    if (!*(uint32_t*)(uintptr_t)(actor + OFF_GUARD324)) return;      // enemies only (see iw_is_enemy)
    int slot = -1;
    for (int i = 0; i < OBJFORM_N; i++) if (g_objForm[i].obj == obj) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < OBJFORM_N; i++)
            if (!g_objForm[i].obj || (int)(g_objForm[i].at - g_objForm[slot].at) < 0) slot = i;
    }
    g_objForm[slot].obj = obj; g_objForm[slot].at = GetTickCount();
    strncpy(g_objForm[slot].nm, nm, SPEC_NAME_LEN); g_objForm[slot].nm[SPEC_NAME_LEN] = 0;
}

// -> the name this actor's object was last left in, or NULL if it was never swapped.
static const char* obj_form_left(uint32_t actor) {
    if (!actor || IsBadReadPtr((void*)(uintptr_t)(actor + OFF_ACTOR_OBJ), 4)) return NULL;
    uint32_t obj = *(uint32_t*)(uintptr_t)(actor + OFF_ACTOR_OBJ);
    if (obj == 0xFFFFFFFFu) return NULL;
    for (int i = 0; i < OBJFORM_N; i++) if (g_objForm[i].obj == obj) return g_objForm[i].nm;
    return NULL;
}

// The fights that offer a form, and the charaspec each form is. Picker mode 1..n indexes `forms`;
// the label in btsc_picker.tsv and the charaspec installed here come from the same row.
// `acts` is the same forms one step upstream: the ability the AI runs to change form. Redirect the
// ABILITY, not just the charaspec -- a charaspec redirected on its own leaves the element the player
// sees unchanged, because that comes from the ability's own effects.
// `aim` names the part a buff should land on, for the fights where that is a separate question from
// which buff it is. NULL leaves the AI's own choice alone.
// `flag` is the AI generic flag the mode's ability spends (0 = this fight keeps no such
// bookkeeping). A bend has to correct it -- see on_ai_afterexec.
static const struct { uint16_t btsc; const char* base;
                      const char* forms[FORMS_MAX]; const char* acts[FORMS_MAX];
                      const char* aim[FORMS_MAX]; const uint8_t flag[FORMS_MAX]; } kAiForms[] = {
    // Mode N means "the Nth entry here", and that binding is the only thing btsc_picker.tsv has to
    // keep. Row order in the file decides where a row is shown, so these can stay in any order.
    { 6046, "m097", { "m097_i",     "m097_f",     "m097_t",     "m097_w"     },   // ice / fire /
                    { "m097_ac510", "m097_ac500", "m097_ac520", "m097_ac530" },   // lightning / water
                    { NULL, NULL, NULL, NULL },
                    { 3, 2, 4, 5 } },
    // Barthandelus 1. Not a form change at all -- no charaspec is swapped, so `forms` is empty and
    // only the ability is pinned:
    //   m300_mg000 = enchant     m300_mg010 = enhance
    // MEASURED, and the opposite of what the data reads like -- a damage source's sounds are
    // dressing and are reused; they are not what the ability does.
    // Both are gated on the same ATB threshold and both take their target with T_COND_0 rather than
    // a roll, so WHICH part is hit is a separate question from which buff lands.
    // The parts are per-party: m301 is the one on Lightning's side, m303 the one on Snow's. Nothing
    // in the data names a side. The other two parts are reachable the same way and simply not
    // listed; add a row here and in btsc_picker.tsv if they are ever wanted.
    { 9055, "m300", { NULL },
                    { "m300_mg000", "m300_mg000", "m300_mg010", NULL, NULL, NULL },
                    { "m301",       "m303",       NULL,         NULL, NULL, NULL },
                    { 0 } },
};
#define AI_FORMS_N ((int)(sizeof(kAiForms)/sizeof(kAiForms[0])))

// ---- miss pin: the debuff that will not land -------------------------------------------------
// Shares the form pin's mode numbering: a fight is in one mode table, never two. The roll itself is
// left alone and the CHANCE it is about to be compared against is written instead -- see
// on_status_roll and common/docs/STATUS_ROLL_RE.md.
#define DBF_MAX 4
#define DBF_UNTIL_MAX 4
#define ST_DEPROTECT 29        // db/resident/status_effect.wdb "sys29"
#define ST_IMPERIL   23        // ...and "sys23"
// `until` names the abilities whose execution ends the pin instead of a count. When it is set,
// `misses` is not read at all and the pin fails EVERY roll of its status until one of them goes
// off; `base` is then the enemy whose action door decides that, and must be filled in too.
// `scale` outranks both: a percent there leaves the roll alone and keeps that much of the chance the
// game just worked out, for the whole fight. 50 = half as likely.
static const struct { uint16_t btsc; uint8_t status[DBF_MAX];
                      const char* on[DBF_MAX]; uint8_t misses[DBF_MAX];
                      const char* base; const char* until[DBF_UNTIL_MAX];
                      uint8_t scale[DBF_MAX]; } kDbfMiss[] = {
    // Sunleth Waterscape. The counts are per enemy and are what the user asked to rehearse, not
    // anything the fight imposes; btsc_picker.tsv's row labels spell them out and must agree.
    { 5027, { ST_DEPROTECT, ST_DEPROTECT },
            { "m234", "m233" },                        // 1 = Enki, 2 = Enlil
            {  5,      3     }, NULL, { NULL } },
    // Orphan's Cradle. Jabberwocky is fought without a debuff of its own, so it gets no row.
    { 12081, { ST_IMPERIL },
             { "m235" },                               // 1 = Bandersnatch (m236 is Jabberwocky)
             {  5     }, NULL, { NULL } },
    // Barthandelus, the last one. m320's AI (AiEnemyBossImplement, block "m320") returns Ultima as
    // its fallthrough answer, so ac700/ac701 are the two it can commit to; ac702/ac703 are the same
    // ability, listed in case a script reaches for them.
    { 12090, { ST_DEPROTECT, ST_IMPERIL },
             { "m320", "m320" },                       // 1 = Imperil only, 2 = Deprotect only
             {  0,      0     }, "m320",
             { "m320_ac700", "m320_ac701", "m320_ac702", "m320_ac703" } },
    // Orphan's Cradle, Wladislaus.
    { 12084, { ST_DEPROTECT },
             { "m242" },
             {  0     }, NULL, { NULL },
             { 45     } },
};
#define DBF_MISS_N ((int)(sizeof(kDbfMiss)/sizeof(kDbfMiss[0])))

// ---- launch pin: the break reaction that will not lift ---------------------------------------
// The fourth mode table, sharing the numbering of the three around it. `scale` is the percent of the
// launch's real success rate to keep, for the whole fight -- as kDbfMiss's scale, except that what
// is scaled is an integer resistance, not a float chance. See common/docs/LAUNCH_ROLL_RE.md.
#define LAU_MAX 4
static const struct { uint16_t btsc; const char* on[LAU_MAX]; uint8_t scale[LAU_MAX]; } kLauMiss[] = {
    // Eden, the Proudclad's second fight. Launch is one roll against the target's blast resistance,
    // 50 on m154. m154_pw, the later form, carries the same 50 and the spec match covers it.
    { 12042, { "m154" }, { 40 } },
};
#define LAU_MISS_N ((int)(sizeof(kLauMiss)/sizeof(kLauMiss[0])))

static int laumiss_count(uint32_t btsc) {
    for (int i = 0; i < LAU_MISS_N; i++) {
        if (kLauMiss[i].btsc != btsc) continue;
        int n = 0;
        while (n < LAU_MAX && kLauMiss[i].on[n]) n++;
        return n;
    }
    return 0;
}

static int dbfmiss_count(uint32_t btsc) {
    for (int i = 0; i < DBF_MISS_N; i++) {
        if (kDbfMiss[i].btsc != btsc) continue;
        int n = 0;
        while (n < DBF_MAX && kDbfMiss[i].on[n]) n++;
        return n;
    }
    return 0;
}

// ---- opening pin: the attacks a fight opens with, made to be one named ability ----------------
// The third mode table, sharing the numbering of the two above. The bend is the form pin's ability
// door (0x4943B0); what ends it is the enemy's first action going through (on_ai_state), with its
// first break as a backstop -- see there for why one attack is the whole pattern.
#define OPEN_MAX      4
#define OPEN_CAND_MAX 6
#define ST_SLOW       22       // ...and "sys22"
static const struct { uint16_t btsc; const char* base;
                      const char* cand[OPEN_CAND_MAX];
                      const char* act[OPEN_MAX]; const char* alt[OPEN_MAX];
                      uint8_t status[OPEN_MAX]; uint8_t lands[OPEN_MAX];
                      uint8_t skip[OPEN_MAX]; uint8_t who[OPEN_MAX]; } kAiOpen[] = {
    // Orphan's Cradle, Tiamat Eliminator. `cand` is its flying form's three attacks: ac500 and
    // ac510 are both Ice Grenades (one display name, two entries), ac600 is Tail Hammer. Overdrive
    // (ac700) and Descent (ac800) are deliberately NOT listed -- bending those steers the fight out
    // of the form. Ice Grenades is five bullets, each with a 5% Slow (damagesrc m137_ac500_00.._04,
    // sys22).
    { 12083, "m137",
      { "m137_ac500", "m137_ac510", "m137_ac600" },
      { "m137_ac500", "m137_ac500", "m137_ac500", "m137_ac600" },  // 1..3 Ice Grenades, 4 Tail Hammer
      { "m137_ac510", "m137_ac510", "m137_ac510", NULL         },  // ac510 already IS Ice Grenades
      {  ST_SLOW,      ST_SLOW,      ST_SLOW,      0            },
      {  1,            1,            1,            0            },
      {  1,            1,            1,            1            },  // all of them on the second go
      {  1,            2,            3,            0            } }, // leader / 2nd / 3rd
};
#define AI_OPEN_N ((int)(sizeof(kAiOpen)/sizeof(kAiOpen[0])))

static int openmode_count(uint32_t btsc) {
    for (int i = 0; i < AI_OPEN_N; i++) {
        if (kAiOpen[i].btsc != btsc) continue;
        int n = 0;
        while (n < OPEN_MAX && kAiOpen[i].act[n]) n++;
        return n;
    }
    return 0;
}

// ---- put-back mode: what the fight is allowed to leave behind --------------------------------
// The fourth mode table, sharing the numbering of the three above. Nothing about the enemy is bent:
// mode 1 keeps the scan the fight teaches across the way back, mode 0 undoes it like any other
// pick.
static const uint16_t kLibraKeepMode[] = { 3032, 4026, 7037, 9041 };
#define LIBRA_KEEPMODE_N ((int)(sizeof(kLibraKeepMode)/sizeof(kLibraKeepMode[0])))

static int librakeep_count(uint32_t btsc) {
    for (int i = 0; i < LIBRA_KEEPMODE_N; i++)
        if (kLibraKeepMode[i] == (uint16_t)btsc) return 1;
    return 0;
}

// The kept rows that also keep the paradigm the fight ENDS on in hand: the first half of a
// back-to-back pair whose second half opens on it in the story. User-specified; btsc09041's second
// half resets the paradigm in the story, and btsc07037's next fight changes the party.
static const uint16_t kSelKeepMode[] = { 3032, 4026 };
#define SEL_KEEPMODE_N ((int)(sizeof(kSelKeepMode)/sizeof(kSelKeepMode[0])))

static int selkeep_count(uint32_t btsc) {
    for (int i = 0; i < SEL_KEEPMODE_N; i++)
        if (kSelKeepMode[i] == (uint16_t)btsc) return 1;
    return 0;
}

// How many choices this fight offers, 0 for "it has none".
static int aimode_count(uint32_t btsc) {
    for (int i = 0; i < AI_FORMS_N; i++) {
        if (kAiForms[i].btsc != btsc) continue;
        int n = 0;
        while (n < FORMS_MAX && kAiForms[i].acts[n]) n++;
        return n;
    }
    int n = dbfmiss_count(btsc);
    if (n) return n;
    n = laumiss_count(btsc);
    if (n) return n;
    n = openmode_count(btsc);
    return n ? n : librakeep_count(btsc);
}

// Long enough to cover the preload cap plus the minutes of fighting before the form comes up, short
// enough that a pin can never reach a later fight.
#define AIPIN_ARM_MS 300000
#define AIPIN_LONG_ARM_MS 1800000   // a pin that only the fight ending takes down (see aipin_arm)
#ifdef FF13_DIAG
static char              g_formSeen[SPEC_NAME_LEN + 1];
static volatile DWORD    g_formWatch = 0;
#endif
#define ACT_NAME_LEN 31
static char              g_formWant[SPEC_NAME_LEN + 1];   // the charaspec to install, "" = no pin
static char              g_formAim[SPEC_NAME_LEN + 1];    // ...the part it should land on, and
static char              g_formAct[ACT_NAME_LEN + 1];     // ...the ability that installs it, and
static char              g_formBase[SPEC_NAME_LEN + 1];   // ...the enemy both belong to
static volatile DWORD    g_formUntil = 0;
static volatile uint32_t g_formScene = 0;                 // for the log only
static volatile int      g_formMode  = 0;
static volatile int      g_formActSaid  = 0;   // the log has named the redirect once
// How long a bend lasts once it has started, for a fight with no charaspec swap to end it on: one
// decision's worth.
#define PIN_ACT_MS 4000
static volatile DWORD    g_formActBent  = 0;  // when the bend expires, 0 = it has not started
// 1 = this pin ends at the charaspec swap, so the bend timeout must NOT end it: the AI can ask
// about its mode-change candidates seconds before it commits, and a pin that dies in between
// leaves the first change unbent (measured once on 6046 -- a non-picked element came up).
static volatile int      g_formSwapEnds = 0;
// The enemy's own record of which modes are still unspent this cycle: a bend has to move the mark
// onto the mode the player is shown, or the pinned one stays unspent and can come round again at
// once (see on_ai_afterexec).
static volatile int      g_formFlag = 0;      // the flag the pinned mode spends, 0 = none
static volatile unsigned g_formFlagPool = 0;  // every flag this fight's modes spend, as a bit set
// The aim's own clock, started by the first target taken: the resolve can come long after the bend,
// so it cannot hang off PIN_ACT_MS. Holds one decision's pair; decisions are a minute apart.
#define AIM_HOLD_MS 3000
static volatile DWORD    g_formAimUntil = 0;  // when the aim stands down, 0 = it has not fired yet
// ...and aim_hold's state, declared here because aipin_arm clears it.
static volatile DWORD    g_aimHoldUntil = 0;
static volatile DWORD    g_aimHoldFrom  = 0;
static volatile int      g_aimHoldSaid  = 0;
// The miss pin's own state.
static char              g_dbfOn[SPEC_NAME_LEN + 1];   // the enemy the misses are aimed at
static volatile int      g_dbfStatus = 0;
// How many rolls are still owed, and the whole switch: 0 = nothing is being made to fail, -1 = every
// roll fails until g_dbfUntilAct goes off.
static volatile int      g_dbfLeft   = 0;
static volatile DWORD    g_dbfUntil  = 0;
static char              g_dbfUntilAct[ACT_NAME_LEN + 1];  // "" = a count ends this pin, not an ability
// >0 = the rolls are not forced at all; this percent of their chance is kept, for the whole fight.
static volatile int      g_dbfScale  = 0;
static volatile int      g_dbfLandN  = 0;   // rolls that came up yes, against the same enemy
#define DBF_LAND_LOG_MAX 300
static volatile int      g_dbfUntilSeen = 0;   // the AI's answer right now IS that ability
static volatile int      g_dbfMissN  = 0;      // rolls this pin has acted on, for the standdown line
// The launch pin's own state. g_lauScale is the whole switch: the percent of the real rate kept.
static char              g_lauOn[SPEC_NAME_LEN + 1];
static volatile int      g_lauScale  = 0;
static volatile DWORD    g_lauUntil  = 0;
static volatile int      g_lauRollN  = 0;   // launch rolls this pin has acted on...
static volatile int      g_lauDownN  = 0;   // ...and how many of them left the target on the ground
#define LAU_LOG_MAX 300
// ...and its mirror image, which the opening pin uses: a roll made to LAND. No enemy goes with it
// -- the rider belongs to the pinned enemy's own attack, so the target is whoever it hit.
static volatile int      g_dbfLandStatus = 0;
static volatile int      g_dbfLandLeft   = 0;
// ...held back until the pinned go's decision is over: a volley's rolls arrive AFTER the state
// change that ends its go, so a land armed earlier is spent on the go that was let past (measured).
static volatile int      g_dbfLandWant   = 0;
static volatile int      g_dbfLandSeen   = 0;
#define LAND_LOG_MAX 10
// The opening pin's own state. g_openPin is the whole switch.
static volatile int      g_openPin  = 0;
static volatile int      g_openBent = 0;   // candidate rewrites so far, for the log
// Goes to let the enemy have before the bending starts. A go is an attempt, not a hit -- see the
// state setter.
static volatile int      g_openSkip = 0;
// 1 = the enemy has been seen choosing among `cand` since the last go was counted. The state setter
// repeats within one action and fires at the start of the fight, and neither of those is a go.
static volatile int      g_openDecided = 0;
static volatile int      g_openIgn  = 0;   // how many times a state change was passed over
// Which party member the go is aimed at: 1 = the leader, 2 and 3 = the other two, 0 = leave the
// enemy's own lot alone. g_openAimed says whether the aim reached the field this go.
static volatile int      g_openWho   = 0;
static volatile int      g_openAimed = 0;
static char              g_openAimOn[SPEC_NAME_LEN + 1];   // ...and the spec it named
#define OPEN_LOG_MAX 12
// 1 = this pick's Libra gain is held across the way back. Armed per pick rather than looked up by
// scene at fight-over, so a fight that offers the mode still resets when its plain row was chosen.
static volatile int      g_libraKeepPick = 0;
static volatile int      g_selKeepPick   = 0;   // ...and the paradigm it ends on stays in hand

// Arm the pin for a fight the picker is about to start. mode 0 (or a fight with no forms) clears it,
// so a plain pick disarms whatever an earlier one left behind.
static void aipin_arm(uint32_t btsc, int mode) {
    extra_force_reset();          // a fresh pick starts the forced-extra draw over as well
    g_formWant[0] = 0; g_formAct[0] = 0; g_formAim[0] = 0; g_formBase[0] = 0;
    g_formScene = 0; g_formMode = 0;
    g_formActSaid = 0; g_formActBent = 0; g_formSwapEnds = 0; g_formAimUntil = 0;
    g_formFlag = 0; g_formFlagPool = 0;
    g_aimHoldUntil = 0; g_aimHoldFrom = 0; g_aimHoldSaid = 0;
    g_dbfOn[0] = 0; g_dbfStatus = 0; g_dbfLeft = 0; g_dbfScale = 0;
    g_dbfLandN = 0;
    g_dbfUntilAct[0] = 0; g_dbfUntilSeen = 0; g_dbfMissN = 0;
    g_dbfLandStatus = 0; g_dbfLandLeft = 0; g_dbfLandWant = 0; g_dbfLandSeen = 0;
    g_lauOn[0] = 0; g_lauScale = 0; g_lauRollN = 0; g_lauDownN = 0;
    g_openPin = 0; g_openBent = 0; g_openSkip = 0;
    g_openIgn = 0; g_openDecided = 0; g_openWho = 0; g_openAimed = 0;
    g_openAimOn[0] = 0;
    g_libraKeepPick = 0; g_selKeepPick = 0;
    if (librakeep_count(btsc)) {
        if (mode != 1) return;
        g_libraKeepPick = 1;
        g_selKeepPick = selkeep_count(btsc);
        ff13logv("[FF13-SRP] battle picker: btsc%05u mode %d -- what this fight teaches about its "
                 "enemies is kept, not put back%s\n", btsc, mode,
                 g_selKeepPick ? ", and neither is the paradigm it ends on" : "");
        return;
    }
    for (int i = 0; i < LAU_MISS_N; i++) {
        if (kLauMiss[i].btsc != btsc) continue;
        if (mode <= 0 || mode > laumiss_count(btsc)) return;
        strncpy(g_lauOn, kLauMiss[i].on[mode - 1], SPEC_NAME_LEN);
        g_lauOn[SPEC_NAME_LEN] = 0;
        // AIPIN_ARM_MS would expire mid-fight on a rehearsed boss and read as the pin breaking.
        g_lauUntil = GetTickCount() + AIPIN_LONG_ARM_MS;
        g_lauScale = kLauMiss[i].scale[mode - 1];   // armed last: the hook tests this one
        ff13logv("[FF13-SRP] launch pin: btsc%05u mode %d armed -- every launch roll against %s "
                 "keeps %d%% of the chance it would have had\n", btsc, mode, g_lauOn, g_lauScale);
        return;
    }
    for (int i = 0; i < DBF_MISS_N; i++) {
        if (kDbfMiss[i].btsc != btsc) continue;
        if (mode <= 0 || mode > dbfmiss_count(btsc)) return;
        strncpy(g_dbfOn, kDbfMiss[i].on[mode - 1], SPEC_NAME_LEN);
        g_dbfOn[SPEC_NAME_LEN] = 0;
        g_dbfStatus = kDbfMiss[i].status[mode - 1];
        g_dbfUntil  = GetTickCount() + AIPIN_ARM_MS;
        if (kDbfMiss[i].scale[mode - 1]) {
            // AIPIN_ARM_MS would expire mid-fight on a rehearsed boss and read as the pin breaking.
            g_dbfUntil = GetTickCount() + AIPIN_LONG_ARM_MS;
            g_dbfScale = kDbfMiss[i].scale[mode - 1];
            g_dbfLeft  = -1;                          // armed last: the hook tests this one
            ff13logv("[FF13-SRP] miss pin: btsc%05u mode %d armed -- every roll of status %d "
                     "against %s keeps %d%% of the chance it would have had\n",
                     btsc, mode, g_dbfStatus, g_dbfOn, g_dbfScale);
            return;
        }
        if (kDbfMiss[i].until[0]) {
            // g_formBase is what both of the doors that end this pin resolve the actor by, so an
            // ability-ended pin has to fill it in even though it bends nothing about the enemy.
            strncpy(g_formBase, kDbfMiss[i].base ? kDbfMiss[i].base : "", SPEC_NAME_LEN);
            g_formBase[SPEC_NAME_LEN] = 0;
            strncpy(g_dbfUntilAct, kDbfMiss[i].until[0], ACT_NAME_LEN);
            g_dbfUntilAct[ACT_NAME_LEN] = 0;
            g_formScene = btsc; g_formMode = mode;
            g_formUntil = g_dbfUntil;
            g_dbfLeft = -1;                           // armed last: the hook tests this one
            ff13logv("[FF13-SRP] miss pin: btsc%05u mode %d armed -- every roll of status %d "
                     "against %s fails until %s gets \"%s\" out\n",
                     btsc, mode, g_dbfStatus, g_dbfOn, g_formBase, g_dbfUntilAct);
            return;
        }
        g_dbfLeft   = kDbfMiss[i].misses[mode - 1];   // armed last: the hook tests this one
        ff13logv("[FF13-SRP] miss pin: btsc%05u mode %d armed -- the next %d rolls of status %d "
                 "against %s are made to fail\n", btsc, mode, g_dbfLeft, g_dbfStatus, g_dbfOn);
        return;
    }
    for (int i = 0; i < AI_FORMS_N; i++) {
        if (kAiForms[i].btsc != btsc) continue;
        if (mode <= 0 || mode > aimode_count(btsc)) return;
        strncpy(g_formBase, kAiForms[i].base, SPEC_NAME_LEN);
        g_formBase[SPEC_NAME_LEN] = 0;
        g_formScene = btsc; g_formMode = mode;
        g_formUntil = GetTickCount() + AIPIN_ARM_MS;
        // The ability first, the charaspec second: the ability door is upstream. Both are armed
        // last -- the hooks run on the game thread and these are what they test to decide there is
        // anything to do.
        strncpy(g_formAct, kAiForms[i].acts[mode - 1], ACT_NAME_LEN);
        g_formAct[ACT_NAME_LEN] = 0;
        if (kAiForms[i].aim[mode - 1]) {
            strncpy(g_formAim, kAiForms[i].aim[mode - 1], SPEC_NAME_LEN);
            g_formAim[SPEC_NAME_LEN] = 0;
        }
        for (int m = 0; m < FORMS_MAX; m++)
            if (kAiForms[i].flag[m]) g_formFlagPool |= 1u << kAiForms[i].flag[m];
        g_formFlag = kAiForms[i].flag[mode - 1];
        if (kAiForms[i].forms[mode - 1]) {
            strncpy(g_formWant, kAiForms[i].forms[mode - 1], SPEC_NAME_LEN);
            g_formWant[SPEC_NAME_LEN] = 0;
            g_formSwapEnds = 1;
        }
#ifdef FF13_DIAG
        g_formSeen[0] = 0;
        g_formWatch = GetTickCount() + AIPIN_ARM_MS;
#endif
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d armed -- the first one becomes \"%s\"%s%s%s%s\n",
                 btsc, mode, g_formAct, g_formWant[0] ? ", charaspec " : "",
                 g_formWant[0] ? g_formWant : "", g_formAim[0] ? ", aimed at " : "",
                 g_formAim[0] ? g_formAim : "");
        return;
    }
    for (int i = 0; i < AI_OPEN_N; i++) {
        if (kAiOpen[i].btsc != btsc) continue;
        if (mode <= 0 || mode > openmode_count(btsc)) return;
        strncpy(g_formBase, kAiOpen[i].base, SPEC_NAME_LEN);
        g_formBase[SPEC_NAME_LEN] = 0;
        g_formScene = btsc; g_formMode = mode;
        g_formUntil = GetTickCount() + AIPIN_ARM_MS;
        g_dbfUntil  = g_formUntil;
        g_dbfLandStatus = kAiOpen[i].status[mode - 1];
        g_dbfLandWant   = kAiOpen[i].lands[mode - 1];
        strncpy(g_formAct, kAiOpen[i].act[mode - 1], ACT_NAME_LEN);
        g_formAct[ACT_NAME_LEN] = 0;
        g_openSkip = kAiOpen[i].skip[mode - 1];
        g_openWho  = kAiOpen[i].who[mode - 1];
        g_openPin = 1;                                // armed last: the hooks test this one
        ff13logv("[FF13-SRP] opening pin: btsc%05u mode %d armed -- \"%s\" for %s's attack %d%s%s%s\n",
                 btsc, mode, g_formAct, g_formBase, g_openSkip + 1,
                 g_openWho == 1 ? ", aimed at the leader" :
                 g_openWho == 2 ? ", aimed at member 2" :
                 g_openWho == 3 ? ", aimed at member 3" : "",
                 g_dbfLandWant ? ", and one roll of its rider is made to land" : "",
                 "");
        return;
    }
}

// The battle actor table. The index into it IS the actor id a conditional target is named by, so
// this is both how a part is found and how one is named.
#define OFF_ACTOR_TABLE  0x13C       // -> array of actor pointers
// Where a conditional target is handed over. T_COND_0 (target kind 22) picks nothing: it reads this
// field, hands it back and clears it to -1 (0x5592F4). B = actorObj + 0x1370, the AI argument block.
#define OFF_AI_CONDTARGET 0x18FC     // actorObj + 0x1370 + 0x58C

static int actor_name_of(uint32_t a, char* out) {
    out[0] = 0;
    if (!a || IsBadReadPtr((void*)(uintptr_t)(a + OFF_ACTOR_NAME), SPEC_NAME_LEN)) return 0;
    memcpy(out, (const void*)(uintptr_t)(a + OFF_ACTOR_NAME), SPEC_NAME_LEN);
    out[SPEC_NAME_LEN] = 0;
    for (int i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7E) { out[i] = 0; break; }
    return out[0] ? 1 : 0;
}

// A charaspec name is the pinned one when it is that spec or a variant of it ("m300"/"m300_hard").
static int spec_is(const char* nm, const char* base) {
    size_t n = strlen(base);
    return !strncmp(nm, base, n) && (!nm[n] || nm[n] == '_');
}

// -> the actor table, or NULL.
uint32_t* actor_table(void) {
    if (!g_base) return NULL;
    uintptr_t p = g_base + RVA_BTL_ACTORS;
    if (IsBadReadPtr((void*)p, 4)) return NULL;
    uint32_t mgr = *(uint32_t*)p;
    if (!mgr || IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_ACTOR_TABLE), 4)) return NULL;
    uint32_t tbl = *(uint32_t*)(uintptr_t)(mgr + OFF_ACTOR_TABLE);
    if (!tbl || IsBadReadPtr((void*)(uintptr_t)tbl, ACTOR_SLOTS * 4)) return NULL;
    return (uint32_t*)(uintptr_t)tbl;
}

// -> the actor whose charaspec starts with `name`, and its slot through *slot. Prefix rather than an
// exact match, because a boss can change spec mid-fight (m300 -> m300_hard) and still be the actor.
static uint32_t actor_by_prefix(const char* name, int* slot) {
    uint32_t* tbl = actor_table();
    if (!tbl || !name || !name[0]) return 0;
    size_t n = strlen(name);
    char nm[SPEC_NAME_LEN + 1];
    for (int i = 0; i < ACTOR_SLOTS; i++) {
        if (!actor_name_of(tbl[i], nm)) continue;
        if (strncmp(nm, name, n)) continue;
        if (slot) *slot = i;
        return tbl[i];
    }
    return 0;
}

// ---- the preemptive strike, and where it lives -----------------------------------------------
// A picked fight always opens with a preemptive strike. What decides it is NOT the chara bit 109 at
// [chara + 0x24E] (that is a momentary marker on whoever takes the free hit) and NOT bit 7 of the
// encounter word at [0x282B190] + 0x1CC (a copy of the decision; it correlated for six battles and
// then disagreed with itself). It is field 4 of the bit array at [0x282B190] + 0x228, decided inside
// 0xB50270 while the encounter is built from the touched object, and set through the bit setter
// 0xB5E9F0(mgr + 0x228, 4, v):
//     field 4 = 0   the party gets the free hit
//     field 4 = 1   they saw you coming            <- what "no ambush" writes
// Measured against the VERDICT line, four encounters, unanimous. [battle_swap] picker_preemptive =
// off writes 1 on a picked fight, = on writes 0; nothing is written by default.
// Hooked at 0xB505B6, the join all five branches of the decision fall through to, so the value read
// there is the decision after every branch has had its say. `lea eax,[esp+0x130]` -- seven bytes,
// esp-relative, which is fine because the trampoline's pushad/popad leaves esp where it found it.
const uint8_t P_ENCF4[7] = {0x8D,0x84,0x24,0x30,0x01,0x00,0x00};
#define ENC_FIELD_SEEN 4               // 0xB5E9F0(mgr+0x228, 4, v)

// ---- ...and the one thing that should be allowed to give the free hit back --------------------
// Deceptisol is an item whose whole purpose is to walk past enemies unnoticed, so a picked fight
// taken under one SHOULD open with the ambush rather than be overruled.
// UNVERIFIED: the gate below was the first candidate for "something is hiding you" and hardware
// disproved it. Its value is still logged for every encounter, picked or walked into.
// `off-always` is the unconditional spelling for anyone who wants the old behaviour.
#define RVA_SHROUD_MGR 0x23FF414       // VA 0x27FF414
#define OFF_SHROUD_GATE 0x298          // what 0x5AFFE0 returns
#define OFF_SHROUD_SIDE 0x290          // written from the same site, one line earlier

// ---- ...so read the decision's own inputs -----------------------------------------------------
// All five predicates are small pure getters taking the touched object id or nothing at all, and by
// the time the hook at the join runs, 0xB50270's own calls to them have already returned -- so
// calling them again is a READ, not a re-entry.
#define RVA_P_GATE   0x1AFFE0    // VA 0x5AFFE0()          the sub-tree gate
#define RVA_P_MODE   0x1B3560    // VA 0x5B3560()          the other argument-less one
#define RVA_P_OBJ1   0x1B1CC0    // VA 0x5B1CC0(id)        bit 1 of the object's flags at +0x84
#define RVA_P_OBJ2   0x1B0FC0    // VA 0x5B0FC0(id)
#define RVA_P_OBJ3   0x1AFF80    // VA 0x5AFF80(id)        bit 2 of the same flags
#define RVA_P_SCENE  0x7533E0    // VA 0xB533E0([mgr+0x354])
static const uint8_t P_P_GATE[6]  = {0x55,0x8B,0xEC,0x83,0xEC,0x08};
static const uint8_t P_P_14[6]    = {0x55,0x8B,0xEC,0x83,0xEC,0x14};   // MODE / OBJ1 / OBJ2 / OBJ3
static const uint8_t P_P_SCENE[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x6C};
typedef int (__cdecl *pred0_t)(void);
typedef int (__cdecl *pred1_t)(uint32_t);
static volatile int g_predOk = 0;         // every one of them matched its prologue

void preds_verify(void) {
    g_predOk = wait_for_bytes(g_base + RVA_P_GATE,  P_P_GATE,  6, 15000) >= 0
            && wait_for_bytes(g_base + RVA_P_MODE,  P_P_14,    6, 15000) >= 0
            && wait_for_bytes(g_base + RVA_P_OBJ1,  P_P_14,    6, 15000) >= 0
            && wait_for_bytes(g_base + RVA_P_OBJ2,  P_P_14,    6, 15000) >= 0
            && wait_for_bytes(g_base + RVA_P_OBJ3,  P_P_14,    6, 15000) >= 0
            && wait_for_bytes(g_base + RVA_P_SCENE, P_P_SCENE, 6, 15000) >= 0;
    ff13logv("[FF13-SRP] preemptive: the decision's five inputs: %s\n",
             g_predOk ? "readable" : "one or more did not match -- not read");
}

// ---- the fights that are SUPPOSED to open with a free hit -------------------------------------
// Two enemy groups already fighting each other are a preemptive strike every time they are played,
// whether you sneak up on one or walk straight in.
// The list comes from the picker table (kBtscAmbush in btsc_picker.h) and not from anything read at
// runtime, and that is where the fact is: at runtime the question cannot even be asked, because the
// decision is about the SYMBOL that was touched and the picker touches whatever live field object it
// can find, then swaps the scene underneath it.
// Which is also why these are WRITTEN to 0 rather than merely left alone: leaving one alone hands it
// to the same coin toss as everything else. A shroud does not override it either.
static int scene_always_ambush(uint32_t btsc) {
    if (!btsc || btsc > 0xFFFF) return 0;
    for (int i = 0; i < FF13_BTSC_AMBUSH_N; i++)
        if (kBtscAmbush[i] == (unsigned short)btsc) return 1;
    return 0;
}

static int shroud_gate(int* side) {
    if (side) *side = -1;
    if (!g_base) return -1;
    uintptr_t p = g_base + RVA_SHROUD_MGR;
    if (IsBadReadPtr((void*)p, 4)) return -1;
    uint32_t m = *(uint32_t*)p;
    if (!m || IsBadReadPtr((void*)(uintptr_t)(m + OFF_SHROUD_GATE), 4)) return -1;
    if (side && !IsBadReadPtr((void*)(uintptr_t)(m + OFF_SHROUD_SIDE), 4))
        *side = (int)*(uint32_t*)(uintptr_t)(m + OFF_SHROUD_SIDE);
    return (int)*(uint32_t*)(uintptr_t)(m + OFF_SHROUD_GATE);
}

#define ENC_BIT_NOTICED  0x00000080    // the disproven one; kept only so encinfo_tweak still builds
// Applied for a while rather than once: when vm_start_battle returns, the encounter info is not
// finished being built, so a single write on the way out changes nothing at all.
#define ENCTWEAK_MS 4000
// ...and a PICK is measured before the staging, where the time goes (a character-set load), so it
// gets its own longer window. The hold is dropped the moment the battle exists (below): by then the
// encounter info has been read and writing into it is pointless.
#define ENCTWEAK_PICK_MS 25000
static volatile uint32_t g_encClear = 0, g_encSet = 0;
static volatile DWORD    g_encUntil = 0;
static volatile uint32_t g_encLast = 0;
#define RVA_ENCMGR   0x242B190       // VA 0x282B190  the encounter manager
#define OFF_ENC_BITS 0x1CC           // ...and the encounter info in it: flags word, then a transform
#define OFF_RESULT_BITS 0x228        // ...and the battle result beside it (see restart_ask_by_bit)
#define OFF_CHARA_FLAGS  0x24E
#define FLAG_PREEMPTIVE  0x6D          // 109: see the assert at 0x47DA71
#define FLAG_IS_LEADER   0x01
#define PREEMPT_WATCH_MS 10000         // how long to keep reading the actors' flags

// __cdecl handler (ff13_install_ecx_hook); ecx is not used -- the manager is a global.
void __cdecl on_enc_field4(uint32_t ecx) {
    (void)ecx;
    if (!g_base) return;
    uintptr_t m = g_base + RVA_ENCMGR;
    if (IsBadReadPtr((void*)m, 4)) return;
    uint32_t mgr = *(uint32_t*)m;
    if (!mgr || IsBadWritePtr((void*)(uintptr_t)(mgr + OFF_RESULT_BITS), 4)) return;
    uint32_t* w = (uint32_t*)(uintptr_t)(mgr + OFF_RESULT_BITS);
    int was = (int)((*w >> ENC_FIELD_SEEN) & 1);
    int now = was;
    int side = -1, gate = shroud_gate(&side);
    // `mode` (0x5B3560) is the only predicate in the tree that is not per-symbol -- obj1/obj2/obj3
    // all read bits of the touched object's own flags at +0x84 -- and it reads one global
    // field-side byte array, which is the shape a party-wide field buff has. So mode == 1 is taken
    // to mean "something is hiding you". An assumption, not a measurement; `off-always` ignores it.
    int mode = (g_predOk) ? ((pred0_t)(uintptr_t)(g_base + RVA_P_MODE))() : 0;
    // A three-way is a free hit every time it is played, so it is WRITTEN, not left alone: leaving
    // it alone would hand it to the same coin toss as everything else.
    int byDesign = scene_always_ambush(g_bsStarted);
    // A shroud is the player's own doing and outranks the setting -- but not the fight's own design.
    int hidden = (mode == 1 && !byDesign);
    if (g_bsStarted && !hidden) {
        now = byDesign ? 0 : 1;
        if (now) *w |=  (1u << ENC_FIELD_SEEN);
        else     *w &= ~(1u << ENC_FIELD_SEEN);
    }
    ff13logv("[FF13-SRP] preemptive: the encounter decided field 4 = %d (%s)%s  [gate %d side %d]  "
             "(%s)\n", was, was ? "they saw you coming" : "free hit",
             now != was
                 ? (now ? " -> forced to 1 (no ambush)"
                        : (byDesign ? " -> forced to 0: a three-way is a free hit every time"
                                    : " -> forced to 0 (ambush)"))
                 : (byDesign ? " -> already 0: a three-way is a free hit every time"
                    : hidden ? " -> left alone: something is hiding you" : ""),
             gate, side, g_bsStarted ? "picked fight" : "walked into");
    (void)mode;
    if (g_predOk) {
        uint32_t id = g_lastTouchedId;
        uint32_t scn = IsBadReadPtr((void*)(uintptr_t)(mgr + 0x354), 4)
                     ? 0 : *(uint32_t*)(uintptr_t)(mgr + 0x354);
        ff13logv("[FF13-SRP] preemptive:   inputs for id=%u: scene(0xB533E0)=%d gate(0x5AFFE0)=%d "
                 "mode(0x5B3560)=%d obj1(0x5B1CC0)=%d obj2(0x5B0FC0)=%d obj3(0x5AFF80)=%d\n",
                 id,
                 ((pred1_t)(uintptr_t)(g_base + RVA_P_SCENE))(scn),
                 ((pred0_t)(uintptr_t)(g_base + RVA_P_GATE))(),
                 ((pred0_t)(uintptr_t)(g_base + RVA_P_MODE))(),
                 ((pred1_t)(uintptr_t)(g_base + RVA_P_OBJ1))(id),
                 ((pred1_t)(uintptr_t)(g_base + RVA_P_OBJ2))(id),
                 ((pred1_t)(uintptr_t)(g_base + RVA_P_OBJ3))(id));
    }
}

static int chara_flag(uint32_t a, int bit) {
    uintptr_t p = (uintptr_t)a + OFF_CHARA_FLAGS + (bit >> 3);
    if (!a || IsBadReadPtr((void*)p, 1)) return -1;
    return (*(uint8_t*)p & (1u << (bit & 7))) ? 1 : 0;
}

static int chara_flag_clear(uint32_t a, int bit) {
    uintptr_t p = (uintptr_t)a + OFF_CHARA_FLAGS + (bit >> 3);
    if (!a || IsBadWritePtr((void*)p, 1)) return 0;
    *(uint8_t*)p &= (uint8_t)~(1u << (bit & 7));
    return 1;
}

// Watched, not glanced at: the actor flags are not filled in on the first frame the actors exist
// (the first version read zero for every bit, "is an enemy" included). Bounded, because a fight that
// flips a bit every frame would otherwise write the log until the disk gave out.
#define PREEMPT_LOG_MAX 60
static void preempt_probe(int inBattle, uint32_t picked) {
    static int    said = 0;                       // the encounter word has been reported
    static DWORD  until = 0;                      // watching the actors until this tick
    static int    logN = 0;
    static int    verdict = 0;                    // 1 = somebody took the free hit; 2 = said so
    static uint8_t was[ACTOR_SLOTS];              // last (leader<<3 | preemptive<<1), +1 = seen
    if (!inBattle) {
        if (said && verdict == 0)
            ff13logv("[FF13-SRP] preemptive: VERDICT no free hit seen in %dms\n", PREEMPT_WATCH_MS);
        said = 0; until = 0; logN = 0; verdict = 0; memset((void*)was, 0, sizeof was); return;
    }
    DWORD now = GetTickCount();
    uint32_t* tbl = actor_table();
    if (!tbl) return;                             // the actors are not up yet; try again next frame
    if (!said) {
        int seen = 0;
        for (int i = 0; i < ACTOR_SLOTS; i++) if (tbl[i]) { seen = 1; break; }
        if (!seen) return;
        said = 1;
        until = now + PREEMPT_WATCH_MS;
        uintptr_t m = g_base + RVA_ENCMGR;
        uint32_t mgr = IsBadReadPtr((void*)m, 4) ? 0 : *(uint32_t*)m;
        if (mgr && !IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_ENC_BITS), 4)) {
            uint32_t f = *(uint32_t*)(uintptr_t)(mgr + OFF_ENC_BITS);
            ff13logv("[FF13-SRP] preemptive: encounter flags %08X (bit 7 %s -> %s), result bits "
                     "%08X (%s)\n", f, (f & ENC_BIT_NOTICED) ? "set" : "clear",
                     (f & ENC_BIT_NOTICED) ? "they saw you coming" : "PREEMPTIVE",
                     IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_RESULT_BITS), 4) ? 0
                         : *(uint32_t*)(uintptr_t)(mgr + OFF_RESULT_BITS),
                     picked ? "picked fight" : "walked into");
        }
        // The encounter info has been read by now; stop writing into it, so nothing of ours can
        // reach the next encounter the player walks into.
        g_encUntil = 0;
    }
    if ((int)(now - until) >= 0) {
        if (verdict == 0) { verdict = 2;
            ff13logv("[FF13-SRP] preemptive: VERDICT no free hit seen in %dms\n", PREEMPT_WATCH_MS);
        }
        return;
    }
    if (logN >= PREEMPT_LOG_MAX) return;
    for (int i = 0; i < ACTOR_SLOTS; i++) {
        uint32_t a = tbl[i];
        if (!a) continue;
        int pre = chara_flag(a, FLAG_PREEMPTIVE);
        if (pre < 0) continue;
        uint8_t cur = (uint8_t)(1 | (pre << 1)
                      | ((chara_flag(a, FLAG_IS_LEADER) == 1) << 3));
        if (was[i] == cur) continue;
        was[i] = cur;
        // What the fight ACTUALLY did, which the flags word only claims: bit 109 rising at all is
        // the free hit being taken. Said once per battle, so a session labels itself.
        if (pre && verdict == 0) {
            verdict = 1;
            char vn[SPEC_NAME_LEN + 1];
            if (!actor_name_of(a, vn)) vn[0] = 0;
            ff13logv("[FF13-SRP] preemptive: VERDICT free hit taken at %lums by slot %d \"%s\"%s\n",
                     (unsigned long)(now - (until - PREEMPT_WATCH_MS)), i, vn[0] ? vn : "?",
                     (cur & 8) ? " (the leader)" : "");
        }
#ifdef FF13_DIAG
        {
            char nm[SPEC_NAME_LEN + 1];
            if (!actor_name_of(a, nm)) nm[0] = 0;
            logN++;
            ff13logv("[FF13-SRP] preemptive: %5lums slot %2d \"%s\"%s preemptive=%d\n",
                     (unsigned long)(now - (until - PREEMPT_WATCH_MS)), i, nm[0] ? nm : "?",
                     (cur & 8) ? " leader" : "", pre);
        }
#else
        (void)logN;
#endif
    }
}

#ifdef FF13_DIAG
// What the enemy's charaspec actually IS, watched for a minute after a redirect: whether the write
// landed and whether the element the player sees comes from the charaspec at all are separate
// questions. Every actor whose name shares the enemy's first three characters, so the boss's parts
// are covered too -- its own pass, because the charaspec watch below stops at the boss.
static void parts_watch(void) {
    static DWORD next = 0;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 3000;
    uint32_t* tbl = actor_table();
    if (!tbl || !g_formBase[0]) return;
    char nm[SPEC_NAME_LEN + 1];
    for (int i = 0; i < ACTOR_SLOTS; i++) {
        if (!actor_name_of(tbl[i], nm)) continue;
        if (strncmp(nm, g_formBase, 3)) continue;
        int32_t ct = -1;
        if (!IsBadReadPtr((void*)(uintptr_t)(tbl[i] + OFF_AI_CONDTARGET), 4))
            ct = *(int32_t*)(uintptr_t)(tbl[i] + OFF_AI_CONDTARGET);
        ff13logv("[FF13-SRP] parts: slot %d is \"%s\" (its conditional target reads %d)\n", i, nm, ct);
    }
}

static void form_watch(void) {
    if (!g_formWatch || (int)(GetTickCount() - g_formWatch) >= 0 || !g_base) return;
    if (!g_formBase[0]) return;
    parts_watch();
    uintptr_t p = g_base + RVA_BTL_ACTORS;
    if (IsBadReadPtr((void*)p, 4)) return;
    uint32_t mgr = *(uint32_t*)p;
    if (!mgr || IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_ACTOR_TABLE), 4)) return;
    uint32_t tbl = *(uint32_t*)(uintptr_t)(mgr + OFF_ACTOR_TABLE);
    if (!tbl || IsBadReadPtr((void*)(uintptr_t)tbl, ACTOR_SLOTS * 4)) return;
    size_t n = strlen(g_formBase);
    for (int i = 0; i < ACTOR_SLOTS; i++) {
        uint32_t a = ((uint32_t*)(uintptr_t)tbl)[i];
        if (!a || IsBadReadPtr((void*)(uintptr_t)(a + OFF_ACTOR_NAME), SPEC_NAME_LEN)) continue;
        char nm[SPEC_NAME_LEN + 1];
        memcpy(nm, (const void*)(uintptr_t)(a + OFF_ACTOR_NAME), SPEC_NAME_LEN);
        nm[SPEC_NAME_LEN] = 0;
        for (int k = 0; nm[k]; k++)
            if ((unsigned char)nm[k] < 0x20 || (unsigned char)nm[k] > 0x7E) { nm[k] = 0; break; }
        if (strncmp(nm, g_formBase, n)) continue;          // only the enemy the pin is about
        if (!strcmp(nm, g_formSeen)) return;
        ff13logv("[FF13-SRP] form pin: the enemy's charaspec is now \"%s\"\n", nm);
        strncpy(g_formSeen, nm, SPEC_NAME_LEN);
        g_formSeen[SPEC_NAME_LEN] = 0;
        return;
    }
}

#endif

// ---- the conditional target, read at the moment it is used -----------------------------------
// 0x558E80(out, kind, actorId) resolves a target kind into an actor id. Kind 22 (T_COND_0) does no
// choosing: it hands back whatever a condition wrote to [B+0x58C] and clears the field. That field
// still reads -1 when the ability is picked, so the write happens in between -- which makes this the
// only place where the value that will actually be used can be seen, and the only place to replace
// it.
const uint8_t P_RESOLVETGT[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x80,0x01,0x00,0x00};
#define TGT_KIND_COND 22

// An actor id is a battle-tagged handle, not a table index: 0x4ED600 reads it as a SIGNED WORD and
// the high half counts the battle (0 in a launch's first fight, which is why a bare index ever
// seemed to work). Read the slot with actor_id_slot; NEVER build an id from a slot.
#define actor_id_slot(id)  ((int)(int16_t)((uint32_t)(id) & 0xFFFFu))

// An actor carries its own handle at +0x16C, where the game's own target selectors read it and
// store it back (ENEMY_AI_TARGET_RE.md 4). It is COPIED, never rebuilt from a slot -- a handle
// assembled out of a tag learned from some other id was wrong on hardware and the game ignored the
// write.
#define OFF_ACTOR_ID 0x16C
static uint32_t actor_handle(uint32_t a, int slot) {
    if (!a || IsBadReadPtr((void*)(uintptr_t)(a + OFF_ACTOR_ID), 4)) return 0;
    uint32_t h = *(uint32_t*)(uintptr_t)(a + OFF_ACTOR_ID);
    return (actor_id_slot(h) == slot) ? h : 0;    // it must name the actor it was read from
}

void __cdecl on_cond_target(uint32_t ecx, uint32_t kind, uint32_t* pActorId, uint32_t* pOut) {
    (void)ecx; (void)pOut;
    if (kind != TGT_KIND_COND || !pActorId) return;
    if (!g_formBase[0]) return;                      // only while a pinned fight is up
    uint32_t* tbl = actor_table();
    if (!tbl) return;
    int id = actor_id_slot(*pActorId);
    if (id < 0 || id >= ACTOR_SLOTS) return;
    uint32_t a = tbl[id];
    if (!a || IsBadReadPtr((void*)(uintptr_t)(a + OFF_AI_CONDTARGET), 4)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(a, nm) || strncmp(nm, g_formBase, 3)) return;   // not this fight's cast
    int32_t tgtRaw = *(int32_t*)(uintptr_t)(a + OFF_AI_CONDTARGET);
    int tgt = (tgtRaw < 0) ? -1 : actor_id_slot((uint32_t)tgtRaw);
    char tn[SPEC_NAME_LEN + 1];
    tn[0] = 0;
    if (tgt >= 0 && tgt < ACTOR_SLOTS) actor_name_of(tbl[tgt], tn);
    // EVERY kind-22 resolve, not one: a decision resolves twice, each with its own roll, and the
    // action can take either. Kind 22 is these two buffs' alone, and only the enemy itself resolves
    // it -- its parts pass the three-character test above but never reach here.
    int aimLive = g_formAim[0] && (int)(GetTickCount() - g_formUntil) < 0
               && !(g_formAimUntil && (int)(GetTickCount() - g_formAimUntil) >= 0);
    if (aimLive && !strncmp(nm, g_formBase, strlen(g_formBase))
        && !IsBadWritePtr((void*)(uintptr_t)(a + OFF_AI_CONDTARGET), 4)) {
        int want = -1;
        char pn[SPEC_NAME_LEN + 1];
        for (int i = 0; i < ACTOR_SLOTS; i++)
            if (actor_name_of(tbl[i], pn) && !strcmp(pn, g_formAim)) { want = i; break; }
        uint32_t wh = (want >= 0) ? actor_handle(tbl[want], want) : 0;
        if (wh && want != tgt) {
            *(uint32_t*)(uintptr_t)(a + OFF_AI_CONDTARGET) = wh;
            if (!g_formAimUntil) g_formAimUntil = GetTickCount() + AIM_HOLD_MS;
            ff13logv("[FF13-SRP] cond target: \"%s\" was to take slot %d%s%s -- aimed at slot %d "
                     "(%s) instead\n", nm, tgt, tn[0] ? " = " : "", tn[0] ? tn : "",
                     want, g_formAim);
            return;
        }
        if (want == tgt) {
            if (!g_formAimUntil) g_formAimUntil = GetTickCount() + AIM_HOLD_MS;
            ff13logv("[FF13-SRP] cond target: \"%s\" already takes slot %d (%s)\n", nm, tgt, g_formAim);
            return;
        }
    }
    ff13logv("[FF13-SRP] cond target: \"%s\" (slot %u) takes slot %d%s%s\n",
             nm, id, tgt, tn[0] ? " = " : "", tn[0] ? tn : "");
}

// ---- ...and the same target, held from the game thread ---------------------------------------
// A second line under the hook above, for a read that lands between resolves. Safe because
// [B+0x58C] is conditional targeting alone and these two buffs are its only kind-22 users.
#define AIM_WRITE_MS 20000      // covers a resolve that trails its ability by seconds
#define AIM_WRITE_LOG 12        // ...without a per-frame log

// Armed once per fight: a later decision is meant to be the game's own again.
static void aim_hold_arm(void) {
    if (!g_formAim[0] || g_aimHoldFrom) return;
    g_aimHoldFrom  = GetTickCount();
    g_aimHoldUntil = g_aimHoldFrom + AIM_WRITE_MS;
    ff13logv("[FF13-SRP] cond target: the decision is in -- holding its target on \"%s\" for %ds\n",
             g_formAim, AIM_WRITE_MS / 1000);
}

static void aim_hold(void) {
    if (!g_aimHoldUntil) return;
    // Only while the fight is up: battle actors are freed at fight-over but their pages stay
    // mapped with their bytes, so the name walk still "finds" the enemy and this writes into memory
    // the game has moved on from. IsBadWritePtr cannot tell the two apart; only this test can.
    if (!g_bsInFight) return;
    DWORD now = GetTickCount();
    if (!g_formAim[0] || (int)(now - g_aimHoldUntil) >= 0) { g_aimHoldUntil = 0; return; }
    // Stands down with the aim's window once a resolve has been steered: holding on writes into
    // the AI's later condition evaluations for nothing.
    if (g_formAimUntil && (int)(now - g_formAimUntil) >= 0) { g_aimHoldUntil = 0; return; }
    uint32_t* tbl = actor_table();
    if (!tbl) return;
    int bslot = -1;
    uint32_t boss = actor_by_prefix(g_formBase, &bslot);   // prefix: "m300" and its "m300_hard"
    if (!boss || IsBadWritePtr((void*)(uintptr_t)(boss + OFF_AI_CONDTARGET), 4)) return;
    int want = -1;
    char pn[SPEC_NAME_LEN + 1];
    for (int i = 0; i < ACTOR_SLOTS; i++)
        if (actor_name_of(tbl[i], pn) && !strcmp(pn, g_formAim)) { want = i; break; }
    if (want < 0) return;
    uint32_t* f = (uint32_t*)(uintptr_t)(boss + OFF_AI_CONDTARGET);
    uint32_t was = *f;
    // A bare index resolves but matches no id the game compares against, so nothing is written
    // until the actor's own handle can be read.
    uint32_t wh = actor_handle(tbl[want], want);
    if (!wh) return;
    int wasSlot = ((int32_t)was < 0) ? -1 : actor_id_slot(was);
    if (wasSlot == want) return;
    *f = wh;
    if (g_aimHoldSaid < AIM_WRITE_LOG) {
        g_aimHoldSaid++;
        char tn[SPEC_NAME_LEN + 1];
        tn[0] = 0;
        if (wasSlot >= 0 && wasSlot < ACTOR_SLOTS) actor_name_of(tbl[wasSlot], tn);
        ff13logv("[FF13-SRP] cond target: +%lums the field read %08X (slot %d%s%s) -- held on slot "
                 "%d (%s) as %08X\n", (unsigned long)(now - g_aimHoldFrom), was, wasSlot,
                 tn[0] ? " = " : "", tn[0] ? tn : "", want, g_formAim, wh);
    }
}

// A pin belongs to ONE fight and is taken down with it, never left to run out its window: an armed
// pin keeps a per-frame writer alive, and the actors it names are gone.
static void aipin_battle_over(void) {
    if (g_dbfScale && g_dbfMissN)
        ff13log("[FF13-SRP] miss pin: fight over -- status %d on %s: %d of %d rolls landed (%d%%)%s\n",
                g_dbfStatus, g_dbfOn, g_dbfLandN, g_dbfMissN,
                g_dbfLandN * 100 / g_dbfMissN, g_dbfScale ? ", chance scaled" : ", nothing bent");
    if (g_lauScale && g_lauRollN)
        ff13log("[FF13-SRP] launch pin: fight over -- %s: %d of %d launch rolls lifted it (%d%%), "
                "%d%% of the real rate kept\n", g_lauOn, g_lauRollN - g_lauDownN, g_lauRollN,
                (g_lauRollN - g_lauDownN) * 100 / g_lauRollN, g_lauScale);
    // Before the guard below: an ability-ended pin has no count to run out, so the fight ending is
    // the only thing that stops it, and an unspent count-ended one would outlive its fight too.
    g_dbfOn[0] = 0; g_dbfStatus = 0; g_dbfLeft = 0; g_dbfScale = 0;
    g_dbfLandN = 0;
    g_dbfUntilAct[0] = 0; g_dbfUntilSeen = 0;
    g_lauOn[0] = 0; g_lauScale = 0; g_lauRollN = 0; g_lauDownN = 0;
    if (!g_formBase[0] && !g_aimHoldUntil) return;
    g_formWant[0] = 0; g_formAct[0] = 0; g_formAim[0] = 0; g_formBase[0] = 0;
    g_formUntil = 0; g_formActBent = 0; g_formAimUntil = 0; g_formSwapEnds = 0;
    g_formFlag = 0; g_formFlagPool = 0;
    g_aimHoldUntil = 0; g_aimHoldFrom = 0; g_aimHoldSaid = 0;
    g_openPin = 0; g_openBent = 0; g_openSkip = 0;
    g_dbfLandStatus = 0; g_dbfLandLeft = 0; g_dbfLandWant = 0; g_dbfLandSeen = 0;
    g_openIgn = 0; g_openDecided = 0; g_openWho = 0; g_openAimed = 0;
    g_openAimOn[0] = 0;
}

// ---- the ability door, one step upstream of the charaspec -----------------------------------
// 0x4943B0 takes an ability label (a C string, [esp+4]) and queues that ability. Redirecting the
// label there swaps the whole mode-change action -- its effects and its presentation, not just the
// charaspec it happens to install.
const uint8_t P_ABILLABEL[9] = {0x55,0x8B,0xEC,0x81,0xEC,0xAC,0x00,0x00,0x00};

// The AI's own count of the attacks it has got out, low 7 bits, raised by AFTER_COUNTER_ADD_1
// (ENEMY_AI_TARGET_RE.md 2). Logged, never acted on: it LAGS a go by one (measured), and a build
// that counted goes by it put the pattern one attack late.
#define OFF_AI_ACTCNT (0x1370 + 0x5B0)

// Where apiAiSetEnemyTarget leaves the handle it drew (ENEMY_AI_TARGET_RE.md 3): the script sets
// the target BEFORE it returns the ability name, so the field is already filled in by the time the
// name reaches the door below. An entry that defers the draw to AFTER_TARGET_0 (Tail Hammer) never
// is.
#define OFF_AI_TARGET  (0x1360 + 0x598)
#define OFF_BS_LEADER  0x154         // the leader's handle (BATTLE_API_RE.md command 9)
// The three party slots, and the SAME array the target lot draws from (0x4EB460, and command 40's
// getRoleCount). Do NOT use bs+0x164/0x168 for this: those two read back as summon actors.
#define OFF_BS_PARTY   0x16C

static uint32_t battle_sys(void) {
    if (!g_base) return 0;
    uintptr_t p = g_base + RVA_BTL_ACTORS;
    if (IsBadReadPtr((void*)p, 4)) return 0;
    return *(uint32_t*)p;
}

// 1 = the leader, 2 and 3 = the other two in party-slot order. Which of those two is "member 2" is
// the array's order and nothing the game calls an order, so the log names the spec it picked.
static uint32_t open_member(int who) {
    uint32_t bs = battle_sys();
    if (!bs || IsBadReadPtr((void*)(uintptr_t)(bs + OFF_BS_LEADER), 4)) return 0;
    uint32_t lead = *(uint32_t*)(uintptr_t)(bs + OFF_BS_LEADER);
    if (who == 1) return ((int32_t)lead < 0) ? 0 : lead;
    if (IsBadReadPtr((void*)(uintptr_t)(bs + OFF_BS_PARTY), 3 * 4)) return 0;
    const uint32_t* p = (const uint32_t*)(uintptr_t)(bs + OFF_BS_PARTY);
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if ((int32_t)p[i] < 0 || p[i] == lead) continue;
        if (++n == who - 1) return p[i];
    }
    return 0;
}

// 0x54BE0F, inside apiAiSetEnemyTarget's worker: the handle the lot drew is about to be stored into
// [aiblk+0x598]. esp is ebp-8 here (the frame keeps a pointer and nothing is outstanding), so [esp]
// is the AI block and [esp+4] the drawn handle -- and popad restores registers, not the stack, so a
// handle written back through it is the one that gets stored.
// It has to be THIS door: at the label door the AI has not drawn a target yet, so the field is
// still empty when the name goes past (hardware -- nothing to replace, every time). An entry that
// defers to AFTER_TARGET_0, as Tail Hammer does, never comes here at all and stays unaimed.
static int prb_ptr_ok(uint32_t p);   // defined with the AI hooks below
const uint8_t P_AITARGET[11] = {0x8B,0x08,0x8B,0x55,0xF8,0x89,0x8A,0x98,0x05,0x00,0x00};
#define AITGT_SELF   0       // esp + : the AI block, whose first field is the actor id
#define AITGT_DRAWN  4       // esp + : the handle the lot drew

void __cdecl on_ai_target(uint32_t esp) {
    if (!g_openPin || g_openSkip > 0 || g_openWho < 1 || g_openWho > 3) return;
    if (!prb_ptr_ok(esp)) return;
    uint32_t aiblk = *(uint32_t*)(uintptr_t)(esp + AITGT_SELF);
    if (!prb_ptr_ok(aiblk)) return;
    uint32_t* tbl = actor_table();
    int slot = actor_id_slot(*(int32_t*)(uintptr_t)aiblk);
    char nm[SPEC_NAME_LEN + 1];
    if (!tbl || slot < 0 || slot >= ACTOR_SLOTS || !actor_name_of(tbl[slot], nm)) return;
    if (!spec_is(nm, g_formBase)) return;
    uint32_t want = open_member(g_openWho);
    if (!want) return;
    *(uint32_t*)(uintptr_t)(esp + AITGT_DRAWN) = want;
    if (!g_openAimed) {
        g_openAimed = 1;
        int ts = actor_id_slot(want);
        if (ts >= 0 && ts < ACTOR_SLOTS) actor_name_of(tbl[ts], g_openAimOn);
        ff13logv("[FF13-SRP] opening pin: aimed at %s (%s, handle %08X)\n",
                 g_openWho == 1 ? "the leader" : g_openWho == 2 ? "member 2" : "member 3",
                 g_openAimOn[0] ? g_openAimOn : "?", want);
    }
}

// Has the pinned enemy broken? That is an opening pin's backstop.
static int open_broken(void) {
    uint32_t a = actor_by_prefix(g_formBase, NULL);
    if (!a || IsBadReadPtr((void*)(uintptr_t)(a + OFF_CHARA_CHAIN + OFF_CHAIN_STATE), 4)) return 0;
    return *(int32_t*)(uintptr_t)(a + OFF_CHARA_CHAIN + OFF_CHAIN_STATE) == CHAIN_BROKEN;
}

// Read a printable C string with every byte bounds-checked: the label may live anywhere, and a hook
// that faults inside the battle loop takes the game with it.
static int read_cstr_safe(uint32_t p, char* out, int cap) {
    int i = 0;
    for (; i < cap - 1; i++) {
        if (IsBadReadPtr((void*)(uintptr_t)(p + i), 1)) break;
        char c = ((const char*)(uintptr_t)p)[i];
        if (!c) break;
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) break;
        out[i] = c;
    }
    out[i] = 0;
    return i;
}

// Battle.apiAiPushEnemyAfterExecArgs (0xBC3300): arguments come off the VM operand stack at
// [ctx+0x114], one 8-byte slot each with the value in the second dword, in REVERSE of the order the
// script writes them -- k0 int argc, k1 int argv, k2 str argc, k3 str argv, k4 enemy id, k5 kind.
// Filled BEFORE the ability name is returned, so the name door cannot reach it
// (ENEMY_AI_FLAGS_RE.md 2).
const uint8_t P_AIAFTER[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x4C};
#define VM_ARG_STACK 0x114
#define VM_ARG(a, k) ((a)[2 * (k) + 1])
#define AFTER_FLAG_SET 19     // AFTER_FLAG_0: raises the generic flag its int arg names
#define AI_FLAGS_MAX   15     // the enemy's flag array, as 0x55AD10 bounds it

void __cdecl on_ai_afterexec(uint32_t ctx) {
    if (!g_formFlag || !g_formFlagPool) return;
    if (!g_formAct[0] && !g_formWant[0]) return;             // the pin has stood down
    if ((int)(GetTickCount() - g_formUntil) >= 0) return;
    if (!ctx || IsBadReadPtr((void*)(uintptr_t)(ctx + VM_ARG_STACK), 4)) return;
    uint32_t sp = *(uint32_t*)(uintptr_t)(ctx + VM_ARG_STACK);
    if (!sp || IsBadReadPtr((void*)(uintptr_t)sp, 6 * 8)) return;
    const uint32_t* a = (const uint32_t*)(uintptr_t)sp;
    if (VM_ARG(a, 5) != AFTER_FLAG_SET || (int32_t)VM_ARG(a, 0) < 1) return;
    uint32_t argv = VM_ARG(a, 1);
    if (!argv || IsBadWritePtr((void*)(uintptr_t)argv, 4)) return;
    int32_t flag = *(int32_t*)(uintptr_t)argv;
    if (flag < 0 || flag > AI_FLAGS_MAX) return;
    if (!(g_formFlagPool & (1u << flag)) || flag == g_formFlag) return;
    // Someone else's bookkeeping is left alone; an id that will not resolve is corrected anyway,
    // since only the pinned fight is up and only its enemy keeps this.
    uint32_t* tbl = actor_table();
    int slot = actor_id_slot(VM_ARG(a, 4));
    char nm[SPEC_NAME_LEN + 1];
    if (tbl && slot >= 0 && slot < ACTOR_SLOTS && actor_name_of(tbl[slot], nm)
        && strncmp(nm, g_formBase, strlen(g_formBase))) return;
    *(int32_t*)(uintptr_t)argv = g_formFlag;
    ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- the enemy was about to spend mode flag %d, "
             "the one it rolled; spending %d instead, the one it is being made to take\n",
             g_formScene, g_formMode, (int)flag, (int)g_formFlag);
}

// The condition dispatcher's join (0x558031): the hooked instruction is the mov that loads the
// answer into eax, so writing [esp+0x2C98] here decides the condition. Frame layout and the rules
// that come with it: ENEMY_AI_FLAGS_RE.md 3.9.
const uint8_t P_AICOND[7] = {0x8B,0x84,0x24,0x98,0x2C,0x00,0x00};

// The AI state setter, 0x549D40(this, state); state 9 = the action went through.
#define OFF_ACTOR_ID 0x16C     // the chara's own actor id (0x4ED600 reads it as a signed word)
const uint8_t P_AISTATE[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x30};

// IsBadReadPtr costs a page probe, and this runs on every condition the battle AI evaluates:
// measured, it drops frames. Do not put one back.
static int prb_ptr_ok(uint32_t p) { return p >= 0x10000u && p < 0x7FFF0000u; }

// The one enemy the AI overrule below applies to. The condition kinds are generic -- every sheet
// spends them on something of its own -- so widening this rewrites AI nobody has looked at.
static const char g_probeSpec[] = "m097";
#ifdef FF13_DIAG
static char g_actLast[ACT_NAME_LEN + 1];   // the ability the AI last named, for the line above
#endif

// btsc06046's optional extra attack, forced often enough to rehearse. A share of CYCLES is
// overruled, never a share of rolls: the element change is what a FAILED roll produces, so a roll
// that cannot fail stops the element cycling altogether. Neither rate is the rate that comes out --
// a forced decision still dies to a hit before the gauge can pay -- so calibrate by measuring.
// Values, and what each one cost to find: CODE_NOTES / ENEMY_AI_FLAGS_RE.md 3.9.
#define EXTRA_FORCE_PCT 25
// Aim the grant at one of the two rolls: the sheet asks the 30% one first, so answering the first
// roll yes makes every forced extra the plain attack (measured, 10 of 10).
#define EXTRA_GRANT_ATTACK_PCT 60
#define EXTRA_PER_CYCLE_MAX 2
#define COND_KIND_30 7
#define COND_KIND_50 5
// A safety stop, not the design: forcing ends when the extra EXECUTES (on_ai_state). Capping by
// count instead never lets the gauge pay for one (measured, 0 extras in 4 fights).
#define EXTRA_FORCE_MAX 12
#define EXTRA_FORCE_SPEC "m097"

static volatile int g_exDecided = 0;    // this element cycle has had its one draw
static volatile int g_exForcing = 0;    // ...and the draw said yes
static volatile int g_exUsed    = 0;    // successes granted so far this cycle
static volatile int g_exGrant   = 0;    // ...and which roll it grants (COND_KIND_30 / _50)
static volatile int g_exDone    = 0;    // extras actually executed in this cycle

// Cleared on every element change (see on_ability_label) and at the start of a picked fight.
void extra_force_reset(void) { g_exDecided = 0; g_exForcing = 0; g_exUsed = 0; g_exDone = 0; }

static unsigned ex_rand(void);
// An extra went through. Forcing does NOT stop here: counter 4 comes round again inside the same
// element cycle, and stopping at the first is why second extras never appeared.
void extra_force_done(void) {
    if (!g_exForcing) return;
    // One execution reaches here more than once (the state setter repeats; the low-HP table commits
    // its pair back to back). Two extras half a second apart cannot be paid for, so it is one.
    static volatile DWORD s_lastDone = 0;
    DWORD now = GetTickCount();
    if (s_lastDone && (now - s_lastDone) < 500) return;
    s_lastDone = now;
    g_exUsed = 0;                                     // the next window gets its own grants
    ++g_exDone;
#ifdef FF13_DIAG
    ff13logv("[FF13-SRP] extra: one went through (%d this cycle); forcing %s\n",
             g_exDone, g_exDone >= EXTRA_PER_CYCLE_MAX ? "stops here" : "stays on for another");
#endif
    if (g_exDone >= EXTRA_PER_CYCLE_MAX) { g_exForcing = 0; return; }
    g_exGrant = ((ex_rand() % 100u) < (unsigned)EXTRA_GRANT_ATTACK_PCT)
              ? COND_KIND_30 : COND_KIND_50;          // so the pair can be one of each
}

static unsigned ex_rand(void) {         // xorshift: the game's own RNG is not ours to disturb
    static unsigned st = 0;
    if (!st) st = GetTickCount() | 1u;
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return st;
}

void __cdecl on_ai_cond(uint32_t esp) {
    if (!prb_ptr_ok(esp)) return;
    uint32_t* f = (uint32_t*)(uintptr_t)esp;
    uint32_t kind = f[0x2C80 / 4];
    if (kind != 5 && kind != 7) return;              // only the extra attack's two rolls
    uint32_t ctx   = f[0x2C84 / 4];
    uint32_t chara = f[0x2C88 / 4];
    if (!prb_ptr_ok(ctx) || !prb_ptr_ok(chara)) return;
    // By NAME, not by the pointer a lookup returned: after an element change the table can answer
    // with an object that is no longer the one acting.
    {
        char nm[SPEC_NAME_LEN + 1];
        if (!actor_name_of(chara, nm)) return;
        size_t pl = strlen(g_probeSpec);
        if (strncmp(nm, g_probeSpec, pl) || (nm[pl] && nm[pl] != '_')) return;
    }
    uint32_t w = *(uint32_t*)(uintptr_t)(ctx + 0x5B0);
    if ((w & 0x7Fu) != 4) return;                   // only the decision that carries the extra
    int arg = *(int32_t*)(uintptr_t)(ctx + 0x10);
    if (arg != 50) return;                          // ...and only its two rolls

    // Picked fights only: the story's own fight is not the mod's to change.
    if (g_bsStarted) {
        if (!g_exDecided) {
            g_exDecided = 1;
            g_exForcing = (ex_rand() % 100u) < (unsigned)EXTRA_FORCE_PCT;
            g_exGrant = ((ex_rand() % 100u) < (unsigned)EXTRA_GRANT_ATTACK_PCT)
                      ? COND_KIND_30 : COND_KIND_50;
        }
        if (g_exForcing && g_exUsed < EXTRA_FORCE_MAX) {
            // [esp+0x2C98] is the answer and the hooked instruction is the mov that loads it into
            // eax, so this lands before anything has read it.
            if ((int)kind == g_exGrant) {
                if (!f[0x2C98 / 4]) {
                    f[0x2C98 / 4] = 1; g_exUsed++;
#ifdef FF13_DIAG
                    ff13logv("[FF13-SRP] extra: granted the %s roll (%d already out this cycle)\n",
                             g_exGrant == COND_KIND_30 ? "attack" : "magic", g_exDone);
#endif
                }
            } else if ((int)kind == COND_KIND_30) {
                // The sheet only asks the 50% roll after the 30% one has failed, so the cycle's
                // other draw has to be made to miss.
                f[0x2C98 / 4] = 0;
            }
        }
    }
}

// The status roll's last fork before it draws (0x4A5597). The hooked instruction sits AFTER the
// chance has been worked out and BEFORE the draw, which is why it is this one and not the compare
// itself: nothing is on the x87 stack here, and the cave saves only the integer registers.
// Written as a bit pattern rather than -1.0f for the same reason.
const uint8_t P_STATROLL[7] = {0x83,0xBD,0x74,0xFF,0xFF,0xFF,0x00};
#define SR_EBP      0x1D4    // ebp - esp at the hooked instruction: the frame is fixed, no push
                             // is outstanding there and the function keeps a frame pointer
#define SR_TARGET   0x1CC    // ebp - : the chara the roll is against
#define SR_CHANCE   0x84     // ebp - : that chance as a float 0..1, read only by the compare
#define SR_PERCENT  0x88     // ebp - : and as an int; the roll branch bounds it to 1..99
#define SR_REGIST   0x8C     // ebp - : the target's resistance to this status, 0..99
#define SR_STATUS   0x54     // ebp - : which status this slot of the effect list carries
#define SR_MISS_BITS 0xBF800000u   // -1.0f: the compare is `chance < roll`, and a roll of exactly
                                   // 0 would let a chance of 0 through
#define SR_LAND_BITS 0x3F800000u   // 1.0f: the draw is 0..1, so a certain chance cannot lose it

// Reading and scaling the chance float without one FPU instruction: the cave saves no FPU state
// (see above). The reading is `(1.m) << (exp - 127)` in fixed point; the roll branch is only reached
// with a positive chance, so no sign or zero case is live.
static int chance_permille(uint32_t bits) {
    uint32_t e = (bits >> 23) & 0xFFu;
    if ((bits & 0x80000000u) || e == 0) return 0;
    uint64_t m = 0x800000u | (bits & 0x7FFFFFu);
    int sh = 150 - (int)e;                              // value = m >> sh
    if (sh > 43) return 0;                              // smaller than a thousandth of a percent
    if (sh < 0) return 99999;                           // past certain; the compare cannot lose
    uint64_t v = m * 1000u;
    if (sh > 0) v += 1ull << (sh - 1);                  // rounded: 0.45 reads 45.0%, not 44.9%
    return (int)(v >> sh);
}

// ...and pct/100 of it, in Q10 on the mantissa followed by a renormalise, which at pct <= 100 costs
// at most two shifts. Exact to float32 for 50 / 30 / 25 / 10, within a ulp elsewhere (measured).
static uint32_t chance_scaled(uint32_t bits, int pct) {
    uint32_t e = (bits >> 23) & 0xFFu;
    if ((bits & 0x80000000u) || e == 0) return SR_MISS_BITS;
    uint64_t v = (((uint64_t)(0x800000u | (bits & 0x7FFFFFu)) * (uint32_t)pct) << 10) / 100u;
    if (!v) return SR_MISS_BITS;
    int e2 = (int)e;
    while (v >= ((uint64_t)0x1000000u << 10)) { v >>= 1; e2++; }
    while (v <  ((uint64_t)0x800000u  << 10)) { v <<= 1; e2--; }
    uint32_t m2 = (uint32_t)((v + (1u << 9)) >> 10);
    if (m2 >= 0x1000000u) { m2 >>= 1; e2++; }
    // Too small to hold a normal exponent is a chance no rehearsal cares about keeping honest.
    if (e2 <= 0 || e2 >= 255) return SR_MISS_BITS;
    return ((uint32_t)e2 << 23) | (m2 & 0x7FFFFFu);
}

void __cdecl on_status_roll(uint32_t esp) {
    int miss = (g_dbfLeft != 0 && g_dbfOn[0]);
    // Not while a go is still being let past: the forced land would be spent on the go that was let
    // through rather than on the one the pin aims.
    int watch = (g_dbfLandStatus && g_formBase[0] && g_openSkip == 0);
    if ((!miss && !watch) || !prb_ptr_ok(esp)) return;
    if ((int)(GetTickCount() - g_dbfUntil) >= 0) { g_dbfLeft = 0; g_dbfLandLeft = 0; return; }
    uint32_t ebp = esp + SR_EBP;
    int pct = *(int32_t*)(uintptr_t)(ebp - SR_PERCENT);
    if (pct < 1 || pct > 99) return;                 // not the frame this hook was cut for
    int st = *(int32_t*)(uintptr_t)(ebp - SR_STATUS);
    int isMiss = (miss && st == g_dbfStatus);
    int isLand = (watch && st == g_dbfLandStatus);
    if (!isMiss && !isLand) return;
    uint32_t chara = *(uint32_t*)(uintptr_t)(ebp - SR_TARGET);
    if (!prb_ptr_ok(chara)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(chara, nm)) return;
    int reg = *(int32_t*)(uintptr_t)(ebp - SR_REGIST);
    if (isMiss && spec_is(nm, g_dbfOn) && g_dbfScale) {
        uint32_t* pc = (uint32_t*)(uintptr_t)(ebp - SR_CHANCE);
        int was = chance_permille(*pc);
        *pc = chance_scaled(*pc, g_dbfScale);
        int now = chance_permille(*pc);
        g_dbfMissN++;
        ff13logv("[FF13-SRP] miss pin: status %d on %s scaled to %d%% -- %d.%d%% -> %d.%d%% "
                 "(%d%% base, %d resist), %d rolls so far\n", g_dbfStatus, nm, g_dbfScale,
                 was / 10, was % 10, now / 10, now % 10, pct, reg, g_dbfMissN);
        return;
    }
    if (isMiss && spec_is(nm, g_dbfOn)) {
        *(uint32_t*)(uintptr_t)(ebp - SR_CHANCE) = SR_MISS_BITS;
        if (g_dbfLeft > 0) --g_dbfLeft;
        g_dbfMissN++;
        if (g_dbfLeft < 0)
            ff13logv("[FF13-SRP] miss pin: status %d on %s made to fail (%d%% chance, %d resist) "
                     "-- %d so far, and they keep failing until \"%s\"\n",
                     g_dbfStatus, nm, pct, reg, g_dbfMissN, g_dbfUntilAct);
        else
            ff13logv("[FF13-SRP] miss pin: status %d on %s made to fail (%d%% chance, %d resist) -- "
                     "%d still owed\n", g_dbfStatus, nm, pct, reg, g_dbfLeft);
        return;
    }
    // The rider being landed is the pinned ENEMY's, so a roll of the same status against the enemy
    // itself is the party's own doing and is left alone.
    if (isLand && !spec_is(nm, g_formBase)) {
        int force = (g_dbfLandLeft > 0 && (!g_openAimOn[0] || spec_is(nm, g_openAimOn)));
        if (force) {
            *(uint32_t*)(uintptr_t)(ebp - SR_CHANCE) = SR_LAND_BITS;
            --g_dbfLandLeft;
        }
        if (g_dbfLandSeen < LAND_LOG_MAX) {
            g_dbfLandSeen++;
            ff13logv("[FF13-SRP] opening pin: status %d rolled against %s (%d%% chance, %d resist)"
                     "%s\n", g_dbfLandStatus, nm, pct, reg,
                     force ? " -- MADE TO LAND" : " -- left to the dice");
        }
    }
}

// The store the roll's compare falls through to when the dice say yes (0x4A55D1): the only "it
// landed" reachable from a roll. Same frame and the same esp as the roll hook -- the rand call
// between them is balanced and nothing else moves esp. The x87 stack is EMPTY here (fcomip and fstp
// popped both), which is what this cave needs: it saves the integer registers and nothing else.
const uint8_t P_STATLAND[7] = {0xC7,0x45,0xC4,0x01,0x00,0x00,0x00};

void __cdecl on_status_land(uint32_t esp) {
    if (!g_dbfScale || !g_dbfOn[0] || !prb_ptr_ok(esp)) return;
    uint32_t ebp = esp + SR_EBP;
    int pct = *(int32_t*)(uintptr_t)(ebp - SR_PERCENT);
    if (pct < 1 || pct > 99) return;                 // not the frame this hook was cut for
    uint32_t chara = *(uint32_t*)(uintptr_t)(ebp - SR_TARGET);
    if (!prb_ptr_ok(chara)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(chara, nm) || !spec_is(nm, g_dbfOn)) return;
    int st = *(int32_t*)(uintptr_t)(ebp - SR_STATUS);
    int p  = chance_permille(*(uint32_t*)(uintptr_t)(ebp - SR_CHANCE));
    int reg = *(int32_t*)(uintptr_t)(ebp - SR_REGIST);
    // Every status that lands on the pinned enemy is logged, not only the pinned one: a debuff with
    // no line here came from somewhere this hook cannot see.
    if (st == g_dbfStatus) g_dbfLandN++;
    if (g_dbfLandN + g_dbfMissN <= DBF_LAND_LOG_MAX * 2)
        ff13logv("[FF13-SRP] miss pin: LANDED status %d on %s at %d.%d%% (%d%% base, %d resist)%s\n",
                 st, nm, p / 10, p % 10, pct, reg,
                 st == g_dbfStatus ? "" : " -- not the pinned status");
}

// The hit reaction's resist fork (0x4A7E79): BattleChara::hitDamageSource has settled which reaction
// the hit carries and how much the target resists it, and has not drawn yet. esp-relative throughout
// -- the function realigns esp on entry, so ebp is not a fixed distance away -- and no push is
// outstanding at this instruction.
const uint8_t P_LAUNCH[8] = {0x83,0xBC,0x24,0x2C,0x03,0x00,0x00,0x00};
#define LAU_REGIST 0x32C   // esp + : how much the target resists this reaction, 0..100
#define LAU_KIND   0x340   // esp + : which reaction, 4 = the launch (3 is the slam, same fork)
#define LAU_TARGET 0x348   // esp + : the chara being hit
#define LAU_BLAST  4       // HIT_REACTION_KIND_BLAST, what Launch carries once the target is broken

void __cdecl on_launch_roll(uint32_t esp) {
    if (!g_lauScale || !g_lauOn[0] || !prb_ptr_ok(esp)) return;
    if ((int)(GetTickCount() - g_lauUntil) >= 0) { g_lauScale = 0; return; }
    if (*(int32_t*)(uintptr_t)(esp + LAU_KIND) != LAU_BLAST) return;
    int* pr = (int*)(uintptr_t)(esp + LAU_REGIST);
    int was = *pr;
    if (was < 0 || was > 100) return;                // not the frame this hook was cut for
    uint32_t chara = *(uint32_t*)(uintptr_t)(esp + LAU_TARGET);
    if (!prb_ptr_ok(chara)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(chara, nm) || !spec_is(nm, g_lauOn)) return;
    // Scaled on the SUCCESS side, so the percent means what it does on the miss pin: 50 = half as
    // likely to go up, whatever the fight's own resistance happens to be.
    int up = ((100 - was) * g_lauScale + 50) / 100;
    *pr = 100 - up;
    g_lauRollN++;
    if (g_lauRollN <= LAU_LOG_MAX)
        ff13logv("[FF13-SRP] launch pin: %s scaled to %d%% -- %d%% -> %d%% chance of going up, "
                 "%d rolls so far\n", nm, g_lauScale, 100 - was, up, g_lauRollN);
}

// The store that downgrades the reaction to plain damage (0x4A7ECD), reached only when the roll says
// the target keeps its feet. The kind it overwrites is still readable: the cave runs before it.
const uint8_t P_LAUNCHRES[11] = {0xC7,0x84,0x24,0x40,0x03,0x00,0x00,0x01,0x00,0x00,0x00};

void __cdecl on_launch_resist(uint32_t esp) {
    if (!g_lauScale || !g_lauOn[0] || !prb_ptr_ok(esp)) return;
    if (*(int32_t*)(uintptr_t)(esp + LAU_KIND) != LAU_BLAST) return;
    uint32_t chara = *(uint32_t*)(uintptr_t)(esp + LAU_TARGET);
    if (!prb_ptr_ok(chara)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(chara, nm) || !spec_is(nm, g_lauOn)) return;
    g_lauDownN++;
    if (g_lauDownN <= LAU_LOG_MAX)
        ff13logv("[FF13-SRP] launch pin: %s stayed down (%d resist), %d of %d rolls so far\n",
                 nm, *(int32_t*)(uintptr_t)(esp + LAU_REGIST), g_lauDownN, g_lauRollN);
}

// `this` is the AI block and its first field is the actor id. Resolve the chara THROUGH that id,
// never by name: this enemy renames itself on every element change and its PARTS keep the old name
// (ENEMY_AI_FLAGS_RE.md 3.10).
void __cdecl on_ai_state(uint32_t ecx, uint32_t state, uint32_t* pRet, uint32_t* pUp) {
    (void)pRet; (void)pUp;
    if (state != 9 || !prb_ptr_ok(ecx)) return;
    uint32_t* tbl = actor_table();
    if (!tbl) return;
    int slot = actor_id_slot(*(int32_t*)(uintptr_t)ecx);
    if (slot < 0 || slot >= ACTOR_SLOTS) return;
    uint32_t self = tbl[slot];
    if (!prb_ptr_ok(self)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(self, nm)) return;
    // An opening pin lives and dies here. A GO is the enemy choosing among its attacks, ended by
    // the next change of its AI state; landing is deliberately NOT required, and one go is the
    // whole of it -- the sheet re-rolls its odds every decision, so a pin that outlived its go
    // would override the next roll too. NOT the after-exec door: those args are pushed before the
    // name is returned.
    if (g_openPin && spec_is(nm, g_formBase)) {
        int cnt = -1;
        if (!IsBadReadPtr((void*)(uintptr_t)(self + OFF_AI_ACTCNT), 4))
            cnt = (int)(*(uint32_t*)(uintptr_t)(self + OFF_AI_ACTCNT) & 0x7Fu);
        if (!g_openDecided) {
            // The fight's opening state change, or one of the setter's repeats. Neither is a go.
            if (g_openIgn++ < 4)
                ff13logv("[FF13-SRP] opening pin: btsc%05u mode %d -- a state change from %s with "
                         "no decision behind it (counter %d); passing over it\n",
                         g_formScene, g_formMode, g_formBase, cnt);
        } else {
            g_openDecided = 0;
            if (g_openSkip > 0) {
                if (--g_openSkip == 0)
                    ff13logv("[FF13-SRP] opening pin: btsc%05u mode %d -- %s has had a go "
                             "(counter %d); the next decision is \"%s\"\n",
                             g_formScene, g_formMode, g_formBase, cnt, g_formAct);
            } else {
                ff13logv("[FF13-SRP] opening pin: btsc%05u mode %d -- the decision for \"%s\" is "
                         "over after %d rewrite(s) (counter %d)%s; %s's attacks are its own again\n",
                         g_formScene, g_formMode, g_formAct, g_openBent, cnt,
                         (g_openWho && !g_openAimed)
                             ? ", and it drew no target here to aim -- nothing forced this go" : "",
                         g_formBase);
                g_formAct[0] = 0; g_openPin = 0;
                // The volley of THIS go is the one to fix -- but only if it is going where the
                // pattern says. An unaimed go would spend the forced land on whoever it hit.
                if (!g_openWho || g_openAimed) g_dbfLandLeft = g_dbfLandWant;
            }
        }
    }
    // An ability-ended miss pin needs BOTH doors: the label door names what the AI settled on, and
    // this is where that decision goes through. The AI is asked again and again while the ATB
    // fills, so a name on its own is an answer, not an action.
    if (g_dbfLeft && g_dbfUntilAct[0] && g_formBase[0] && spec_is(nm, g_formBase)) {
        if (g_dbfUntilSeen) {
            ff13logv("[FF13-SRP] miss pin: btsc%05u mode %d -- \"%s\" is out after %d forced "
                     "miss(es); status %d is the dice's own again\n",
                     g_formScene, g_formMode, g_dbfUntilAct, g_dbfMissN, g_dbfStatus);
            g_dbfLeft = 0; g_dbfUntilAct[0] = 0;
        }
        g_dbfUntilSeen = 0;      // named since the LAST go, or it says nothing about this one
    }
    size_t pl = strlen(g_probeSpec);
    if (strncmp(nm, g_probeSpec, pl) || (nm[pl] && nm[pl] != '_')) return;
    uint32_t w = *(uint32_t*)(uintptr_t)(self + OFF_AI_ACTCNT);
#ifdef FF13_DIAG
    // One line per action: ability, action counter, generic flag array (chara+8). The flags are
    // the open question -- ENEMY_AI_FLAGS_RE.md 3.8 has been wrong about them twice.
    ff13logv("[FF13-SRP] m097 acted: %-12s cnt=%u flags=%08X act=%d\n",
             g_actLast[0] ? g_actLast : "?", w & 0x7Fu,
             *(uint32_t*)(uintptr_t)(self + 8), *(signed char*)(uintptr_t)(self + 0x265));
#endif
    // Do NOT test the counter alone: AFTER_COUNTER_SET_0 has often already put it back to 0 by the
    // time the state setter runs (half the executions, measured), and the missed half let a forced
    // cycle run past its cap. An outstanding grant identifies that half -- while one is pending the
    // enemy is holding the counter-4 decision and can be committing nothing else.
    unsigned cnt = w & 0x7Fu;
    if (cnt != 4 && !(cnt == 0 && g_exUsed > 0)) return;
    if (*(signed char*)(uintptr_t)(self + 0x265) == 19) return;   // broken: a free attack, not this
    extra_force_done();
}

void __cdecl on_ability_label(uint32_t ecx, uint32_t label) {
    (void)ecx;
    if (!label) return;
    char cur[ACT_NAME_LEN + 1];
    if (!read_cstr_safe(label, cur, (int)sizeof cur)) return;
    // An element change ends the cycle, so the forced-extra draw starts again from the next one.
#ifdef FF13_DIAG
    if (!strncmp(cur, EXTRA_FORCE_SPEC "_", sizeof(EXTRA_FORCE_SPEC "_") - 1)) {
        strncpy(g_actLast, cur, ACT_NAME_LEN); g_actLast[ACT_NAME_LEN] = 0;
    }
#endif
    if (!strncmp(cur, EXTRA_FORCE_SPEC "_ac5", sizeof(EXTRA_FORCE_SPEC "_ac5") - 1)) {
#ifdef FF13_DIAG
        if (g_exDecided)
            ff13logv("[FF13-SRP] extra: element change -- the cycle ends with %d\n", g_exDone);
#endif
        extra_force_reset();
    }
    if (!g_formBase[0]) return;
    size_t n = strlen(g_formBase);
    if (strncmp(cur, g_formBase, n) || cur[n] != '_') return;      // some other actor's ability
    // ...and for an ability-ended miss pin, whether that answer is the one it waits for. Set AND
    // cleared here: the AI's latest answer is the only thing this door can honestly report.
    if (g_dbfLeft && g_dbfUntilAct[0]) {
        int isEnd = 0;
        for (int i = 0; i < DBF_MISS_N && !isEnd; i++) {
            if (kDbfMiss[i].btsc != g_formScene) continue;
            for (int u = 0; u < DBF_UNTIL_MAX && kDbfMiss[i].until[u]; u++)
                if (!strcmp(cur, kDbfMiss[i].until[u])) { isEnd = 1; break; }
        }
        g_dbfUntilSeen = isEnd;
    }
#ifdef FF13_DIAG
    // Only the mode-change ones: logging every ability this enemy reaches for buries them.
    if (strstr(cur, "_ac5"))
        ff13logv("[FF13-SRP] form pin: ability \"%s\"\n", cur);
#endif
    if (!g_formAct[0]) return;
    if ((int)(GetTickCount() - g_formUntil) >= 0) { g_formAct[0] = 0; return; }   // window lapsed
    // An opening pin ends at the enemy's action (on_ai_state); the break here is only a backstop
    // for an enemy whose after-exec door never opens.
    if (g_openPin) {
        // Throttled: open_broken costs a page probe per actor slot and this door is reached far
        // more often than the enemy acts; a break noticed 200ms late costs nothing.
        static DWORD lastBrk = 0;
        DWORD nowB = GetTickCount();
        if (nowB - lastBrk >= 200) {
            lastBrk = nowB;
            if (open_broken()) {
                ff13logv("[FF13-SRP] opening pin: btsc%05u mode %d -- %s has broken after %d "
                         "rewrite(s); its attacks are its own again\n",
                         g_formScene, g_formMode, g_formBase, g_openBent);
                g_formAct[0] = 0; g_openPin = 0; g_dbfLandLeft = 0;
                return;
            }
        }
    } else if (!g_formSwapEnds && g_formActBent && (int)(GetTickCount() - g_formActBent) >= 0) {
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- done; the abilities are the game's own "
                 "again\n", g_formScene, g_formMode);
        g_formAct[0] = 0; g_formActBent = 0;
        return;
    }
    // The abilities this pin may rewrite, and -- for an opening pin -- a second name the mode's
    // own attack answers to, which is left standing rather than bent into the first.
    const char* alt = NULL;
    int isCand = 0;
    for (int i = 0; i < AI_FORMS_N && !isCand; i++) {
        if (kAiForms[i].btsc != g_formScene) continue;
        for (int m = 0; m < FORMS_MAX; m++)
            if (kAiForms[i].acts[m] && !strcmp(cur, kAiForms[i].acts[m])) { isCand = 1; break; }
    }
    for (int i = 0; i < AI_OPEN_N && !isCand && g_openPin; i++) {
        if (kAiOpen[i].btsc != g_formScene) continue;
        for (int c = 0; c < OPEN_CAND_MAX; c++)
            if (kAiOpen[i].cand[c] && !strcmp(cur, kAiOpen[i].cand[c])) { isCand = 1; break; }
        if (isCand && g_formMode >= 1 && g_formMode <= OPEN_MAX) alt = kAiOpen[i].alt[g_formMode - 1];
    }
    if (!isCand) return;
    g_openDecided = 1;                   // the enemy choosing among its attacks IS the go
    if (g_openSkip > 0) return;          // this pattern is about a later go than this one
    char want[ACT_NAME_LEN + 1];
    strncpy(want, g_formAct, ACT_NAME_LEN);
    want[ACT_NAME_LEN] = 0;
    if (!strcmp(cur, want) || (alt && !strcmp(cur, alt))) {   // the game reached for it by itself
        // The clock still starts, or a decision that needed no bend leaves the pin armed and the
        // next one gets bent instead.
        if (!g_formActBent) g_formActBent = GetTickCount() + PIN_ACT_MS;
        aim_hold_arm();
        return;
    }
    // EVERY mode-change ability of this enemy is bent, not just the first: this door is the AI's
    // CANDIDATE LOOKUP rather than its decision (hardware showed one candidate redirected and two
    // more asked about afterwards), so whichever it settles on must already read as the pinned one.
    // The label may be read-only (an AI script constant pool), in which case this door cannot be
    // used at all and the charaspec one is still armed behind it.
    size_t len = strlen(cur);
    if (strlen(want) > len || IsBadWritePtr((void*)(uintptr_t)label, (UINT)(len + 1))) {
        ff13loga("[FF13-SRP] form pin: ability \"%s\" cannot be redirected here (label is %s) -- "
                 "leaving it to the charaspec door\n", cur,
                 strlen(want) > len ? "shorter than the replacement" : "not writable");
        g_formAct[0] = 0;
        return;
    }
    memcpy((void*)(uintptr_t)label, want, strlen(want) + 1);
    if (!g_formActBent) g_formActBent = GetTickCount() + PIN_ACT_MS;   // the clock starts here
    aim_hold_arm();
#ifdef FF13_DIAG
    // The target as it stands right now is the one this ability will be given: T_COND_0 reads the
    // field a moment later and hands back whatever is in it. Sampling it here is what ties a part
    // to the buff that landed on it.
    {
        int slot = -1;
        uint32_t boss = actor_by_prefix(g_formBase, &slot);
        if (boss && !IsBadReadPtr((void*)(uintptr_t)(boss + OFF_AI_CONDTARGET), 4))
            ff13logv("[FF13-SRP] parts: \"%s\" is picked with its conditional target on slot %d\n",
                     want, *(int32_t*)(uintptr_t)(boss + OFF_AI_CONDTARGET));
    }
#endif
    if (g_openPin) {
        if (++g_openBent <= OPEN_LOG_MAX)
            ff13logv("[FF13-SRP] opening pin: \"%s\" now reads \"%s\" (%d)\n", cur, want, g_openBent);
    } else if (!g_formActSaid) {
        g_formActSaid = 1;
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- every mode-change ability of \"%s\" now "
                 "reads \"%s\" (first was \"%s\")\n", g_formScene, g_formMode, g_formBase, want, cur);
    }
    // The charaspec door stays armed on a swap-ended pin: the bend above is only a candidate
    // rewrite, and the swap is the one moment that proves the change really happened.
    if (!g_formSwapEnds) g_formWant[0] = 0;
}

// At the entry of the charaspec swap: ecx = the actor, [esp+4] = the 16-byte name it is about to
// install. Writing that buffer here redirects the change; popad restores registers only, so a
// write into the caller's frame survives.
void __cdecl on_spec_replace(uint32_t ecx, uint32_t name) {
    if (!name || IsBadWritePtr((void*)(uintptr_t)name, SPEC_NAME_LEN)) return;
    char* dst = (char*)(uintptr_t)name;
    char cur[SPEC_NAME_LEN + 1];
    memcpy(cur, dst, SPEC_NAME_LEN);
    cur[SPEC_NAME_LEN] = 0;
    for (int i = 0; cur[i]; i++)
        if ((unsigned char)cur[i] < 0x20 || (unsigned char)cur[i] > 0x7E) { cur[i] = 0; break; }
    if (g_swapLogN < 8) {
        g_swapLogN++;
        ff13logv("[FF13-SRP] battle: charaspec swap -> \"%s\" on actor %08X (object %08X)%s\n", cur, ecx,
                 (ecx && !IsBadReadPtr((void*)(uintptr_t)(ecx + OFF_ACTOR_OBJ), 4))
                     ? *(uint32_t*)(uintptr_t)(ecx + OFF_ACTOR_OBJ) : 0xFFFFFFFFu,
                 g_bsStarted ? "  (picked fight)" : "");
    }
    // Every fight, not only a picked one: a walked-into fight leaves its objects behind just the
    // same, and a pick right after it binds to them.
    obj_form_note(ecx, cur);
    if (!g_formBase[0]) return;
    // A form OF the pinned enemy: "m097_x", never plain "m097" -- the fight installs the base spec
    // itself, and treating that as the change would spend the pin before the fight starts.
    size_t n = strlen(g_formBase);
    int isForm = !strncmp(cur, g_formBase, n) && cur[n] == '_';
    // The swap IS the change happening, so this is where the ability door stands down: every change
    // after the first is meant to be as random as it ever was.
    if (isForm && g_formAct[0]) {
        g_formAct[0] = 0;
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- the change has happened (\"%s\"); every "
                 "change after this one is the game's own\n", g_formScene, g_formMode, cur);
    }
    if (!g_formWant[0]) return;
    if ((int)(GetTickCount() - g_formUntil) >= 0) { g_formWant[0] = 0; return; }   // window lapsed
    if (!isForm) return;
    // Written even when the game rolled the wanted form itself -- same 16 bytes either way, and it
    // means the write path is exercised on every pinned fight.
    char want[SPEC_NAME_LEN + 1];
    strncpy(want, g_formWant, SPEC_NAME_LEN);
    want[SPEC_NAME_LEN] = 0;
    int same = !strcmp(cur, want);
    memset(dst, 0, SPEC_NAME_LEN);
    memcpy(dst, want, strlen(want));
    if (same)
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- the game rolled \"%s\" itself\n",
                 g_formScene, g_formMode, want);
    else
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- \"%s\" redirected to \"%s\"; every "
                 "change after this one is the game's own\n", g_formScene, g_formMode, cur, want);
#ifdef FF13_DIAG
    // Read the buffer back: this separates "the write did not land" from "the write landed on
    // something the element does not come from".
    char back[SPEC_NAME_LEN + 1];
    memcpy(back, dst, SPEC_NAME_LEN);
    back[SPEC_NAME_LEN] = 0;
    ff13logv("[FF13-SRP] form pin: the argument now reads \"%s\"\n", back);
    g_formSeen[0] = 0;                              // make the next name the watch reads a change
    g_formWatch = GetTickCount() + 60000;
#endif
    g_formWant[0] = 0;                              // one change only
}

// ---- putting the party back where the fight was picked ---------------------------------------
// A picked fight ends at the checkpoint, which is wherever the game decided -- not where you were
// standing when you opened the picker. So the spot is taken when the fight is picked and the party
// warped back once the field is really there (warp_to, camera carry and recentre included).
// NOT in every zone. The Archylte Steppe is one continuous field streamed in cells as you walk and
// the way back crosses most of it: the party arrives and the game locks up on ground that has not
// streamed. Until the destination's cells can be asked for up front (AnywhereSave appends them to
// reconcile's desired[] at 0x42b9a0), the honest answer there is not to move the party at all.
#define HOME_SETTLE_MS 300      // after FIELD_IDLE returns, before the party is moved (was 900;
                                // the destination's cells are pre-armed now -- raise this first
                                // if the way back ever wedges again)
volatile int      g_homeHave = 0;    // a spot was taken for this fight
volatile int      g_homeDue  = 0;    // 1 = waiting for the field, 2 = the worker may warp
static volatile int g_homeNoWarp = 0;  // ...unless this fight's row asked to be left where it ends
static volatile DWORD    g_homeAt   = 0;
static volatile uint32_t g_homeZone = 0;
float g_homeX = 0, g_homeY = 0, g_homeZ = 0;
int g_homeCells[HOME_CELLS_MAX][2];      // the ground under the pick spot (loc,map pairs)
int g_homeCellsN = 0;
static char g_homeBg[40];                // the mapset the game had up at the pick (bg_heal_watch)
static volatile int g_bgHealLast = -1;
static char g_homeBgm[BGM_NAME_MAX + 1];  // ...and the field's music (bgm_heal_watch)
// ...and the chain's restart flag, which the RELOAD raises behind our back (rpflag_watch).
static const char* g_rpFlag = NULL;
static volatile int g_rpFlagLast = -1;
static volatile int   g_bgmHealLast = -1;
static volatile DWORD g_bgmHealAt   = 0;
// Zone 23, the Archylte Steppe: streamed in cells, so the way back can arrive on missing ground.
#define HOME_SKIP_ZONE 23u

static int home_zone_skipped(uint32_t zone) { return zone == HOME_SKIP_ZONE; }

static signed char  g_keepDecks[8][3];   // the player's own decks, captured at the pick
static int          g_keepParty[3];      // ...and who was fighting when they were captured
static int          g_keepN = 0;
static int          g_keepSel = 0;       // which deck was in hand at the pick (0 for table decks)
static volatile int g_keepGuard = 0;     // 1 = apply g_decksWant only if the party is unchanged
// The stat this fight is meant to be fought with (scene_stat), and what the actor had before.
static const ff13_scene_stat* g_statWant = NULL;
static volatile uint32_t g_statFor  = 0;   // the picked fight it is armed for, 0 = nothing armed
static volatile int      g_statBase = -1;  // the actor's own figure, once one has been seen
static volatile int      g_statAgain = 0;  // how many times something wrote over it
static volatile uint32_t g_statActor = 0;  // ...and which actor, so the fight's end can undo it
static volatile int g_statDone = 0;        // the raise has already been taken back out
static void tp_sample(void);        // reads the party's TP; defined with the refund work below
static void libra_sample(void);     // ...and the Datalog's Libra arrays, likewise
static void libra_keep_take(uint32_t btsc);  // ...and holds what it scanned across the way back
static void libra_zero_take(uint32_t btsc);  // ...and unscans the few fights the story meets cold
#ifdef FF13_DIAG
static void libra_diag_before(uint32_t btsc); // which species a fight writes, measured either way
static void libra_diag_point(const char* when, int last);
#endif
static volatile int g_libraHave;    // a snapshot is held and has not been put back yet

static void bs_start_selected(void) {
    if (g_bsCount <= 0 || g_bsSel < 0 || g_bsSel >= g_bsCount) return;
    if (field_zone_state() != 3) {                       // FIELD_IDLE only
        ff13logv("[FF13-SRP] battle picker: not in the field (zone_state=%d) -> ignored\n",
                 field_zone_state());
        flash_say(ui_text(T_PICK_FIELD_ONLY));
        return;
    }
    if (riding_chocobo()) {
        ff13logv("[FF13-SRP] battle picker: on a chocobo -> nothing started (dismount first)\n");
        flash_say(ui_text(T_PICK_CHOCOBO));
        return;
    }
    uint32_t mgr = field_mgr();
    if (!mgr || IsBadWritePtr((void*)(uintptr_t)(mgr + OFF_TOUCHED_ID), 4)) {
        ff13loga("[FF13-SRP] battle picker: no field manager -> not on the field?\n");
        flash_say(ui_text(T_PICK_FIELD_ONLY));
        return;
    }
    uint32_t touch = 0;
    if (!pick_touch_id(mgr, &touch)) {
        ff13loga("[FF13-SRP] battle picker: no live field object to touch\n");
        flash_say(ui_text(T_PICK_NO_OBJECT));
        return;
    }
    uint32_t want = g_bsList[g_bsSel];
#ifdef FF13_DIAG
    trk_audit("pick");
#endif
    btres_clear_for_pick(want);
    // Everything the staging asks for is a call the game itself would only make on a settled field,
    // so how long the field has been up belongs in a report.
    ff13logv("[FF13-SRP] battle picker: the loading indicator went down %lums ago\n",
             g_loadDownAt ? (unsigned long)(GetTickCount() - g_loadDownAt) : 0UL);
    evquiet_lift();                    // a fresh pick never inherits an earlier pick's quiet bit
    zb_remember_sel(field_zone_id(), want, g_bsVar[g_bsSel]);
    // Which form of this fight was picked, if it offers a choice. Armed before anything else so it
    // is already waiting when the enemy is built, however long that takes.
    aipin_arm(want, g_bsVar[g_bsSel]);
    // The chain's restart flag (rpflag_watch): taken DOWN, never restored -- one left up by a
    // story game over would open the menu on every pick. An evEvFlag entry keeps its own.
    {
        const ff13_arena* ra = scene_arena(want);
        g_rpFlag = (ra && ra->evRpFlag) ? ra->evRpFlag : NULL;
        g_rpFlagLast = -1;
        if (g_rpFlag && !(ra->evEvFlag && !strcmp(ra->evEvFlag, g_rpFlag))) {
            if (vm_get_event_flag(g_rpFlag)) {
                vm_set_event_flag(g_rpFlag, 0);
                ff13logv("[FF13-SRP] battle picker: \"%s\" (the chain's restart flag) was up at the "
                         "pick -- taken down, so this fight opens no preparation menu\n", g_rpFlag);
            }
        } else {
            g_rpFlag = NULL;                        // the entry owns this flag itself
        }
    }
    g_swapBtscId  = want;
    g_swapEnabled = 1;
    g_bsOpen = 0;
    const ff13_arena* ar = scene_arena(want);
    // ctl_lock() goes on BELOW, after the party has been set: the lock reads the leader at the
    // moment it is applied, so a fight that changes the party would leave it on somebody who is no
    // longer in it, and the party you were given could be walked around under the cover.
    // Music: only note what we want here. It is asked for once the game has started its own battle
    // theme (see on_bgm_play -> bs_pump); asking sooner collides with that request and both are lost.
    {
        const char* origin = "";
        const char* track;
        if (ar && ar->ev[0] && !ar->evMusic) {
            // Started by the story's own flow, which plays the story's own music. Asking for a
            // track here would ask over the top of it, and the end-of-fight sampler would then
            // record whatever won as this fight's own.
            track = NULL;
            ff13logv("[FF13-SRP] battle picker: btsc%05u is started by its event -- its music is "
                     "the story's too\n", want);
            // ...but the field's own track is stopped by hand, here, before the cutscene: only an
            // ordinary encounter has the game do it, and a chain entered mid-way never runs the
            // step that would. bgm_heal_watch puts it back on the way home.
            {
                char cur[BGM_NAME_MAX + 1];
                if (bgm_current(cur) && cur[0]) {
                    bgm_stop(cur, 0.0f);
                    ff13logv("[FF13-SRP] battle picker: stopped the field's \"%.16s\" before the "
                             "chain's cutscene\n", cur);
                }
            }
        } else if (ar && ar->bgm && !ar->bgm[0]) {
            // The entry says the fight's own music is already right (btsc01016 plays the zone's
            // ordinary battle theme; the offline match hands it its cutscene's boss track).
            track = NULL;
            ff13logv("[FF13-SRP] battle picker: btsc%05u keeps its own music\n", want);
        } else if (ar && ar->bgm) {
            track = ar->bgm; origin = " (named by the arena entry)";
        } else {
            track = bgm_pick_track(want, &origin);
        }
        g_bgmWant[0] = 0; g_bgmDue = 0; g_bgmHoldUntil = 0; g_bgmSkipHold = 0;
        if (track) {
            strncpy(g_bgmWant, track, BGM_NAME_MAX);
            g_bgmWant[BGM_NAME_MAX] = 0;
            g_bgmWantUntil = GetTickCount() + 30000;
            g_bgmArmedAt   = GetTickCount();
            g_btlScene     = 0;      // nothing from an earlier fight counts towards this one
            // Loaded into "general" NOW, the way the game fronts its own playBgm calls: lazyPlay
            // ACCEPTS an unloaded track and reports it playing, with no audio behind it.
            sound_make_room(g_bgmWant);      // a loaded track holds a stream; five is all there is
            vm_load_sound(g_bgmWant, "general");
            ff13logv("[FF13-SRP] battle picker: BGM \"%.16s\"%s -- loading it ahead\n",
                     g_bgmWant, origin);
        }
    }
    // The party this fight is meant to be fought with, before anything else is asked for. The table
    // is the authority, event door or not: some chains set their own party up and some make the
    // change a whole fight earlier, so the rule is per scene.
    g_partyWho = scene_party_who(want, (int*)&g_partyWhoN);
    g_partyDoor = (ar != NULL);
    party_script_run("picked");
    // A party the MOD set gets the decks the game generates for it; the story's own change ends in
    // restoreOptimaDeckAll and this one cannot. Only where no table row writes the decks anyway.
    g_decksRestoreFor = (g_partyWho && g_partyWhoN > 0 && !scene_decks(want, &(int){0})) ? want : 0;
    // An arena fight stages under the blackout cover, so root the party the way the story's own
    // staging does (ucOff) and shut the field menu -- otherwise the party can be walked off the mark
    // in the dark, or the pause menu opened over a party that is being rebuilt. Every picked fight,
    // not only arena ones; every way the start can end -- battle up, deadline, give-up, back on the
    // field -- lifts both.
    ctl_lock();
    g_evNoFightAt = 0; g_evSkipDoor = 0;   // a fresh pick always goes through the door first
    g_bsStarted = want; g_bsStartedAt = GetTickCount(); g_bsInFight = 0; g_bsSawEnemy = 0;
    g_openLogN = 0; bank_log_reset();
    g_bsTouchFull = 0; g_bsProbeLogged = 0; g_bsProbeMoved = 0;
    // Field-door picks only: an arena fight is started by the script and builds no encounter from
    // the touch, so the probe would call every one of them wedged (measured).
    g_touchAt = ar ? 0 : GetTickCount();
    g_touchSeq = g_encIdSeq;   // see touch_stall_probe
    // "They saw you coming", if asked for, is held over the whole build rather than written once,
    // because the encounter info is not finished being built when the door returns. Armed here and
    // not with the arena start: the picker's ordinary door is a field touch and never reaches that.
    // g_bsArena also routes the END of the fight through the checkpoint. An event fight otherwise
    // hands the run to whatever the story does next, and when the picker started it there is no
    // story running to take it -- so a fight can need that routing without needing an arena.
    g_bsArena = scene_win_at_checkpoint(want) ? 1 : 0;
    // Held here rather than in the door, because a fight does not need a door -- or an arena entry
    // at all -- to need its paradigms fixed. They go in once the battle is up: see decks_apply.
    // (g_keep*: the PLAYER's decks captured at the pick, guarded on the party staying the same.)
    {
        int dn = 0;
        const signed char (*dk)[3] = scene_decks(want, &dn);
        g_decksWant = dk; g_decksWantN = dn; g_decksAgainAt = 0;
        g_decksWantFor = dn ? want : 0;
        g_keepGuard = 0; g_keepSel = 0;
        if (dn)
            ff13logv("[FF13-SRP] battle picker: %d paradigm decks are held for btsc%05u -- they go "
                     "in once the fight is up, which is after the game has finished generating its "
                     "own\n", dn, want);
        else {
            // A chain that sets its own party wipes the paradigms with it. Captured here and put
            // back once the fight is up, only if the chain fielded the SAME three (see pump_decks).
            const ff13_arena* ka = scene_arena(want);
            if (ka && ka->evEndRp) {
                g_keepN = decks_read_live(g_keepDecks, 8);
                g_keepSel = deck_sel_live();
                if (g_keepN > 0 && party_battle_ids(g_keepParty)) {
                    g_decksWant = (const signed char (*)[3])g_keepDecks;
                    g_decksWantN = g_keepN; g_decksWantFor = want;
                    g_keepGuard = 1;
                    ff13logv("[FF13-SRP] battle picker: the player's %d deck(s) (deck %d in "
                             "hand) are held for btsc%05u -- they go back in if the chain fields "
                             "the same three\n", g_keepN, g_keepSel, want);
                }
            }
        }
    }
    // ...and the stat the story brings to this fight, if it is one of the few that has one. Not when
    // the story's own setup has already landed it: initSetupParty records how far it has got in
    // globalWork[210], and btsc03037's door runs setup_hgcr190 (= initSetupParty(2600)) itself.
    g_statWant = scene_stat(want);
    g_statFor  = g_statWant ? want : 0;
    g_statBase = -1; g_statAgain = 0; g_statActor = 0; g_statDone = 0;
    if (g_statWant) {
        int got = vm_get_work(SETUP_PROGRESS_WORK), cap = vm_capacity_level();
        const char* why = (got >= g_statWant->setup) ? "the setup step has run"
                        : (cap >= g_statWant->capLv) ? "the Crystarium is already at that stage"
                                                     : NULL;
        if (why) {
            const ff13_scene_stat* had = g_statWant;
            g_statWant = NULL; g_statFor = 0;
            ff13logv("[FF13-SRP] battle picker: btsc%05u adds nothing to status %d -- %s (setup "
                     "progress %d/%d, Crystarium stage %d/%d)\n", want, had->stat, why,
                     got, had->setup, cap, had->capLv);
        } else {
            ff13logv("[FF13-SRP] battle picker: btsc%05u is fought with status %d +%d on its one "
                     "party member (setup progress %d/%d, Crystarium stage %d/%d) -- it goes into "
                     "the battle actor once the fight is up and comes back out at the end\n", want,
                     g_statWant->stat, g_statWant->add, got, g_statWant->setup, cap,
                     g_statWant->capLv);
        }
    }
    // ...and which paradigm was in hand, so the field gets it back however the fight ends. The game
    // does this itself on the ordinary way back, but not for every fight -- see sel_restore_watch.
    g_selBefore = deck_sel_live();
    g_selLast = -1;
    tp_sample();               // TP as it stands now: what the fight gets back afterwards
    // ...and what is known about the enemies. Taking no snapshot is what leaves a kept fight's scan
    // alone; any snapshot still pending from an earlier fight is dropped with it, because putting
    // THAT one back after this fight would undo this fight's scan by proxy.
#ifdef FF13_DIAG
    libra_diag_before(want);   // an independent snapshot, so the write is visible either way
#endif
    // Taken for every fight, whether or not this pick keeps its scan: for an ordinary one it is
    // what the scan is put back TO, and for a kept one it is what the gain is measured against.
    libra_sample();
    // ...and for the handful of fights the story only ever meets cold, take the scan back off the
    // enemy before the fight is set up: the record it fights with is filled from the store here.
    libra_zero_take(want);
    g_homeHave = 0; g_homeDue = 0; g_homeAt = 0;
    // Rows that ask for it keep the spot (the mapset and music heals read it) but never warp back:
    // an arena fight's own end already puts the party somewhere sensible, and the warp from there
    // was the less stable of the two (measured).
    g_homeNoWarp = (ar && ar->noHomeWarp) ? 1 : 0;
    if (field_party_pos(&g_homeX, &g_homeY, &g_homeZ)) {
        g_homeZone = field_zone_id();
        g_homeHave = 1;
        // What the ground under the pick spot is made of, for the way back's reconcile arm.
        g_homeCellsN = save_capture_resident_cells(g_homeCells, HOME_CELLS_MAX);
        save_mapset_census("at the pick");
        // ...and which mapset it belongs to, for bg_heal_watch.
        strncpy(g_homeBg, g_curBg, sizeof g_homeBg - 1);
        g_homeBg[sizeof g_homeBg - 1] = 0;
        g_bgHealLast = -1;
        // ...and what the field was PLAYING, for bgm_heal_watch.
        // What is really playing, not what this mod calls the field's track: healing back to the
        // mod's idea plays the pre-battle track over a perfectly good one (measured).
        {
            char nowBgm[BGM_NAME_MAX + 1] = "";
            if (!bgm_current(nowBgm) || !nowBgm[0])
                strncpy(nowBgm, g_bgmFieldNow, BGM_NAME_MAX);
            strncpy(g_homeBgm, nowBgm, BGM_NAME_MAX);
        }
        g_homeBgm[BGM_NAME_MAX] = 0;
        g_bgmHealLast = -1; g_bgmHealAt = 0;
    } else {
        ff13loga("[FF13-SRP] battle picker: the party's position could not be read -- the way back "
                 "will leave you wherever the checkpoint does\n");
    }
    blackout_begin();
    cam_disarm();                          // whatever the last fight borrowed, give it back
#ifdef FF13_DIAG
    g_gatesDone = 0;
#endif
    g_camDone = 0; g_camWatch = GetTickCount() + 30000;
    ff13logv("[FF13-SRP] battle picker: starting btsc%05u (touch id=%u%s)\n",
             want, touch, g_lastTouchedId ? "" : ", picked from the object bitmap");
    // Field objects the story clears right before this fight. Door-less picks only -- a door's
    // chain does this itself. field_drop_watch owes them back; an earlier debt is left pending.
    if (!(ar && ar->ev[0])) {
        int dn = field_drops_apply(want);
        if (dn) {
            g_fieldDropFor = want;
            g_fieldDropZone = field_zone_id();
            g_fieldDropDue = 1; g_fieldDropFrom = GetTickCount();
            ff13log("[FF13-SRP] battle picker: %d field object(s) the story clears before "
                    "btsc%05u -- cleared for this fight\n", dn, want);
        }
    }
    // The fight's enemies need their models resident, and the set that is loaded is the one for
    // where you are STANDING, not for the fight you picked -- so ask for one that covers it and give
    // the load a moment (see charaset_needed).
    // Same for a fight whose arena terrain lives in a mapset the field has no reason to have.
    // Only when it is not already the one loaded: asking for the mapset that is already up is not
    // free -- the request is queued and can complete about a second AFTER the battle has started,
    // re-streaming the area underneath a fight that is already running.
    if (ar && ar->bg) {
        if (strncmp(g_curBg, ar->bg, sizeof g_curBg - 1) == 0) {
            ff13logv("[FF13-SRP] battle picker: arena mapset \"%s\" is already loaded -- left alone\n",
                     ar->bg);
        } else {
            ff13logv("[FF13-SRP] battle picker: loading arena mapset \"%s\" for this fight "
                     "(was \"%s\")\n", ar->bg, g_curBg[0] ? g_curBg : "(none seen)");
            vm_load_bg(ar->bg);
            g_bgAsked = 1;
        }
    }
    // And the battle's own mapsets, which the field never loads and so can never be already there:
    // the story fight is started off an event that has staged this room, ours off the field.
    if (ar) {
        for (int i = 0; i < 2 && ar->btlBg[i]; i++) {
            ff13logv("[FF13-SRP] battle picker: loading the fight's own mapset \"%s\"\n",
                     ar->btlBg[i]);
            vm_load_bg(ar->btlBg[i]);
            g_bgAsked = 1;
        }
    }
    // The room is lit later, once the party is standing in it -- see the note on scene_light_apply.
    g_lightMapAt = 0;
    g_lightMapN = 0;
    g_lightSweep = -1;
    g_sweepDone = 0;
    // A fight with an event door loads nothing here. The chain does its own loading, and preloading
    // first takes that away from it -- the chain finds the models resident and never asks for the
    // set it actually wanted, while its own setup call sits armed.
    const ff13_charaset* cs = (ar && ar->ev[0] && !g_evSkipDoor)
                            ? NULL : charaset_needed(field_zone_id(), want);
    // The table cannot always answer -- a scene can ask for a charaspec the sets carry under another
    // name, a different charaspec on the SAME model. The entry names the set the story itself loads.
    if (!cs && ar && ar->chset) {
        const char* cur = g_csLoaded[0] ? g_csLoaded : g_curCharset;
        if (strcmp(ar->chset, cur) != 0) cs = charaset_by_name(ar->chset);
    }
    if (cs) {
        strncpy(g_csRestore, g_curCharset, CSET_NAME_MAX);
        g_csRestore[CSET_NAME_MAX] = 0;
        ff13logv("[FF13-SRP] battle picker: loading character set \"%s\" for this fight "
                 "(was \"%s\"), starting in %dms\n",
                 cs->name, g_csRestore[0] ? g_csRestore : "?", g_bsPreloadMs);
        vm_load_charaset(cs->name);
        load_loose_specs(want, cs);        // after the set: it replaces, this adds
        strncpy(g_csLoaded, cs->name, CSET_NAME_MAX);
        g_csLoaded[CSET_NAME_MAX] = 0;
        // Nothing from the resource system is called here to shorten this -- see the note by
        // RVA_SYNCWAIT for the three that were tried and what each of them broke.
        g_bsTouchPending = touch;                  // bs_pump hands it over once the wait is up
        g_bsTouchDue = 1;
        g_bsTouchAt = GetTickCount() + (DWORD)g_bsPreloadMs;
        g_bsTouchCap = g_bsPreloadMs; g_bsTouchExtra = 0;
        return;
    }
    int asked = load_loose_specs(want, NULL);   // no set was loaded, so everything is missing
    // A fight that changes the party has one more thing to wait for that is in no scene's model
    // list: the member's own model. Handing the touch over at once put the member in the party but
    // not in the battle, and the game came down. The readiness probe cannot answer for a party
    // member either, so this is the full window rather than "until it is there".
    if (g_partyWho && g_partyWhoN > 0) {
        ff13logv("[FF13-SRP] battle picker: this fight brings its own party -- starting in %dms so "
                 "their models can arrive\n", g_bsPreloadMs);
        g_bsTouchPending = touch;
        g_bsTouchDue = 1;
        g_bsTouchFull = 1;      // the probe cannot answer for a party member: wait it out
        g_bsTouchAt = GetTickCount() + (DWORD)g_bsPreloadMs;
        g_bsTouchCap = g_bsPreloadMs; g_bsTouchExtra = 0;
        return;
    }
    // A fight the story stages goes through the arena door whether or not a character set had to be
    // loaded first: the arena is chosen in bs_pump, on the pending touch, so hand it over as pending
    // with the wait already expired. (Standing in the fight's own zone used to lose the arena.)
    if (ar) {
        g_bsTouchPending = touch;
        g_bsTouchDue = 1;
        // Due immediately only when nothing was asked for: a charaspec requested a line above is a
        // load in flight, and a fight started on top of one builds actors out of models that are
        // not there. A door is the exception -- its chain loads its own set on top and asks again.
        int wait = asked && !(ar->ev[0] && !g_evSkipDoor);
        g_bsTouchAt = GetTickCount() + (DWORD)(wait ? g_bsPreloadMs : 0);
        g_bsTouchCap = wait ? g_bsPreloadMs : 0; g_bsTouchExtra = 0;
        return;
    }
    if (asked) {
        ff13logv("[FF13-SRP] battle picker: %d charaspec(s) asked for by hand -- starting in %dms "
                 "so they can arrive\n", asked, g_bsPreloadMs);
        g_bsTouchPending = touch;
        g_bsTouchDue = 1;
        g_bsTouchAt = GetTickCount() + (DWORD)g_bsPreloadMs;
        g_bsTouchCap = g_bsPreloadMs; g_bsTouchExtra = 0;
        return;
    }
    // Hand the game a pending touch; its own doEncountCheck picks it up next frame and runs the
    // whole normal sequence (encounter info, presentation, trigger, battle music).
    blackout_end();                            // nothing was staged, so there is nothing to cover
    *(uint32_t*)(uintptr_t)(mgr + OFF_TOUCHED_ID) = touch;
}

// ---- ending a staged fight the way a lost one ends ---------------------------------------------
//
// The game's own way back is the one it takes when you lose and retry. The zone state machine's
// transition graph (built by 0x817D60) has exactly one edge into STATE_RESTART:
//
//     STATE_ENCOUNT_BACK(0x26) --TRG_RESTART_REQ(0x37)--> STATE_RESTART(0x4d)
//
// and 0x26 is the state every fight passes through on its way back to the field. The game posts 0x37
// there itself, from the worker 0x814B90, when the battle result carries the restart bit; that bit
// lives in a local the worker rebuilds every cycle and cannot be set from outside, but the trigger
// can simply be posted. Anywhere else the post is a clean no-op -- the resolver finds no edge.
// What the restart replays is the game's live checkpoint, not the spot you were standing on.
const uint8_t P_SENDTRIG[9] = {0x55,0x8B,0xEC,0x51,0x89,0x4D,0xFC,0x6A,0x00};
typedef void (__thiscall *sendTrig_t)(void* zsm, int trigger);
#define TRG_RESTART_REQ    0x37
#define STATE_ENCOUNT_BACK 0x26
// How long to keep looking for 0x26 after the fight ends: the way back runs a fade and a map restore,
// so it is not immediate. If it never comes, stop watching rather than fire the trigger into
// whatever the game moved on to.
#define RESTART_WATCH_MS 15000
volatile int   g_trigOk = 0;            // 0x817400 verified present
static volatile int   g_restartOnArena = 1;    // [battle_swap] arena_restart
static volatile int   g_ctlGiveBack   = 0;     // this restart owes the leader the field back
#define CTL_GIVEBACK_RETRY_MS 3000
static volatile DWORD g_ctlRetryUntil = 0;     // ...and the leader was not in the table yet
static volatile DWORD g_restartUntil = 0;      // watching until this tick (0 = not watching)
static volatile int   g_restartLast = -1;      // last state seen, so only changes are logged
#define RESTART_AFTER_MS 45000
static volatile DWORD g_afterUntil = 0;        // and the same again once the trigger has gone
static volatile int   g_afterLast = -1;
static volatile int   g_wantCP = 0;            // this fight should end at the checkpoint
volatile int   g_workerHowOk = 0;       // the worker can be told (see on_worker_how)

static uint32_t zsm_ptr(void) {
    uintptr_t p = g_base + 0x2411190;                      // ZoneStateManager facade
    if (IsBadReadPtr((void*)p, 4)) return 0;
    uint32_t facade = *(uint32_t*)p;
    if (!facade || IsBadReadPtr((void*)(uintptr_t)(facade + 0x14), 4)) return 0;
    return *(uint32_t*)(uintptr_t)(facade + 0x14);         // [facade+0x14] = the state machine
}

static void restart_after_watch(void) {
    if (!g_afterUntil) return;
    if ((int)(GetTickCount() - g_afterUntil) >= 0) { g_afterUntil = 0; return; }
    int st = field_zone_state();
    if (st == g_afterLast) return;
    g_afterLast = st;
    ff13logv("[FF13-SRP] restarting: zone state %d\n", st);
    if (st == 3) {
        g_afterUntil = 0; g_wantCP = 0;
        ctl_unlock();                  // safety net: never carry the event lock onto the field
        evquiet_lift();                // ...nor the story's auto-music-off bit
        save_mapset_census("back on the field");
        // ...and the same for a lock this mod never took. Only where the mod asked for the restart
        // in the first place, which is the case that has no story left to give control back.
        // A miss is not "nothing to do": the chara table can still be short of the leader this
        // soon after a restart, and this state edge does not come round a second time. Keep asking
        // -- the debt is a field the player cannot walk in.
        if (g_ctlGiveBack) {
            g_ctlGiveBack = 0;
            if (!ctl_force_on()) g_ctlRetryUntil = GetTickCount() + CTL_GIVEBACK_RETRY_MS;
        }
        ff13logv("[FF13-SRP] restarting: back on the field\n");
    }
}

// ---- ...and the same way out of a GAME OVER, which does not take it by itself -----------------
// A picked fight that is LOST goes back through a restart the mod never asked for, and that restart
// rebuilds nothing: no STATE_RESTART, no loadBg, no character set, one activateArea. The field is
// then left to fault the map in by itself at FIELD_IDLE.
// DO NOT ask for that restart ourselves to fix it. It shipped briefly as `picked_reload` and was
// removed: after a restart the MOD asked for, the field is never handed back -- doEncountCheck
// throws every symbol contact away (its third gate stays non-zero) and the pause menu will not open.
// A game over's own restart hands control back at the end of it; one asked for from outside has no
// such ending, and that is as true of the loss side as of the win side.
// To take this on again, start with what the game runs at the end of its OWN restart to give the
// field back -- not switchControl, which ctl_force_on already tried.
#define PICKED_RECENT_MS 60000
#define BS_GIVEUP_MS     20000         // a pick that has not become a fight by now is not going to
static volatile DWORD g_goUp = 0;              // when the GAME OVER screen came up
// (g_pickedEndAt is declared up with the battle-entry hook -- that is where it is CLEARED, by any
// battle that is not ours, which is what keeps an ordinary game over out of the practice shortcuts.)

// A chain-marked checkpoint's restart loader loads the FIGHT's mapset, not the field's -- void
// beyond it, which the story never walks and a picked win does. Watched by state rather than from
// fight-over: a mid-fight Restart never reaches the fight-over code.
// The walk is a virtual call per slot, so it is asked five times a second, not per frame.
static void ctl_giveback_retry(void) {
    static DWORD next = 0;
    if (!g_ctlRetryUntil) return;
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0) return;
    next = now + 200;
    if (ctl_force_on()) { g_ctlRetryUntil = 0; return; }
    if ((int)(now - g_ctlRetryUntil) >= 0) {
        g_ctlRetryUntil = 0;
        ff13logv("[FF13-SRP] battle picker: the leader never turned up in the chara table -- "
                 "leaving control alone\n");
    }
}

static void bg_heal_watch(void) {
    if (!g_homeBg[0]) return;
    if (!g_bsStarted
        && !(g_pickedEndAt && (DWORD)(GetTickCount() - g_pickedEndAt) < PICKED_RECENT_MS)) {
        g_homeBg[0] = 0;                               // the pick is long gone; stop watching
        return;
    }
    int st  = field_zone_state();
    int was = g_bgHealLast;
    g_bgHealLast = st;
    if (st != 3 || was == 3 || was < 0) return;        // only on coming BACK to the field
    if (field_zone_id() != g_homeZone) { g_homeBg[0] = 0; return; }
    // The descriptor in force, not the name the game last asked for: an arena fight leaves the
    // field on the ARENA's mapset without a loadBg of its own, so g_curBg still reads the field's
    // and the hole in the ground goes unhealed (measured).
    char now[17];
    if (!save_current_mapset(now)) return;
    if (!strcmp(now, g_homeBg)) return;
    ff13loga("[FF13-SRP] battle picker: the field came back on \"%s\" (the fight's own mapset) -- "
             "asking for \"%s\" back\n", now, g_homeBg);
    vm_load_bg(g_homeBg);
    // Ours does not update g_curBg (by design); after the heal the field really is on it again.
    strncpy(g_curBg, g_homeBg, sizeof g_curBg - 1);
    g_curBg[sizeof g_curBg - 1] = 0;
    g_homeBg[0] = 0;
}

// A checkpoint restart starts the music the STORY has for that checkpoint, and the end-of-fight
// restore only puts back tracks the mod started. Settled first: a chain speaks after FIELD_IDLE.
#define BGM_HEAL_SETTLE_MS 800
// An ordinary encounter changes the field's music for its own reasons, and the way back from one
// is not damage to repair. Called from startBattle for a fight that is not ours.
void bgm_heal_standdown(void) { g_homeBgm[0] = 0; }

static void bgm_heal_watch(void) {
    if (!g_homeBgm[0]) return;
    if (!g_bsStarted
        && !(g_pickedEndAt && (DWORD)(GetTickCount() - g_pickedEndAt) < PICKED_RECENT_MS)) {
        g_homeBgm[0] = 0;                              // the pick is long gone; stop watching
        return;
    }
    int st  = field_zone_state();
    int was = g_bgmHealLast;
    g_bgmHealLast = st;
    if (st != 3) { g_bgmHealAt = 0; return; }
    if (was != 3) { g_bgmHealAt = GetTickCount() + BGM_HEAL_SETTLE_MS; return; }
    if (!g_bgmHealAt || (int)(GetTickCount() - g_bgmHealAt) < 0) return;
    g_bgmHealAt = 0;
    if (field_zone_id() != g_homeZone) { g_homeBgm[0] = 0; return; }
    char cur[BGM_NAME_MAX + 1];
    if (!bgm_current(cur) || !cur[0]) return;
    if (strncmp(cur, g_homeBgm, BGM_NAME_MAX) == 0) { g_homeBgm[0] = 0; return; }
    // Stopped, not just played over: a track the manager no longer calls current keeps sounding.
    ff13loga("[FF13-SRP] battle picker: the way back left the field on \"%.16s\" -- stopping it and "
             "asking for \"%.16s\", which is what was playing at the pick\n", cur, g_homeBgm);
    bgm_stop(cur, 0.0f);
    bgm_lazy_play(g_homeBgm);
    g_homeBgm[0] = 0;
}

// ---- the preparation menu, made to not open ---------------------------------------------------
// A chain's restart branch does the fade the pick needs AND opens this menu; the fade cannot be
// issued from outside (it crashes there), so the branch runs and the menu is stopped instead.
// NOT a plain return: the native first takes its argument off the VM operand stack ([argp+4], then
// argp += 8), and leaving it there desyncs the VM. Armed while a pick is staged only.
#define MENUSKIP_MS 60000
static volatile DWORD g_menuSkipUntil = 0;
#define RVA_SHOWMAINMENU 0x7E80D0      // VA 0xBE80D0  Window.showMainMenu(int)
static const uint8_t P_SHOWMENU[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38 };
volatile unsigned char g_menuSkip = 0;   // 1 = the next showMainMenu does nothing

int menuskip_install(void) {
    if (!g_base) return 0;
    if (wait_for_bytes(g_base + RVA_SHOWMAINMENU, P_SHOWMENU, (int)sizeof P_SHOWMENU, 15000) < 0)
        return 0;
    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) return 0;
    uintptr_t site = g_base + RVA_SHOWMAINMENU;
    uint8_t* p = cave;
    uint32_t flag = (uint32_t)(uintptr_t)&g_menuSkip;
    *p++ = 0x80; *p++ = 0x3D; memcpy(p, &flag, 4); p += 4; *p++ = 0x00;   // cmp byte [flag],0
    *p++ = 0x74; uint8_t* je = p++;                                        // je RUNORIG
    // The skip: consume the one argument the way the native does, then return (cdecl).
    *p++ = 0x55;                                                           // push ebp
    *p++ = 0x8B; *p++ = 0xEC;                                              // mov ebp,esp
    *p++ = 0x8B; *p++ = 0x45; *p++ = 0x08;                                 // mov eax,[ebp+8]
    *p++ = 0x8B; *p++ = 0x88; { uint32_t o = 0x114; memcpy(p, &o, 4); p += 4; }  // mov ecx,[eax+114]
    *p++ = 0x83; *p++ = 0xC1; *p++ = 0x08;                                 // add ecx,8
    *p++ = 0x89; *p++ = 0x88; { uint32_t o = 0x114; memcpy(p, &o, 4); p += 4; }  // mov [eax+114],ecx
    *p++ = 0x5D;                                                           // pop ebp
    *p++ = 0xC3;                                                           // ret
    *je = (uint8_t)(p - (je + 1));
    memcpy(p, P_SHOWMENU, sizeof P_SHOWMENU); p += sizeof P_SHOWMENU;      // the original prologue
    *p++ = 0xE9;
    { int32_t rel = (int32_t)((site + sizeof P_SHOWMENU) - ((uintptr_t)p + 4)); memcpy(p, &rel, 4); }
    DWORD old = 0;
    if (!VirtualProtect((void*)site, sizeof P_SHOWMENU, PAGE_EXECUTE_READWRITE, &old)) return 0;
    uint8_t* at = (uint8_t*)site;
    at[0] = 0xE9;
    { int32_t rel = (int32_t)((uintptr_t)cave - (site + 5)); memcpy(at + 1, &rel, 4); }
    at[5] = 0x90;
    VirtualProtect((void*)site, sizeof P_SHOWMENU, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, sizeof P_SHOWMENU);
    return 1;
}

// The chain's restart loader raises an event flag and the chain opens its preparation menu
// whenever it is up. A picked fight's way back IS a restart, so the flag is taken down after it.
static void menuskip_watch(void) {
    if (!g_menuSkip) return;
    if (g_bsInFight || !g_menuSkipUntil || (int)(GetTickCount() - g_menuSkipUntil) >= 0) {
        g_menuSkip = 0; g_menuSkipUntil = 0;
        ff13logv("[FF13-SRP] battle picker: the preparation menu is the field's own again\n");
    }
}

static void rpflag_watch(void) {
    if (!g_rpFlag) return;
    // Only tracked while the fight is on: the chain's own loads take the field through FIELD_IDLE
    // several times before the restart that raises the flag.
    if (g_bsStarted) { g_rpFlagLast = field_zone_state(); return; }
    if (!(g_pickedEndAt && (DWORD)(GetTickCount() - g_pickedEndAt) < PICKED_RECENT_MS)) {
        g_rpFlag = NULL;                               // the pick is long gone; stop watching
        return;
    }
    int st  = field_zone_state();
    int was = g_rpFlagLast;
    g_rpFlagLast = st;
    if (st != 3 || was == 3 || was < 0) return;        // only on coming BACK to the field
    // Down is not "done": a win reaches the field before the restart that raises it.
    if (!vm_get_event_flag(g_rpFlag)) return;
    vm_set_event_flag(g_rpFlag, 0);
    ff13loga("[FF13-SRP] battle picker: the way back raised \"%s\" (the chain's restart flag) -- "
             "taken down, so the next pick opens no preparation menu\n", g_rpFlag);
    g_rpFlag = NULL;
}

// The other half of the gated replay (see g_gateReplayVal): once the reload has settled on the
// field, hand control back the way the game's own restarts do -- com_ucon_ev, nothing else.
static volatile int   g_uconLast = -1;
static volatile DWORD g_uconAt   = 0;
static void ucon_watch(void) {
    if (!g_uconDue) { g_uconLast = -1; g_uconAt = 0; return; }
    int st = field_zone_state();
    int was = g_uconLast;
    g_uconLast = st;
    if (st != 3) { g_uconAt = 0; return; }
    if (was != 3) { g_uconAt = GetTickCount() + 1500; return; }   // let the field settle first
    if (!g_uconAt || (int)(GetTickCount() - g_uconAt) < 0) return;
    g_uconDue = 0; g_uconAt = 0;
    ff13loga("[FF13-SRP] battle picker: the replay was stopped on the field -- fade in and "
             "com_ucon_ev for control, as the game's own restarts end\n");
    vm_fade_in();               // fs_Restart faded out and its chain never fades back in
    vm_field_ucon();
}

static void restart_watch(void) {
    if (!g_restartUntil) return;
    int st = field_zone_state();
    if (st != g_restartLast) {
        g_restartLast = st;
        ff13logv("[FF13-SRP] after the fight: zone state %d\n", st);
    }
    if (st == STATE_ENCOUNT_BACK) {
        uint32_t zsm = zsm_ptr();
        g_restartUntil = 0;
        if (!zsm) {
            ff13logv("[FF13-SRP] after the fight: no state machine -> left alone\n");
            return;
        }
        if (g_workerHowOk) {
            // The worker has been told; it posts at its own moment and the restart completes.
            ff13logv("[FF13-SRP] after the fight: the way back is open; the worker has it\n");
        } else {
            ff13logv("[FF13-SRP] after the fight: back at the checkpoint instead of on with the "
                     "story (TRG_RESTART_REQ)\n");
            ((sendTrig_t)(uintptr_t)(g_base + RVA_SENDTRIG))((void*)(uintptr_t)zsm,
                                                             TRG_RESTART_REQ);
        }
        // Keep reading the state out afterwards: some fights come back through it onto the field,
        // and one was seen to sit in STATE_RESTART (77) for a full minute with its victory movie
        // playing underneath. Which states it passes through is what separates those two cases.
        g_afterUntil = GetTickCount() + RESTART_AFTER_MS;
        g_afterLast = -1;
        return;
    }
    if ((int)(GetTickCount() - g_restartUntil) >= 0) {
        g_restartUntil = 0;
        ff13logv("[FF13-SRP] after the fight: the way back never passed ENCOUNT_BACK in %dms "
                 "(state %d) -- left alone\n", RESTART_WATCH_MS, st);
    }
}

// DO NOT re-post TRG_PAUSE_REQ / TRG_BATTLE_ABORT_REQ once a battle has declared its outcome, as a
// way of ending a fight early: the chain does run, but on hardware the restart hung on the loading
// screen and cancelling the pause could not re-enter the fight either. Whatever the outcome
// declaration tears down is something both ends of the pause need.

// ---- a game over goes straight back to the checkpoint ----------------------------------------
// DO NOT use the game's own dflag.debug_auto_restart (bit 0x50 of the flag bits at 0x2820D4C) for
// this. The site it gates also fires in the gap between the last enemy dying and the win being
// declared, and both gates tried around that gap crashed hard on hardware the frame an enemy hit
// zero -- and the flag changes nothing on a retail loss anyway.
//
// The lever that works is the GAME OVER menu's own little state machine, found by the window names
// it builds ("#GAMEOVER_SEL", "#GAMEOVER_CONF"):
//
//   0x8ED130      the menu's update, __thiscall(ecx = the menu controller)
//   [ctrl+0xA50]  its state, 1..6 through a jump table at 0x8ED914
//   state 4       the one that waits on the selection: it looks the choice up by name
//                 (0x8EEA60(ctrl, &name, 0)), asks whether it has been answered (0x9F7D60 -- byte
//                 [choice+0x3A] is 2 or 3), reads the chosen index at [choice+0x84], and takes
//                 index 0 to state 7 (restart) and anything else to state 5 (return to title).
//
// So the skip is not a state to force -- it is an ANSWER to supply. Stamp the choice as answered
// with index 0 and the menu's own state 4 runs its ordinary restart path on the very next frame.
#define RVA_STRCTOR        0x0112B0    // VA 0x4112B0 __thiscall(ecx = buf, const char* literal)
#define RVA_WINFIND        0x4EEA60    // VA 0x8EEA60 __thiscall(ecx = ctrl, name, flag)
#define GO_OFF_STATE       0xA50       // in the controller
#define GO_STATE_SELECT    4           // waiting on the selection
#define GO_OFF_ANSWERED    0x3A        // in the choice: 2 or 3 = the player has answered
#define GO_OFF_INDEX       0x84        // in the choice: which item
volatile int g_goOk = 0;        // the menu update is hooked
static volatile int g_goDone = 0;      // this menu has been answered already
// The menu update's prologue, byte for byte: push ebp; mov ebp,esp; sub esp,0x374.
const uint8_t P_GOMENU[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x74,0x03,0x00,0x00};
static volatile int g_autoRestart = 1;         // [battle_swap] auto_restart

// ---- ...and how long it takes to say it ------------------------------------------------------
// The GAME OVER screen is on the critical path back to the field: the battle-over worker calls
// 0x815DE0 with how==2 and that does not return until the screen is finished. Its states, in order:
//
//   1  makes the caption (the $gameover_01 layout), starts it playing IN, plays SE 0x16
//   2  waits for the caption to finish playing in            <- the long one, and it only waits
//   3  builds the "|Restart|Quit" window (#GAMEOVER_SEL)
//   4  waits for the answer -- supplied above -- then starts the caption playing out
//   7  nothing; the poll 0x8ED980 waits for the caption to reach "done" and destroys it
//
// State 2 does nothing but wait, so state 3 can simply be written into it: nothing is skipped except
// the waiting, and playing the caption out while it is still playing in is a case the widget already
// handles. The caption itself may NOT be skipped -- it is what the poll waits on, and with no
// caption 0x8ED980 answers "not yet" for ever and the worker never resumes.
#define GO_STATE_APPEAR    2           // waiting for the caption to finish appearing
#define GO_STATE_MENU      3           // ...and the state that stops waiting
static volatile int   g_goFast = 1;    // the GAME OVER screen answers itself after a picked fight
#ifdef FF13_DIAG
static volatile int   g_goSeen = 0;    // which state the last frame was in
static volatile DWORD g_goAt = 0;      // ...and when it started
#endif

// ---- ...and the noise it makes ---------------------------------------------------------------
// State 1 also fires the sound, from one site only -- the 13 bytes at RVA_GOSE:
//
//   008ED24D  6A 16                 push 0x16              ; the sound id: "system_g_over"
//   008ED24F  8B 8D AC FC FF FF     mov  ecx, [ebp-0x354]  ; the controller
//   008ED255  E8 C6 5D 00 00        call 0x8F3020          ; GuiManager::playSE
//
// Nothing else in the game reaches this site. Fill ALL 13 with NOPs, not just the call: playSE takes
// its argument off the stack and cleans up itself, so a call removed while its push stays would
// leave the stack short by four bytes. The mov goes with them -- nothing reads ecx once the call is
// gone. Patched at the moment it is needed rather than watched per frame.
const uint8_t P_GOSE[GO_SE_LEN] = {0x6A,0x16,0x8B,0x8D,0xAC,0xFC,0xFF,0xFF,
                                          0xE8,0xC6,0x5D,0x00,0x00};
// ---- ...and none of it belongs to an ordinary game over ---------------------------------------
// Everything this block does -- skipping the caption's wait, answering the menu, and the silence --
// was written for a practice fight retried forty times an hour; on an ordinary game over it would
// take the screen, the choice and the sound away from a moment the game means the player to sit
// through. So all three are held to a game over that belongs to a fight the picker staged
// (g_pickedEndAt, cleared the moment ANY other battle begins).
// The sound is a byte patch on the game's own code, so it is written per screen rather than once at
// startup: this handler runs at the TOP of the update, before the state's body, so the bytes are
// always already what they need to be by the time anything can execute them.
volatile int g_goSeOk = 0;         // the site matched its prologue at startup
static volatile int g_goSeQuiet = -1;     // what the bytes currently say (-1 = untouched)

static void go_se_set(int quiet) {
    if (!g_goSeOk || g_goSeQuiet == quiet) return;
    uint8_t* at = (uint8_t*)(g_base + RVA_GOSE);
    DWORD old = 0;
    if (!VirtualProtect(at, GO_SE_LEN, PAGE_EXECUTE_READWRITE, &old)) return;
    if (quiet) memset(at, 0x90, GO_SE_LEN);           // push, mov and call alike
    else       memcpy(at, P_GOSE, GO_SE_LEN);
    VirtualProtect(at, GO_SE_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, GO_SE_LEN);
    g_goSeQuiet = quiet;
}

#ifdef FF13_DIAG
// Which latch really says "lost"? Record the evidence rather than guess: the three latch words at
// BM+0x38 (bits 0x00..0x5F -- 0x45 the win latch lives in the third) and the outcome word [BM+0xEC],
// one line per change. One death read back from this tells which bit rises at the wipe, which at the
// menu, and which never rises on a win.
static void bm_latch_watch(void) {
    static uint32_t last[5] = { 0, 0, 0, 0, 0 };
    static int      had = 0;
    if (!g_base) return;
    uintptr_t bmp = g_base + RVA_BM_PTR;
    if (IsBadReadPtr((void*)bmp, 4)) return;
    uint32_t bm = *(uint32_t*)bmp;
    if (!bm || IsBadReadPtr((void*)(uintptr_t)(bm + 0x38), 0x198)) { had = 0; return; }
    // The three bytes are what 0x4D9880 decides "the fight is over" from: two alive-counts (which
    // side is which is what this log answers) and a hold-off. Logged as one word for the diff.
    uint32_t cnt = (uint32_t)*(uint8_t*)(uintptr_t)(bm + 0x1C4)
                 | ((uint32_t)*(uint8_t*)(uintptr_t)(bm + 0x1C5) << 8)
                 | ((uint32_t)*(uint8_t*)(uintptr_t)(bm + 0x1C8) << 16);
    uint32_t cur[5] = { *(uint32_t*)(uintptr_t)(bm + 0x38), *(uint32_t*)(uintptr_t)(bm + 0x3C),
                        *(uint32_t*)(uintptr_t)(bm + 0x40), *(uint32_t*)(uintptr_t)(bm + 0xEC),
                        cnt };
    if (had && cur[0] == last[0] && cur[1] == last[1] && cur[2] == last[2] && cur[3] == last[3]
            && cur[4] == last[4])
        return;
    ff13logv("[FF13-SRP] BM latches: %08X %08X %08X  outcome %u  counts 1C4=%u 1C5=%u hold=%u\n",
             cur[0], cur[1], cur[2], cur[3], cnt & 0xFF, (cnt >> 8) & 0xFF, cnt >> 16);
    for (int i = 0; i < 5; i++) last[i] = cur[i];
    had = 1;
}
#endif

typedef void (__thiscall *strCtor_t)(void* buf, const char* lit);
typedef void* (__thiscall *winFind_t)(void* ctrl, void* name, int flag);

// __cdecl handler, ecx = the menu controller (ff13_install_ecx_hook). Runs on the game thread,
// once a frame while the menu is up, at the top of the update -- so a state written here is the
// state the game dispatches on this very frame.
void __cdecl on_gameover_menu(uint32_t ctrl) {
    if (!g_base || !ctrl) return;
    if (IsBadReadPtr((void*)(uintptr_t)(ctrl + GO_OFF_STATE), 4)) return;
    uint32_t st = *(uint32_t*)(uintptr_t)(ctrl + GO_OFF_STATE);
#ifdef FF13_DIAG
    if ((int)st != g_goSeen) {              // where the time goes, one line per step
        DWORD now = GetTickCount();
        if (g_goSeen)
            ff13logv("[FF13-SRP] game over: step %d took %lums\n", g_goSeen, now - g_goAt);
        g_goSeen = (int)st;
        g_goAt = now;
    }
#endif
    // Whose game over is this? Only a fight the picker staged gets the shortcuts. Decided every
    // frame the screen is up, and the sound with it, because the byte patch has to be right before
    // state 1 runs its body.
    int mine = g_pickedEndAt
            && (DWORD)(GetTickCount() - g_pickedEndAt) < PICKED_RECENT_MS;
    go_se_set(mine ? 1 : 0);
    if (st == GO_STATE_APPEAR && mine && g_autoRestart && g_goFast) {
        *(uint32_t*)(uintptr_t)(ctrl + GO_OFF_STATE) = GO_STATE_MENU;   // ask now, not after
        return;
    }
    if (st != GO_STATE_SELECT) {
        if (st < GO_STATE_SELECT) { if (!g_goDone && st == 1) g_goUp = GetTickCount();
                                    g_goDone = 0; }
        return;
    }
    if (!mine || !g_autoRestart || g_goDone) return;
    {
        uint8_t name[0x40];
        void*   choice;
        memset(name, 0, sizeof name);
        ((strCtor_t)(uintptr_t)(g_base + RVA_STRCTOR))(name, "#GAMEOVER_SEL");
        choice = ((winFind_t)(uintptr_t)(g_base + RVA_WINFIND))((void*)(uintptr_t)ctrl, name, 0);
        if (!choice || IsBadWritePtr(choice, GO_OFF_INDEX + 4)) return;
        // Already answered (the player was faster): leave it entirely alone.
        {
            uint8_t a = *((uint8_t*)choice + GO_OFF_ANSWERED);
            if (a == 2 || a == 3) { g_goDone = 1; return; }
        }
        *(uint32_t*)((uint8_t*)choice + GO_OFF_INDEX) = 0;      // the first item: restart
        *((uint8_t*)choice + GO_OFF_ANSWERED) = 2;              // ...and it has been chosen
        g_goDone = 1;
        ff13logv("[FF13-SRP] game over: answering the menu for you -- restart, %lums after the "
                 "screen came up\n", GetTickCount() - g_goUp);
    }
}

// Ask for the checkpoint ending the way the game asks for it.
//
// The worker that ends a battle (0x814B90) copies a bit array into a local and at 0x81501B tests
// bit 0 of it; if it is set, 0x815044 posts TRG_RESTART_REQ itself, from inside the worker, at the
// point in its own sequence where the restart job can actually run.
//
// The array is NOT the encounter info: 0xB4FD60 -> 0xB52B50(ecx = [0x282B190], out) copies from
// [mgr + 0x228], the battle RESULT, not the +0x1CC the encounter info lives at. Setting bit 0 there
// was tried first and the fight ended through the ordinary result screen exactly as before.
// Posting the trigger ourselves from the frame tick reaches the same state and then the job never
// runs at all -- one fight sat at STATE_RESTART for forty-five seconds. Setting the bit instead
// leaves the moment to the game.
static volatile int   g_restartBitSet = 0;

// Ending a won fight the way a lost one ends -- in the worker, by telling it what it asked.
//
// The battle-over worker 0x814B90 reads how the battle ended into [ebp-4] (0x814DD4, the return of
// 0x4BBEA0) and throws everything else out at 0x814F49:
//
//     if ([ebp-4] != 0 && [ebp-4] != 2) goto out;
//
// Measured on hardware: losing and choosing retry gives 2, winning gives 1, and with a 2 the worker
// goes on to post TRG_RESTART_REQ itself and the restart completes in about a second.
//
// So write a 2. Hooked at 0x814DDC, the instruction after the store -- five bytes, position-
// independent, and the frame is whole there (ebp = esp + 0x808), so [ebp-4] is at esp + 0x804.
// From there the whole thing is the game's own: its worker, its trigger, its moment.
//
// The visible cost is that this IS the retry path, so the fight ends on the game's GAME OVER caption
// before it takes you back. Everything a win would have given is put back anyway (the refund work).
const uint8_t P_WORKHOW[5] = {0xA1,0x68,0xF0,0x7F,0x02};
#define WORKER_HOW_EBP 0x808         // ebp = esp + this
void __cdecl on_worker_how(uint32_t esp) {
    if (!g_wantCP) return;
    uintptr_t f = (uintptr_t)esp + WORKER_HOW_EBP;
    if (IsBadWritePtr((void*)(f - 4), 4)) return;
    int* how = (int*)(f - 4);
    if (*how == 2) return;                       // already ending the way we want
    if (!g_restartBitSet) {
        g_restartBitSet = 1;
        ff13logv("[FF13-SRP] battle over: it ended %d; telling the worker 2, which is what a retry "
                 "gives it\n", *how);
    }
    *how = 2;
}

static void encinfo_tweak(void) {
    if (!g_encClear && !g_encSet) return;
    if (!g_encUntil || (int)(GetTickCount() - g_encUntil) >= 0) return;
    uintptr_t p = g_base + RVA_ENCMGR;
    if (IsBadReadPtr((void*)p, 4)) return;
    uint32_t mgr = *(uint32_t*)p;
    if (!mgr || IsBadWritePtr((void*)(uintptr_t)(mgr + OFF_ENC_BITS), 4)) return;
    uint32_t* w = (uint32_t*)(uintptr_t)(mgr + OFF_ENC_BITS);
    uint32_t was = *w;
    uint32_t now = (was & ~g_encClear) | g_encSet;
    if (now == was) return;
    *w = now;
    if (was != g_encLast) {                 // one line per value it had to be talked out of
        g_encLast = was;
        ff13logv("[FF13-SRP] encounter info: %08X -> %08X (clear %08X, set %08X)\n",
                 was, now, g_encClear, g_encSet);
    }
}

#ifdef FF13_DIAG
// ---- who gets placed, and from what ------------------------------------------------------------
// 0x466720 decides an actor's opening transform (it prints the game's own
// "[BattleChara%d<%s>(oh:%d)]_init, x y z"); its first branch is
//     if (arg10 != 0) { transform = *(mat4*)(this + 0x38); print; return; }
// so the enemy's place is a MATRIX at [actor+0x38] that exists before this runs.
// The two doors do not place things the same way either. A field encounter comes out RELATIVE (the
// game puts every actor where the scene data says); a scripted (arena) fight comes out ABSOLUTE, in
// world coordinates, and whoever called it is expected to have put the party there. What decides it
// is the encounter-info bit array at [0x282B190] + 0x1CC (setEncountInfo @0xB500E0 writes it; the
// bit setter is 0xB5E9F0(index, value)).
//
// 0x466720 composes the arena's transform onto an actor's own placement only when a chain of
// conditions holds, two of them bits of the array below:
//   0x466813  bit 0x23 of [[0x27FD20C]+0x38]
//   0x4668FE  bit 0x4A of [[0x27FD20C]+0x38]   <- must be SET or the arena is skipped
// The other two gates are encounter-info bits 8 and 9, already clear in our fights. Read only.
#define RVA_BATFLAGS 0x23FD20C       // VA 0x27FD20C
#define OFF_BF_BITS  0x38
static void batflags_probe(const char* when) {
    uintptr_t p = g_base + RVA_BATFLAGS;
    if (IsBadReadPtr((void*)p, 4)) return;
    uint32_t o = *(uint32_t*)p;
    if (!o || IsBadReadPtr((void*)(uintptr_t)(o + OFF_BF_BITS), 16)) return;
    const uint32_t* b = (const uint32_t*)(uintptr_t)(o + OFF_BF_BITS);
    ff13logv("[FF13-SRP] battle flags (%s): %08X %08X %08X %08X  (bit35=%d bit74=%d)\n", when,
             b[0], b[1], b[2], b[3], (b[1] >> 3) & 1, (b[2] >> 10) & 1);
}

#ifdef FF13_DIAG
// ---- why a won fight does not reach the restart ------------------------------------------------
//
//   0x814DD4  [ebp-4] = 0x4BBEA0()                 // how the battle ended
//   0x814F49  if ([ebp-4] != 0 && [ebp-4] != 2) goto 0x8152CC        // out
//   0x814F5D  [ebp-0x204] = 0x815DE0([ebp-4])
//   0x814F91  if ([ebp-0x204] < 0) goto ...        // out
//   0x815003  ...bit 0 of the local... 0x815044 post TRG_RESTART_REQ
//
// A hook on 0x815003 never fired for a won fight, so one of the two tests above throws it out.
// Read both, on a win and on a loss. Read only.
const uint8_t P_WORKIN[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x08,0x08,0x00,0x00};
const uint8_t P_WORKARM[5] = {0x68,0x80,0xEC,0x0C,0x01};
#define WORKER_EBP 0x808             // ebp = esp + this, at both sites
static volatile int g_workLogN = 0;

void __cdecl on_worker_in(uint32_t esp) {
    (void)esp;
    if (g_workLogN >= 12) return;
    g_workLogN++;
    ff13logv("[FF13-SRP] battle-over worker: in\n");
}

void __cdecl on_worker_arm(uint32_t esp) {
    if (g_workLogN >= 12) return;
    uintptr_t f = (uintptr_t)esp + WORKER_EBP;
    if (IsBadReadPtr((void*)(f - 0x204), 4) || IsBadReadPtr((void*)(f - 4), 4)) return;
    g_workLogN++;
    ff13logv("[FF13-SRP] battle-over worker: how=%d gate=%d%s\n",
             *(int*)(f - 4), *(int*)(f - 0x204),
             *(int*)(f - 0x204) >= 0 ? "  (reaches the restart test)" : "  (thrown out)");
}
#endif

void encinfo_probe(const char* when) {
    uintptr_t p = g_base + RVA_ENCMGR;
    if (IsBadReadPtr((void*)p, 4)) return;
    uint32_t mgr = *(uint32_t*)p;
    if (!mgr || IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_ENC_BITS), 16)) return;
    const uint32_t* b = (const uint32_t*)(uintptr_t)(mgr + OFF_ENC_BITS);
    ff13logv("[FF13-SRP] encounter info (%s): %08X %08X %08X %08X\n", when, b[0], b[1], b[2], b[3]);
}
#endif

// ---- giving the boss a place of its own --------------------------------------------------------
//
// 0x466720 fetches every battle actor's opening transform through
//
//     0x6C3C60(id = [this+8], out = &localMatrix)
//
// which looks the id up in the manager at [0x26E80DC] (0x6A18B0) and asks it for its 4x4 (0x7C43F0).
// Nothing of ours is registered under the boss's id, so the lookup comes back with nothing and the
// boss opens at 0 0 0. What the game hands out here is a MATRIX, once, and the actor keeps it --
// facing included -- unlike the position the actor commits each frame.
//
// The result sits in the caller's frame at [ebp-0x70] and nothing reads it until the trace prints,
// so it can simply be overwritten. 0x466C07 (mov eax, 2) is the first position-independent
// instruction after the join, the frame is whole there (esp = ebp - 0x520, the lookup cleaned its
// own arguments), and a write through the caller's frame survives popad.
//
// Rows come from the battle's own stage, turned half round so the boss looks back down it at the
// party (the scene authors its facing as -179.5 degrees). Row 3 is where the scene puts it.
// [battle_swap] arena_place_matrix = 0 turns it off.
// 0x471140 takes a string OBJECT, not a char buffer -- built the way its own callers build it.
typedef void (__thiscall *specInstall_t)(void* actor, void* name, int mode);
static void spec_install(uint32_t actor, const char* spec) {
    uint8_t nm[0x40];
    memset(nm, 0, sizeof nm);
    ((strCtor_t)(uintptr_t)(g_base + RVA_STRCTOR))(nm, spec);
    ((specInstall_t)(uintptr_t)(g_base + RVA_SPECREPL))((void*)(uintptr_t)actor, nm, 0);
}

const uint8_t P_PLACEJOIN[5] = {0xB8,0x02,0x00,0x00,0x00};
#define OPEN_LOG_GAP_MS 3000     // quiet for this long = the next fight is building
#define PLACE_EBP_OFF  0x520         // ebp = esp + this, at that instruction
#define PLACE_OUT_OFF  0x70          // ...and the matrix is at ebp - this
#define PLACE_THIS_OFF 0x51C
static volatile int g_placeMatrix = 1;
static volatile int g_placeMatN = 0;
volatile int g_bossGotMatrix = 0;
static volatile int g_lightDone = 0;         // ...and the fight has been lit once they exist

#ifdef FF13_DIAG
// Which early-out turned an actor away: one that never reaches the write below is left on the
// commit hook's leash, which pins it where it was put -- an enemy that cannot walk.
static volatile int g_pjLog = 0;
#define PJ_WHY(why) do { if (g_placeBoss && g_pjLog < 12) { g_pjLog++; \
        ff13logv("[FF13-SRP] placement join: turned away (%s)\n", (why)); } } while (0)
#else
#define PJ_WHY(why) ((void)0)
#endif

void __cdecl on_place_join(uint32_t esp) {
    uintptr_t f = (uintptr_t)esp + PLACE_EBP_OFF;        // = ebp
    // The ONLY place the game hands an actor its opening transform, and the name it carries is the
    // actor's CHARASPEC -- for a shape-changer, the form the fight is opening in. A fight's actors
    // are all built in one burst, so a gap ends the burst and the counts start over.
    {
        DWORD tnow = GetTickCount();
        if ((int)(tnow - g_openLogAt) > OPEN_LOG_GAP_MS)
            { g_openLogN = 0; g_swapLogN = 0; g_specFixN = 0; }
        g_openLogAt = tnow;
    }
    if (g_openLogN < 8
        && !IsBadReadPtr((void*)(f - PLACE_THIS_OFF), 4)
        && !IsBadReadPtr((void*)(f - PLACE_OUT_OFF), 0x40)) {
        uint32_t who = *(uint32_t*)(f - PLACE_THIS_OFF);
        const float* o = (const float*)(f - PLACE_OUT_OFF);
        char nm[20] = "(?)";
        if (who && !IsBadReadPtr((void*)(uintptr_t)(who + OFF_ACTOR_NAME), 16)) {
            memcpy(nm, (const void*)(uintptr_t)(who + OFF_ACTOR_NAME), 16); nm[16] = 0;
        }
        g_openLogN++;
        ff13logv("[FF13-SRP] battle: actor \"%.16s\" [%08X] opens at %.2f %.2f %.2f%s\n",
                 nm, who, (double)o[12], (double)o[13], (double)o[14],
                 g_bsStarted ? "  (picked fight)" : "");
    }
    // The build binds the actor and installs its spec without telling the scene object, so an
    // object left as m063_x1 still shows that while the actor reads m063; the actor's own name
    // through 0x471140 (mode 0, as its callers pass) re-applies it. +0x1A40 is what the scene
    // names for this actor, so a scene that opens in a variant is left alone. Only objects an
    // enemy swap was seen on are in the table, so pc* are never touched.
    if (g_specFixN < 8 && !IsBadReadPtr((void*)(f - PLACE_THIS_OFF), 4)) {
        uint32_t self2 = *(uint32_t*)(f - PLACE_THIS_OFF);
        char nm2[20] = {0};
        if (self2 && !IsBadReadPtr((void*)(uintptr_t)(self2 + OFF_ACTOR_NAME), 16)) {
            memcpy(nm2, (const void*)(uintptr_t)(self2 + OFF_ACTOR_NAME), 16); nm2[16] = 0;
            for (int i = 0; nm2[i]; i++)
                if ((unsigned char)nm2[i] < 0x20 || (unsigned char)nm2[i] > 0x7E) { nm2[i] = 0; break; }
        }
        const char* left = nm2[0] ? obj_form_left(self2) : NULL;
        if (left && strcmp(left, nm2)) {
            g_specFixN++;
            ff13logv("[FF13-SRP] battle: object %08X was left as \"%s\" -- \"%s\" put back on actor "
                     "%08X%s\n", *(uint32_t*)(uintptr_t)(self2 + OFF_ACTOR_OBJ), left, nm2, self2,
                     g_bsStarted ? "  (picked fight)" : "");
            spec_install(self2, nm2);       // goes through the swap hook, which notes the new name
        }
    }
    if (!g_placeMatrix || !g_placeBoss) return;          // only while we are staging an arena fight
    if (IsBadReadPtr((void*)(f - PLACE_THIS_OFF), 4)) { PJ_WHY("this unreadable"); return; }
    uint32_t self = *(uint32_t*)(f - PLACE_THIS_OFF);
    if (!self || IsBadReadPtr((void*)(uintptr_t)(self + OFF_ACTOR_NAME), 16))
        { PJ_WHY("no name at +0x1A40"); return; }
    if (IsBadWritePtr((void*)(f - PLACE_OUT_OFF), 0x40)) { PJ_WHY("out not writable"); return; }
    float* out = (float*)(f - PLACE_OUT_OFF);
    // The one the lookup could not answer for. A character always has a place, so this is the
    // enemy the fight never registered.
    if (out[12] != 0.0f || out[13] != 0.0f || out[14] != 0.0f) {
        // The fight placed this enemy itself, so the commit hook's leash must come OFF: it adopts
        // an actor that shows at the origin for one frame and then drags it back there every frame,
        // which reads as an enemy that cannot walk (measured). Only the enemies this row names.
        char n2[20];
        memcpy(n2, (const void*)(uintptr_t)(self + OFF_ACTOR_NAME), 16); n2[16] = 0;
        if (g_arena && !g_bossGotMatrix) {
            for (int i = 0; i < ARENA_ENEMIES; i++) {
                const char* en = g_arena->en[i].name;
                if (!en || !en[0]) continue;
                if (strncmp(n2, en, strlen(en))) continue;
                g_bossSlotN = 0;              // nothing of ours is held any more
                g_bossGotMatrix = 1;          // ...and place_boss stands down for this fight
                ff13logv("[FF13-SRP] battle picker: \"%.16s\" placed itself at %.2f %.2f %.2f -- "
                         "the hold is off\n", n2, (double)out[12], (double)out[13],
                         (double)out[14]);
                break;
            }
        }
#ifdef FF13_DIAG
        if (g_placeBoss && g_pjLog < 12) { g_pjLog++;
            ff13logv("[FF13-SRP] placement join: \"%.16s\" already has a place (%.2f %.2f %.2f)\n",
                     n2, (double)out[12], (double)out[13], (double)out[14]); }
#endif
        return;
    }
    char* bm = g_base ? *(char**)(g_base + RVA_BM_PTR) : NULL;
    if (!bm || IsBadReadPtr(bm + OFF_BM_STAGE, 64)) return;
    const float* st = (const float*)(bm + OFF_BM_STAGE);
    float p[3];
    if (!stage_place(g_bossOx, g_bossOy, g_bossOz, p)) return;
    // ...and, where the scene lists several enemies, this actor's own place among them. The name
    // the game prints in its own _init line is the one to match on, and it is right here.
    char nm[20];
    memcpy(nm, (const void*)(uintptr_t)(self + OFF_ACTOR_NAME), 16);
    nm[16] = 0;
    {
        float ox, oy, oz;
        // This is the authoritative path and it shuts the per-frame one off (g_bossGotMatrix below),
        // so the first actor through here starts the hand-out over: claims the commit hook made
        // before the battle got this far are not ours to keep.
        if (!g_bossGotMatrix)
            for (int i = 0; i < ARENA_ENEMIES; i++) g_enUsed[i] = 0;
        arena_enemy_pick(nm, &ox, &oy, &oz);
        if (ox != g_bossOx || oy != g_bossOy || oz != g_bossOz) stage_place(ox, oy, oz, p);
    }
    out[0]  = -st[0]; out[1]  = -st[1]; out[2]  = -st[2];  out[3]  = 0.0f;   // right, turned round
    out[4]  =  st[4]; out[5]  =  st[5]; out[6]  =  st[6];  out[7]  = 0.0f;   // up
    out[8]  = -st[8]; out[9]  = -st[9]; out[10] = -st[10]; out[11] = 0.0f;   // forward, at the party
    out[12] = p[0];   out[13] = p[1];   out[14] = p[2];    out[15] = 1.0f;
    // The commit override is the old way of doing this and must not fight it: an actor that has
    // been given a place keeps it, and dragging its committed position back every frame is what
    // made it look like it was teleporting whenever it moved.
    g_bossGotMatrix = 1;
    // This call only runs while the battle is building its actors, so the stage now standing is THIS
    // fight's -- which is what lets the party be put on it as well. The stage matrix is not cleared
    // between fights, so without recording it a leftover one reads as still current and the party is
    // left where it stood in the field.
    g_stageArena = g_arena ? g_arena->arena : NULL;
    if (g_placeMatN < 8) {
        g_placeMatN++;
        ff13logv("[FF13-SRP] battle picker: \"%.16s\" had no place of its own -- given %.2f %.2f "
                 "%.2f facing %.2f %.2f %.2f\n", nm, p[0], p[1], p[2],
                 (double)out[8], (double)out[9], (double)out[10]);
    }
}

// ---- what a practised fight costs ---------------------------------------------------------------
// Everything the game takes out of the party's bags goes through
//     removeItem(const char id[16], int n)   @ 0xB6A940
// Hooked at the ENTRY, before anything has been taken, so asking the game how many there are right
// then gives the number to come back to. Coming back to a NUMBER rather than adding back what was
// taken is what makes this safe to run twice.
// Item ids are exactly 16 bytes, NUL-padded ("it_potion"), which is both what a bag entry stores and
// what the game builds for a lookup, so an id can be copied straight out of the caller's argument.
// The tally runs from the last give-back until the next picked fight ends, so it covers the Shrouds
// spent walking up to the fight; it is not selective about where an item went.
// TP is not in the bags: getTp() @ 0xB272E0, setTp(int) @ 0xB272F0, both taking the same lock the
// gil and item calls take, both ending at [party+0x6C]. The setter clamps to 0..system_tp_max and
// tells the gauge, which is why it is called rather than the field written. TP cannot be spent
// outside a fight, so it needs no window. Put back, never taken -- a won fight keeps its gain.
const uint8_t P_ITEMTAKE [6] = {0x55,0x8B,0xEC,0x83,0xEC,0x50};
const uint8_t P_ITEMCOUNT[7] = {0x55,0x8B,0xEC,0x8B,0x45,0x08,0x50};
const uint8_t P_ITEMGIVE [7] = {0x55,0x8B,0xEC,0x8B,0x45,0x10,0x50};
typedef int  (__cdecl *itemCount_t)(const char* id);
typedef void (__cdecl *itemGive_t )(const char* id, int n, int flag);
const uint8_t P_TPGET[8]  = {0x55,0x8B,0xEC,0xE8,0x18,0xEA,0x03,0x00};
const uint8_t P_TPSET[12] = {0x55,0x8B,0xEC,0x8B,0x45,0x08,0x50,0xE8,0x44,0xEA,0x03,0x00};
typedef int  (__cdecl *tpGet_t)(void);
typedef void (__cdecl *tpSet_t)(int tp);

// The store is FOUR sheets deep and battle init (0x4D49E0) leaves it alone when the scene's bit 34
// (scene+0x252 & 4, a chain's second half) is set -- so every pick empties it, or the fifth such
// pick runs off its end (crash report, reproduced; CODE_NOTES).
const uint8_t P_BTRES_CLEAR[14] = {0x55,0x8B,0xEC,0x83,0xEC,0x08,0x6A,0x00,0x8B,0x0D,0x60,0xB0,0x82,0x02};
typedef void (__cdecl *btresClear_t)(void);
volatile int g_btresOk = 0;
static int btres_level(void) {
    uintptr_t p = g_base + RVA_BTRESDB_PTR;
    if (!g_base || IsBadReadPtr((void*)p, 4)) return -1;
    uint32_t db = *(uint32_t*)p;
    if (!db || IsBadReadPtr((void*)(uintptr_t)(db + OFF_BTRES_LEVEL), 4)) return -1;
    return *(int*)(uintptr_t)(db + OFF_BTRES_LEVEL);
}
// Game thread, on the field, before the fight is staged: the store's lock asserts it is not held.
static void btres_clear_for_pick(uint32_t btsc) {
    static int saidUnreadable = 0;
    if (!g_btresOk) return;
    int lvl = btres_level();
    if (lvl < 0) {
        if (!saidUnreadable) {
            saidUnreadable = 1;
            ff13loga("[FF13-SRP] battle picker: the battle result store cannot be read -- not emptied, "
                     "so a fifth pick of a chain's second half can still crash the game\n");
        }
        return;
    }
    ((btresClear_t)(uintptr_t)(g_base + RVA_BTRES_CLEAR))();
    if (lvl > 0)
        ff13log("[FF13-SRP] battle picker: the battle result store held %d fight(s) -- emptied for "
                "btsc%05u, as battle init does for any fight that is not a chain's second half\n",
                lvl, btsc);
}

// Libra -- what a fight teaches about the enemies in it -- is the third thing a picked fight leaves
// behind, and the only one that has to be put back DOWNWARDS: nothing spends it, so there is no
// "give back what was taken" to do; the fight simply knows more afterwards.
//
// The store is the Datalog singleton, *(void**)0x0282B1F4 (sizeof 0x45E8). Two arrays, one per enemy
// species, 384 entries each and provably adjacent:
//
//     +0x37CC   u32 mask[384][2]   which properties are open. word0 = props 0-31, word1 = 32-63.
//     +0x43CC   u8  points[384]    UNSPENT points towards the next property
//     +0x45CB   u8  how many species are fully scanned. Kept BY HAND by the mask writer (0xB5E8F0)
//               and NEVER decremented, so putting the masks back without this would leave it
//               permanently high -- and it is what the game counts for a trophy at 100 species.
//               Also used, before anything is written, as a check on the whole layout.
//
// BOTH arrays are needed. The points byte is spent DOWN, not accumulated: the award routine adds the
// points, then subtracts each threshold as it opens a property, and stores the remainder -- so a
// fight can leave the byte LOWER than it found it while the mask has grown.
//
// The index is NOT an enemy id: it is the row of the charaspec name ("m020") in the r_monsbk
// datasheet. Nothing here needs it -- both arrays are copied whole.
//
// Written raw rather than through the game's own setters: the mask setter 0xB5E220 OVERWRITES both
// arguments with -1, so it can only ever raise a mask. The lock those accessors take is taken here
// instead, so nothing is written behind the Datalog's back.
//
// Timing: the persistent store only moves at the END of a battle, so sampling at the pick and
// putting back where the items and TP go back is comfortably either side of the only write.
#define RVA_DATALOG_PTR 0x242B1F4   // VA 0x0282B1F4 -> the Datalog object
#define OFF_LIBRA_MASK  0x37CC
#define OFF_LIBRA_PTS   0x43CC
#define OFF_LIBRA_FULLN 0x45CB      // count of fully-scanned species; see above
#define OFF_LIBRA_LOCK  0x45D0      // the object's own lock; every Datalog accessor takes this one
#define LIBRA_SPECIES   384
// The lock is the engine's own wrapper, not a bare CRITICAL_SECTION: enter takes a source tag and
// asserts the lock is NOT already held (thread.cpp:134), so it must never be taken recursively.
// Both call sites here are on the game thread outside any Datalog call, so it cannot be.
const uint8_t P_LOCKENTER[9] = {0x55,0x8B,0xEC,0x83,0xEC,0x0C,0x89,0x4D,0xF4};
const uint8_t P_LOCKLEAVE[7] = {0x55,0x8B,0xEC,0x51,0x89,0x4D,0xFC};
typedef void (__thiscall *lockEnter_t)(void* lock, int tag);
typedef void (__thiscall *lockLeave_t)(void* lock);

// How long to wait for the field after a picked fight before giving up on putting anything back.
// Giving up only stops the waiting -- the tally is kept, and the next picked fight that does come
// back honours it.
#define REFUND_WAIT_MS 60000
#define ITEMLOG_MAX 64
typedef struct { char id[16]; int back; } ff13_itemtally;
static ff13_itemtally g_itemLog[ITEMLOG_MAX];
static volatile int   g_itemLogN = 0;
volatile int   g_itemOk = 0;          // the read/give pair is present (the hook is separate)
static volatile int   g_refundItems = 1;     // [battle_swap] refund_items
static volatile DWORD g_refundUntil = 0;     // waiting for the field until this tick (0 = not waiting)
volatile int   g_tpOk = 0;            // the TP pair is present
static volatile int   g_refundTp = 1;        // [battle_swap] refund_tp
static volatile int   g_tpBefore = -1;       // TP as the picked fight started (-1 = nothing to put back)

static int item_count_now(const char* id) {
    return ((itemCount_t)(uintptr_t)(g_base + RVA_ITEMCOUNT))(id);
}

// __cdecl handler: esp is the caller's stack pointer at removeItem's first instruction, so
// [esp+4] = the id and [esp+8] = how many are going.
void __cdecl on_item_take(uint32_t esp) {
    if (!g_refundItems || !g_itemOk) return;
    if (IsBadReadPtr((void*)(uintptr_t)(esp + 4), 8)) return;
    const char* id = *(const char* const*)(uintptr_t)(esp + 4);
    int n = *(const int*)(uintptr_t)(esp + 8);
    if (!id || n <= 0 || IsBadReadPtr((void*)id, 16)) return;
    for (int i = 0; i < g_itemLogN; i++)
        if (memcmp(g_itemLog[i].id, id, 16) == 0) return;   // the first tally is the one worth keeping
    if (g_itemLogN >= ITEMLOG_MAX) return;
    int have = item_count_now(id);                          // nothing has been taken yet
    if (have <= 0) return;
    memcpy(g_itemLog[g_itemLogN].id, id, 16);
    g_itemLog[g_itemLogN].back = have;
    g_itemLogN++;
}

static void items_give_back(void) {
    int n = g_itemLogN;
    g_itemLogN = 0;                                         // cleared first: a give must not re-tally
    for (int i = 0; i < n; i++) {
        int have = item_count_now(g_itemLog[i].id);
        int missing = g_itemLog[i].back - have;
        if (missing <= 0) continue;
        ((itemGive_t)(uintptr_t)(g_base + RVA_ITEMGIVE))(g_itemLog[i].id, missing, 1);
        ff13logv("[FF13-SRP] battle picker: %.16s back to %d (+%d)\n",
                 g_itemLog[i].id, g_itemLog[i].back, missing);
    }
}

static uint32_t g_libraMask[LIBRA_SPECIES][2];
static uint8_t  g_libraPts[LIBRA_SPECIES];
static volatile int g_libraFullN = 0;   // the game's own fully-scanned count, put back with them
volatile int g_libraOk = 0;      // the lock pair is where it should be
static volatile int g_libraHave = 0;

static uint8_t* libra_datalog(void) {
    if (!g_libraOk) return NULL;
    uint8_t* dl = *(uint8_t**)(uintptr_t)(g_base + RVA_DATALOG_PTR);
    if (!dl || IsBadReadPtr(dl, 0x45E8)) return NULL;
    return dl;
}

static void libra_sample(void) {
    // The OLDEST pending snapshot is the one to keep: a second fight can be picked before the first
    // one's put-back has fired, and re-sampling there would take the first fight's gain as the state
    // to come back to -- putting back to a point AFTER the thing being put back.
    if (g_libraHave && g_refundUntil) return;
    g_libraHave = 0;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    memcpy(g_libraMask, dl + OFF_LIBRA_MASK, sizeof g_libraMask);
    memcpy(g_libraPts,  dl + OFF_LIBRA_PTS,  sizeof g_libraPts);
    g_libraFullN = dl[OFF_LIBRA_FULLN];
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    // Everything above is read-only, so this is the last moment before the mod would start WRITING
    // 3456 bytes into a structure the game serialises into the save. +0x45CB is the game's own
    // independent witness to where the mask array is, so it must equal the number of -1,-1 masks we
    // can see. If it does not, the layout is not what this code thinks it is: touch nothing.
    int counted = 0;
    for (int i = 0; i < LIBRA_SPECIES; i++)
        if (g_libraMask[i][0] == 0xFFFFFFFFu && g_libraMask[i][1] == 0xFFFFFFFFu) counted++;
    if (counted != g_libraFullN) {
        g_libraOk = 0;
        ff13loga("[FF13-SRP] battle picker: Libra layout check FAILED -- %d fully-scanned species "
                 "in the mask array at +0x%X, but the game's own count at +0x%X says %d. Nothing "
                 "will be written; what picked fights teach stays learned.\n",
                 counted, OFF_LIBRA_MASK, OFF_LIBRA_FULLN, g_libraFullN);
        return;
    }
    g_libraHave = 1;
}

// ---- fights the story reaches with the enemy never scanned ------------------------------------
// For a fight whose only place to stand and pick from is PAST it: the story meets these enemies
// unscanned and an ordinary pick would not. Zeroed at the pick, because each species' record is
// filled FROM the store as the fight is set up (0x4C40D0). The ordinary put-back restores it -- a
// lowering the snapshot already covers -- and libra_zero_undo is only for the paths that miss it.
static const uint16_t kLibraZero[] = { 3001 };
#define LIBRA_ZERO_N ((int)(sizeof(kLibraZero)/sizeof(kLibraZero[0])))
#define LIBRA_ZERO_MAX 4
// VA 0xB5D410: charaspec name -> its row in the r_monsbk datasheet, -1 for a species with no row.
// The row IS the index into both Datalog arrays, so nothing here has to know a species number.
// The argument is a FIXED 16-BYTE KEY, not a C string: the game's own callers hand it the field at
// spec+8, built by 0x4112B0 (16 bytes zeroed, up to 16 chars copied, no terminator). A pointer to a
// 5-byte literal answered -1 for a species that is in the sheet (measured).
#define RVA_MONSBK_ROW 0x75D410
#define MONSBK_KEY_LEN 16
typedef int (__cdecl *monsbkRow_t)(const char* key16);
// Bits 51..55 of the mask are the five scan stages (openLibraProp(51+n)); everything else is one
// property each. Counting them is what "Libra value" means on screen.
#define LIBRA_STAGE_BITS(m1) (((m1) >> (51 - 32)) & 0x1Fu)

static struct { uint16_t idx; uint32_t mask[2]; uint8_t pts; } g_libraZeroRows[LIBRA_ZERO_MAX];
static volatile int g_libraZeroN = 0;

static int librazero_wanted(uint32_t btsc) {
    for (int i = 0; i < LIBRA_ZERO_N; i++) if (kLibraZero[i] == (uint16_t)btsc) return 1;
    return 0;
}

static int stage_count(uint32_t m1) {
    unsigned b = LIBRA_STAGE_BITS(m1), n = 0;
    while (b) { n += b & 1u; b >>= 1; }
    return (int)n;
}

// The lookup pushes its own sheet name as its first argument, so the check is that constant rather
// than a byte pattern: base-independent, and a different function there cannot pass it.
static int libra_row_ok(void) {
    const uint8_t* fn = (const uint8_t*)(uintptr_t)(g_base + RVA_MONSBK_ROW);
    if (IsBadReadPtr((void*)fn, 11)) return 0;
    if (fn[0] != 0x55 || fn[1] != 0x8B || fn[2] != 0xEC || fn[6] != 0x68) return 0;
    const char* sheet = *(const char* const*)(fn + 7);
    return sheet && !IsBadReadPtr((void*)sheet, 9) && memcmp(sheet, "r_monsbk", 9) == 0;
}

static void libra_zero_undo(const char* why);

static void libra_zero_take(uint32_t btsc) {
    libra_zero_undo("another fight was picked");   // an outstanding one goes back before a new one
    if (!librazero_wanted(btsc)) return;
    if (!g_libraHave) {
        ff13loga("[FF13-SRP] battle picker: btsc%05u wants its enemies unscanned, but there is no "
                 "Libra snapshot to put back afterwards -- left alone\n", btsc);
        return;
    }
    if (!libra_row_ok()) {
        ff13loga("[FF13-SRP] battle picker: btsc%05u wants its enemies unscanned, but the r_monsbk "
                 "lookup is not at 0x%08X -- left alone\n", btsc, (unsigned)(g_base + RVA_MONSBK_ROW));
        return;
    }
    const ff13_scene_models* sm = scene_models(btsc);
    if (!sm || !sm->n) {
        ff13loga("[FF13-SRP] battle picker: btsc%05u wants its enemies unscanned, but the scene "
                 "lists no models -- left alone\n", btsc);
        return;
    }
    const char* spec[LIBRA_ZERO_MAX];
    int row[LIBRA_ZERO_MAX], rn = 0;
    for (int i = 0; i < sm->n && rn < LIBRA_ZERO_MAX; i++) {
        const char* nm = kModelName[kModelIdx[sm->first + i]];
        char key[MONSBK_KEY_LEN];
        size_t kl = strlen(nm);
        if (kl > sizeof key) kl = sizeof key;
        memset(key, 0, sizeof key);
        memcpy(key, nm, kl);
        int r = ((monsbkRow_t)(uintptr_t)(g_base + RVA_MONSBK_ROW))(key);
        if (r < 0 || r >= LIBRA_SPECIES) {
            // The key goes out as bytes: a -1 cannot otherwise separate "not in the sheet" from
            // a key that is not shaped the way the sheet is read.
            char hex[MONSBK_KEY_LEN * 2 + 1];
            for (int b = 0; b < MONSBK_KEY_LEN; b++)
                sprintf(hex + b * 2, "%02X", (unsigned char)key[b]);
            ff13logv("[FF13-SRP] battle picker: btsc%05u enemy \"%s\" has no r_monsbk row (%d, key "
                     "%s) -- nothing to unscan for it\n", btsc, nm, r, hex);
            continue;
        }
        spec[rn] = nm; row[rn] = r; rn++;
    }
    if (!rn) return;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    uint32_t* mask = (uint32_t*)(dl + OFF_LIBRA_MASK);
    uint8_t*  pts  = (uint8_t*)(dl + OFF_LIBRA_PTS);
    int n = 0, full = 0;
    for (int k = 0; k < rn; k++) {
        int i = row[k];
        g_libraZeroRows[n].idx     = (uint16_t)i;
        g_libraZeroRows[n].mask[0] = mask[i*2];
        g_libraZeroRows[n].mask[1] = mask[i*2+1];
        g_libraZeroRows[n].pts     = pts[i];
        n++;
        if (mask[i*2] == 0xFFFFFFFFu && mask[i*2+1] == 0xFFFFFFFFu) full++;
        ff13log("[FF13-SRP] battle picker: btsc%05u \"%s\" (r_monsbk row %d) unscanned -- mask "
                "%08X %08X -> 00000000 00000000 (%d of 5 stages -> 0), points %u -> 0\n",
                btsc, spec[k], i, mask[i*2], mask[i*2+1], stage_count(mask[i*2+1]),
                (unsigned)pts[i]);
        mask[i*2] = 0; mask[i*2+1] = 0; pts[i] = 0;
    }
    // The fully-scanned count is kept by hand by the game's own mask writer and never decremented by
    // it, so a species dropped out of it has to be taken off here or the count reads high.
    if (full) dl[OFF_LIBRA_FULLN] = (uint8_t)(dl[OFF_LIBRA_FULLN] > full
                                              ? dl[OFF_LIBRA_FULLN] - full : 0);
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    g_libraZeroN = n;
}

// Only ever RAISES: the rows are OR'd back and the points taken as the higher of the two, so it is
// safe on a stale record -- unlike the general put-back, which is a lowering and is dropped when the
// field does not come back. After a put-back that already restored the snapshot it is a no-op.
static void libra_zero_undo(const char* why) {
    int n = g_libraZeroN;
    g_libraZeroN = 0;
    if (!n) return;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    uint32_t* mask = (uint32_t*)(dl + OFF_LIBRA_MASK);
    uint8_t*  pts  = (uint8_t*)(dl + OFF_LIBRA_PTS);
    int back = 0, full = 0;
    for (int k = 0; k < n; k++) {
        int i = g_libraZeroRows[k].idx;
        if (i < 0 || i >= LIBRA_SPECIES) continue;
        uint32_t m0 = mask[i*2] | g_libraZeroRows[k].mask[0];
        uint32_t m1 = mask[i*2+1] | g_libraZeroRows[k].mask[1];
        int wasFull = (mask[i*2] == 0xFFFFFFFFu && mask[i*2+1] == 0xFFFFFFFFu);
        if (m0 != mask[i*2] || m1 != mask[i*2+1] || pts[i] < g_libraZeroRows[k].pts) back++;
        mask[i*2] = m0; mask[i*2+1] = m1;
        if (pts[i] < g_libraZeroRows[k].pts) pts[i] = g_libraZeroRows[k].pts;
        if (!wasFull && m0 == 0xFFFFFFFFu && m1 == 0xFFFFFFFFu) full++;
    }
    if (full) dl[OFF_LIBRA_FULLN] = (uint8_t)(dl[OFF_LIBRA_FULLN] + full);
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    if (back)
        ff13log("[FF13-SRP] battle picker: what was unscanned for the pick is back for %d "
                "species (%s)\n", back, why);
}

#ifdef FF13_DIAG
// What a fight actually WROTE into the Datalog, by species index: the put-back is a net effect and
// says nothing about which species moved, which is the question when two fights field what looks
// like the same enemy (the game keeps four separate monster_book entries for m019). Taken
// independently of the snapshot the put-back uses, so it works for a pick that keeps its scan too.
static uint32_t g_libraDiagMask[LIBRA_SPECIES][2];
static uint8_t  g_libraDiagPts[LIBRA_SPECIES];
static volatile int g_libraDiagHave = 0;
static volatile uint32_t g_libraDiagFor = 0;
static void libra_diag_before(uint32_t btsc) {
    g_libraDiagHave = 0;
    g_libraDiagFor = btsc;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    memcpy(g_libraDiagMask, dl + OFF_LIBRA_MASK, sizeof g_libraDiagMask);
    memcpy(g_libraDiagPts,  dl + OFF_LIBRA_PTS,  sizeof g_libraDiagPts);
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    g_libraDiagHave = 1;
}
// Compared against the snapshot WITHOUT consuming it, so the same fight can be measured twice: once
// as the fight ends and once after the way back. That is what tells "the restart took the scan"
// apart from "we did".
static void libra_diag_point(const char* when, int last) {
    if (!g_libraDiagHave) return;
    uint32_t btsc = g_libraDiagFor;
    if (last) g_libraDiagHave = 0;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    const uint32_t* mask = (const uint32_t*)(dl + OFF_LIBRA_MASK);
    const uint8_t*  pts  = (const uint8_t*)(dl + OFF_LIBRA_PTS);
    int said = 0;
    for (int i = 0; i < LIBRA_SPECIES; i++) {
        if (mask[i*2] == g_libraDiagMask[i][0] && mask[i*2+1] == g_libraDiagMask[i][1]
            && pts[i] == g_libraDiagPts[i]) continue;
        ff13logv("[FF13-SRP] libra (%s): btsc%05u species index %d -- mask %08X %08X -> %08X %08X,"
                 " points %u -> %u\n", when, btsc, i, g_libraDiagMask[i][0], g_libraDiagMask[i][1],
                 mask[i*2], mask[i*2+1], (unsigned)g_libraDiagPts[i], (unsigned)pts[i]);
        said++;
    }
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    if (!said) ff13logv("[FF13-SRP] libra (%s): btsc%05u -- nothing differs from before the fight\n",
                        when, btsc);
}
#endif

// What a kept fight TAUGHT, held across the way back and written again on the far side.
//
// Leaving the scan alone is not enough, because the restart does not leave it alone: whether it
// survives depends on how much the checkpoint reloads (a full 77/1/2/3 reload restores the Datalog
// from the save; an in-place one keeps it). Only the species the fight actually moved are written,
// so nothing else the restart restored is disturbed.
#define LIBRA_KEEP_MAX 8
static struct { uint16_t idx; uint32_t mask[2]; uint8_t pts; } g_libraKeepRows[LIBRA_KEEP_MAX];
static volatile int g_libraKeepN = 0;

static void libra_keep_take(uint32_t btsc) {
    g_libraKeepN = 0;
    if (!g_libraHave) return;                  // nothing to measure the gain against
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    const uint32_t* mask = (const uint32_t*)(dl + OFF_LIBRA_MASK);
    const uint8_t*  pts  = (const uint8_t*)(dl + OFF_LIBRA_PTS);
    int n = 0, over = 0;
    for (int i = 0; i < LIBRA_SPECIES; i++) {
        if (mask[i*2] == g_libraMask[i][0] && mask[i*2+1] == g_libraMask[i][1]
            && pts[i] == g_libraPts[i]) continue;
        if (n >= LIBRA_KEEP_MAX) { over++; continue; }
        g_libraKeepRows[n].idx     = (uint16_t)i;
        g_libraKeepRows[n].mask[0] = mask[i*2];
        g_libraKeepRows[n].mask[1] = mask[i*2+1];
        g_libraKeepRows[n].pts     = pts[i];
        n++;
    }
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    g_libraKeepN = n;
    g_libraHave = 0;              // the put-back must not undo what this is about to keep
    if (n)
        ff13logv("[FF13-SRP] battle picker: btsc%05u keeps what it scanned -- %d species held "
                 "across the way back%s\n", btsc, n,
                 over ? " (and more than this build can hold were dropped)" : "");
}

static void libra_keep_apply(void) {
    int n = g_libraKeepN;
    g_libraKeepN = 0;
    if (!n) return;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    uint32_t* mask = (uint32_t*)(dl + OFF_LIBRA_MASK);
    uint8_t*  pts  = (uint8_t*)(dl + OFF_LIBRA_PTS);
    int changed = 0, gainedFull = 0;
    for (int k = 0; k < n; k++) {
        int i = g_libraKeepRows[k].idx;
        if (i < 0 || i >= LIBRA_SPECIES) continue;
        if (mask[i*2] == g_libraKeepRows[k].mask[0] && mask[i*2+1] == g_libraKeepRows[k].mask[1]
            && pts[i] == g_libraKeepRows[k].pts) continue;             // the restart kept it
        int wasFull = (mask[i*2] == 0xFFFFFFFFu && mask[i*2+1] == 0xFFFFFFFFu);
        int nowFull = (g_libraKeepRows[k].mask[0] == 0xFFFFFFFFu
                       && g_libraKeepRows[k].mask[1] == 0xFFFFFFFFu);
        mask[i*2]   = g_libraKeepRows[k].mask[0];
        mask[i*2+1] = g_libraKeepRows[k].mask[1];
        pts[i]      = g_libraKeepRows[k].pts;
        changed++;
        if (!wasFull && nowFull) gainedFull++;
    }
    // Added to what the restart left rather than written from a remembered total: the count is the
    // game's own and the restart has just set it to whatever the save says.
    if (gainedFull) dl[OFF_LIBRA_FULLN] = (uint8_t)(dl[OFF_LIBRA_FULLN] + gainedFull);
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    if (changed)
        ff13logv("[FF13-SRP] battle picker: Libra put back for %d species the restart undid%s\n",
                 changed, gainedFull ? " (and the fully-scanned count with them)" : "");
}

static void libra_put_back(void) {
    if (!g_libraHave) return;
    g_libraHave = 0;
    uint8_t* dl = libra_datalog();
    if (!dl) return;
    ((lockEnter_t)(uintptr_t)(g_base + RVA_LOCK_ENTER))(dl + OFF_LIBRA_LOCK, 0);
    uint32_t* mask = (uint32_t*)(dl + OFF_LIBRA_MASK);
    uint8_t*  pts  = (uint8_t*)(dl + OFF_LIBRA_PTS);
    int changed = 0;
    for (int i = 0; i < LIBRA_SPECIES; i++) {
        if (mask[i*2] == g_libraMask[i][0] && mask[i*2+1] == g_libraMask[i][1]
            && pts[i] == g_libraPts[i]) continue;
        mask[i*2]   = g_libraMask[i][0];
        mask[i*2+1] = g_libraMask[i][1];
        pts[i]      = g_libraPts[i];
        changed++;
    }
    dl[OFF_LIBRA_FULLN] = (uint8_t)g_libraFullN;   // never decremented by the game; see above
    ((lockLeave_t)(uintptr_t)(g_base + RVA_LOCK_LEAVE))(dl + OFF_LIBRA_LOCK);
    if (changed)
        ff13log("[FF13-SRP] battle picker: Libra back to what it was for %d species\n", changed);
}

static void tp_sample(void) {
    g_tpBefore = (g_refundTp && g_tpOk) ? ((tpGet_t)(uintptr_t)(g_base + RVA_TPGET))() : -1;
}

static void tp_give_back(void) {
    int want = g_tpBefore;
    g_tpBefore = -1;
    if (want < 0 || !g_refundTp || !g_tpOk) return;
    int have = ((tpGet_t)(uintptr_t)(g_base + RVA_TPGET))();
    if (have >= want) return;                    // a fight won can leave more than it started with
    ((tpSet_t)(uintptr_t)(g_base + RVA_TPSET))(want);
    ff13log("[FF13-SRP] battle picker: TP back to %d (was %d)\n", want, have);
}

// The field has to be back AND settled: writing the party's position into a zone that is still being
// rebuilt is a write the rebuild overwrites, and the restart passes through FIELD_IDLE before it is
// finished with it.
// There are two restarts and they are not the same thing: a full reload walks the zone state
// 77 -> 1 -> 2 -> 3 and re-runs the zone's script, and an in-place one never leaves 3 and rebuilds
// nothing -- no object on/off, no loadBg.
// ---- the loading indicator, and how long it is really up -------------------------------------
// The indicator stays up longer than any one zone state does, so measure the indicator itself. The
// game has exactly one implementation of each end:
//
//   0x8E2080(mode)  raises it. Called from the zone state machine 0x80F340 (0x80F58E), the encounter
//                   entry 0x812140, and -- the one that matters here -- the battle-over worker
//                   0x814B90 at 0x814FD4, which is what puts it up on the way back.
//   0x8E2150()      lowers it. Called from 0x80F340 twice (0x80F573, 0x80F598) among others.
//
// The VM natives Window.showNowLoading / hideNowLoading (0xBEA060 / 0xBEA080) are four-instruction
// wrappers around these two, so hooking the implementation catches a script's calls as well as the
// engine's. Read-only, one line at each end. Raises while it is already up are not counted.
const uint8_t P_LOADSHOW[6]  = {0x55,0x8B,0xEC,0x83,0xEC,0x08};
// Four bytes is short of the five a jmp needs, so the copy takes the mov that follows as well.
const uint8_t P_LOADHIDE[10] = {0x55,0x8B,0xEC,0x51,0x8B,0x0D,0x4C,0x6A,0x82,0x02};
static volatile DWORD g_loadUpAt = 0;
static volatile int   g_loadUp = 0;
// ...and when it went down: a pick taken seconds after a load lands while the game's own loader is
// still finishing (CODE_NOTES).
static volatile DWORD g_loadDownAt = 0;

void __cdecl on_load_show(uint32_t mode) {
    if (g_loadUp) return;
    g_loadUp = 1; g_loadUpAt = GetTickCount();
    ff13logv("[FF13-SRP] loading indicator: up (mode %u, zone state %d)\n",
             (unsigned)mode, field_zone_state());
}

void __cdecl on_load_hide(uint32_t ecx) {
    (void)ecx;
    if (!g_loadUp) return;
    g_loadUp = 0;
    g_loadDownAt = GetTickCount();
    ff13logv("[FF13-SRP] loading indicator: down after %lums (zone state %d)\n",
             (unsigned long)(GetTickCount() - g_loadUpAt), field_zone_state());
}

// A spinner that turns for four seconds and one four-second freeze add up to the same total and are
// not the same fault. This says which: the frame tick runs every frame, so a gap in it is the game
// not getting to a frame.
// ...and a stall must not be allowed to spend a deadline that covers a frame-driven build. The swap
// window is wall clock; what it has to cover is an encounter being BUILT, which is frames. Hardware
// showed them come apart -- the game thread stalled for most of half a minute in the middle of one
// build, the window expired inside the gap, and startBattle came back unsubstituted.
// So a deadline that was still live when the stall began is pushed out by the stall's length. Only
// the live ones: a window that had already closed must stay closed, or a stall minutes later would
// reopen it over an ordinary walked-into encounter.
static void gap_extend(volatile DWORD* deadline, DWORD now, DWORD gap) {
    if (!*deadline) return;
    if ((int)(*deadline - (now - gap)) <= 0) return;      // was already closed before the stall
    *deadline += gap;
}

static void frame_gap_watch(DWORD now) {
    static DWORD last = 0;
    DWORD gap = last ? (DWORD)(now - last) : 0;
    last = now;
    if (gap < 250) return;
    gap_extend(&g_swapWindowUntil, now, gap);
    ff13logv("[FF13-SRP] frame gap: %lums (zone state %d, loading indicator %s)\n",
             (unsigned long)gap, field_zone_state(), g_loadUp ? "up" : "down");
}

// ---- why a pick sometimes starts nothing ------------------------------------------------------
// A picked fight that never becomes a fight leaves the event lock on, which takes the buttons away
// while the party can still walk. doEncountCheck (0x5CEE00) refuses before it ever looks at the
// touched id, on three conditions:
//
//   0x5CF7F0()   bit 1 of the field manager's flags at [0x27FF124]+0x14660
//   0x5CEA30()   bit 6 of the same array
//   [[0x281C9BC]+0x36C]+0x234 == 0
//
// -- if any of them says no it zeroes the id and returns. So when a pick has not produced an
// encounter after a moment, say which one refused.
#define RVA_FIELDMGR   0x23FF124   // VA 0x27FF124
#define OFF_FIELD_BITS 0x14660
#define RVA_ENCGATE3   0x241C9BC   // VA 0x281C9BC -- the third of doEncountCheck's three refusals
#define TOUCH_STALL_MS 2500        // a touch that has not built an encounter by now is refused

// The three of them, READ rather than called: the functions take the array's spinlock at +0x14668,
// peeking at the word does not, and a torn read here costs a wrong log line and nothing else.
// *ok says whether *val was read at all -- NOT a sentinel value, because -1 is a value this field
// takes and a sentinel made the one line that mattered readable either way.
static void encount_gates(uint32_t* bits, uint32_t* val, int* ok) {
    *bits = 0; *val = 0; *ok = 0;
    uintptr_t f = g_base + RVA_FIELDMGR;
    if (!IsBadReadPtr((void*)f, 4)) {
        uint32_t mgr = *(uint32_t*)f;
        if (mgr && !IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_FIELD_BITS), 4))
            *bits = *(uint32_t*)(uintptr_t)(mgr + OFF_FIELD_BITS);
    }
    uintptr_t s = g_base + RVA_ENCGATE3;
    if (!IsBadReadPtr((void*)s, 4)) {
        uint32_t scene = *(uint32_t*)s;
        if (scene && !IsBadReadPtr((void*)(uintptr_t)(scene + 0x36C), 4)) {
            uint32_t sub = *(uint32_t*)(uintptr_t)(scene + 0x36C);
            if (sub && !IsBadReadPtr((void*)(uintptr_t)(sub + 0x234), 4)) {
                *val = *(uint32_t*)(uintptr_t)(sub + 0x234);
                *ok = 1;
            }
        }
    }
}

static int async_loader_type(void);
// What a pick armed for the fight it was about to become. The arming is keyed on the SCENE ID and
// nothing else, so a pick that never becomes a fight must disarm it here: left standing it fires on
// that same fight reached in the STORY, hours later.
static void staging_disarm(void) {
    g_decksRestoreFor = 0;
    g_decksWantFor = 0; g_decksWantN = 0; g_decksWant = NULL;
    g_decksAgainAt = 0;
    g_statFor = 0; g_statWant = NULL; g_statBase = -1;
}

// The fight's one player character, or 0 when there is not exactly one. Found by charaspec NAME:
// PC actors are "pcNNN", enemies "mNNN". The side bits do NOT separate them this early, and the
// number is not the party's own character id (Snow is "pc001", measured).
#define STAT_ACTOR_SPAN (OFF_ACTOR_STATS + ACTOR_STAT_LAYER * 0x100 + 0x40 * 4)
static uint32_t lone_pc_actor(void) {
    uint32_t* tbl = actor_table();
    if (!tbl) return 0;
    uint32_t found = 0;
    char nm[SPEC_NAME_LEN + 1];
    for (int i = 0; i < ACTOR_SLOTS; i++) {
        uint32_t a = tbl[i];
        if (!a || IsBadReadPtr((void*)(uintptr_t)a, STAT_ACTOR_SPAN)) continue;
        if (!actor_name_of(a, nm) || nm[0] != 'p' || nm[1] != 'c') continue;
        if (found) return 0;                    // more than one: the row does not say which
        found = a;
    }
    return found;
}

// "pcNNN" -> the party's own character id. NNN is the MODEL index minus one (the game's own
// mot/pc/sk_c001_lt .. sk_c006_fa), and the character ids run in a different order, so the two
// numbers are NOT the same -- Snow is "pc001" and character 2. -1 = not one of the six.
static int pc_actor_charid(const char* nm) {
    static const signed char kPcToChar[6] = { 0, 2, 1, 3, 4, 5 };
    if (!nm || nm[0] != 'p' || nm[1] != 'c') return -1;
    for (int i = 2; i < 5; i++) if (nm[i] < '0' || nm[i] > '9') return -1;
    int n = (nm[2] - '0') * 100 + (nm[3] - '0') * 10 + (nm[4] - '0');
    return (n >= 0 && n < 6) ? kPcToChar[n] : -1;
}

// The actor holds a status as five layers: layer 0 is the character's own figure, layer 4 is what
// getStatus reads, and the game rebuilds 4 from 0 during a fight (measured), so BOTH are written.
// Layer 0 is also the only base there is -- the array Party.getStatus names (roster record + 0xC88)
// reads 0 for all six in ordinary play -- and the `setup` gate at the pick is what keeps it honest.
static int stat_layer(uint32_t a, int layer) {
    return *(int*)(uintptr_t)(a + OFF_ACTOR_STATS + (uintptr_t)layer * 0x100
                              + (uintptr_t)g_statWant->stat * 4);
}
static void stat_layer_set(uint32_t a, int layer, int v) {
    *(int*)(uintptr_t)(a + OFF_ACTOR_STATS + (uintptr_t)layer * 0x100
                       + (uintptr_t)g_statWant->stat * 4) = v;
}
// Take the raise back out of the actor. Called when the fight RESOLVES, not when the battle state
// ends: the game writes the fight's figures down somewhere between those two moments, and undoing at
// the end left the next pick starting from the raised figure (measured).
static void stat_undo(const char* when) {
    if (g_statDone || g_statBase < 0 || !g_statActor || !g_statWant) return;
    g_statDone = 1;
    int want = g_statBase + g_statWant->add, n = 0;
    if (!IsBadReadPtr((void*)(uintptr_t)g_statActor, STAT_ACTOR_SPAN))
        for (int L = 0; L <= ACTOR_STAT_LAYER; L += ACTOR_STAT_LAYER)
            if (stat_layer(g_statActor, L) == want) {
                stat_layer_set(g_statActor, L, g_statBase); n++;
            }
    ff13logv("[FF13-SRP] battle picker: status %d back to %d at \"%s\" (%d of 2 layer(s) still had "
             "%d)\n", g_statWant->stat, g_statBase, when, n, want);
}

static void stat_pump(void) {
    if (!g_statWant || !g_statFor || g_statDone) return;
    uint32_t a = lone_pc_actor();
    if (!a) return;
    if (g_statBase >= 0 && battle_resolved()) { stat_undo("the fight resolved"); return; }
    if (g_statBase < 0) {
        // Wait for the actor to be BUILT: a base captured from a still-zero slot would hold the
        // character at the raise alone for the whole fight.
        if (stat_layer(a, ACTOR_STAT_LAYER) <= 0 || *(int*)(uintptr_t)(a + OFF_MAXHP) <= 0) return;
        char nm[SPEC_NAME_LEN + 1];
        if (!actor_name_of(a, nm)) nm[0] = 0;
        int base = stat_layer(a, 0);        // the actor's own figure, which is the story's by now
        if (base <= 0) return;
        g_statBase = base;
        g_statActor = a;
        ff13logv("[FF13-SRP] battle picker: \"%s\" (char %d) status %d %d -> %d (shown %d -> %d) "
                 "for btsc%05u  [layers %d %d %d %d %d]\n", nm[0] ? nm : "?", pc_actor_charid(nm),
                 g_statWant->stat, base, base + g_statWant->add, base / 100,
                 (base + g_statWant->add) / 100, g_statFor, stat_layer(a, 0), stat_layer(a, 1),
                 stat_layer(a, 2), stat_layer(a, 3), stat_layer(a, 4));
        stat_layer_set(a, ACTOR_STAT_LAYER, base + g_statWant->add);
        stat_layer_set(a, 0, base + g_statWant->add);
        return;
    }
    int want = g_statBase + g_statWant->add;
    int had4 = stat_layer(a, ACTOR_STAT_LAYER), had0 = stat_layer(a, 0);
    if (had4 == want && had0 == want) return;
    stat_layer_set(a, ACTOR_STAT_LAYER, want);
    stat_layer_set(a, 0, want);
    if (g_statAgain++ == 0)
        ff13logv("[FF13-SRP] battle picker: status %d read %d (layer 0 %d) -- put at %d again "
                 "(further times are counted, not logged)\n", g_statWant->stat, had4, had0, want);
}

// The actor OUTLIVES the fight: two picks in a row came up on the same actor object (measured), and
// anything left on it follows the party into an ordinary encounter as well.
static void stat_battle_over(void) {
    if (!g_statWant || !g_statFor) { g_statFor = 0; g_statWant = NULL; g_statBase = -1; return; }
    int want = g_statBase + g_statWant->add;
    stat_undo("the fight ended");        // backstop: normally the resolve already did it
    ff13logv("[FF13-SRP] battle picker: btsc%05u was fought at status %d %d (its own %d), put back "
             "%d time(s)%s\n", g_statFor, g_statWant->stat, g_statBase < 0 ? -1 : want,
             g_statBase, g_statAgain,
             g_statBase < 0 ? "  <- NEVER APPLIED: no single \"pc*\" actor was found" : "");
    g_statFor = 0; g_statWant = NULL; g_statBase = -1; g_statAgain = 0; g_statActor = 0;
    g_statDone = 0;
}

static void touch_stall_probe(DWORD now) {
    if (!g_touchAt) return;
    if (overlay_in_battle()) { g_touchAt = 0; return; }   // a fight is up; nothing stalled
    if (g_encIdSeq != g_touchSeq) { g_touchAt = 0; return; }   // it built one; nothing to explain
    if ((int)(now - (g_touchAt + TOUCH_STALL_MS)) < 0) return;
    g_touchAt = 0;
    uint32_t bits, val; int ok;
    encount_gates(&bits, &val, &ok);
    ff13loga("[FF13-SRP] battle picker: the touch built no encounter in %dms -- field bits %08X "
             "(bit1 %s, bit6 %s), [+0x234] = %s%08X (needs 0)%s\n",
             TOUCH_STALL_MS, bits, (bits & 0x02) ? "set" : "CLEAR -> refused",
             (bits & 0x40) ? "set" : "CLEAR -> refused", ok ? "" : "unreadable, last ", val,
             (ok && val) ? " -> refused" : "");
    // A nonzero [+0x234] flickers several times a second in ordinary play, so one that outlasts the
    // whole stall window is wedged. Only a frozen AsyncLoader ever produced that (cs_restore_watch
    // prevents it), so this is a last-resort net: fold the staging and give the field back rather
    // than hold the lock for the long give-up.
    // Do NOT write 0 into the slot: measured, that un-wedges the touch gate over an inconsistent
    // staging and the re-accepted touch hangs the zone in state 5.
    if (ok && val && g_bsStarted) {
        cam_disarm();
        flash_say(ui_text(T_PICK_NEVER_STARTED));
        ff13loga("[FF13-SRP] battle picker: the pending-touch slot is wedged (loader frozen? "
                 "type %d) -- giving the field back; reload the save if fights stay "
                 "unstartable\n", async_loader_type());
        g_bsStarted = 0;
        g_csRestore[0] = 0;
        staging_disarm();
        g_swapEnabled = 0; g_swapBtscId = 0;   // a re-touch must not half-start the picked fight
        ctl_unlock();
        evquiet_lift();
    }
}

// ---- ...and the refusal itself, which is the only moment worth reading them at -----------------
// Do NOT sample the three gates on a timer: bit 6 and [+0x234] flicker several times a second in
// ordinary play, so they are momentary state (the field being ready for a touch this frame), not a
// switch that gets left off. A sampler on that produces noise.
//
// doEncountCheck has the exact point instead -- reached only when an id WAS pending and the gates
// said no:
//
//   005CEE94  cmp [ebp-0x1c], 0        ; the touched id
//   005CEE98  jl  0x5CEF2B             ; nothing pending -> ordinary quiet return
//   005CEE9E  cmp [ebp+8], 0           ; the gates' verdict
//   005CEEA2  jne 0x5CEEB5             ; allowed -> build the encounter
//   005CEEA4  push "[Encount] fetchTouchedObjectNoLock in doEncountCheck"   <- HERE, five bytes
//
// The gates are read again in the handler: it is the same frame, a few instructions later, so they
// are what the decision was made on.
const uint8_t P_ENCREFUSE[5] = {0x68,0x08,0xDC,0x0A,0x01};
static volatile int g_refuseLogN = 0;

void __cdecl on_encount_refused(uint32_t ecx) {
    (void)ecx;
    if (!g_base || g_refuseLogN >= 40) return;
    g_refuseLogN++;
    uint32_t bits, val; int ok;
    encount_gates(&bits, &val, &ok);
    ff13logv("[FF13-SRP] encounters: the game threw a touch away -- bit1 %s, bit6 %s, [+0x234] %s%08X"
             " (needs 0)%s\n", (bits & 0x02) ? "on" : "OFF", (bits & 0x40) ? "on" : "OFF",
             ok ? "= " : "UNREADABLE ", val, g_bsStarted ? "  (a picked fight is staging)" : "");
}

// The AsyncLoader's current-command type, the byte the game's own query at 0x446620 returns:
// 0 = its worker thread is idle, 5 = a chara/model load, 7 = a character-set load, -1 = unreadable.
// [[0x27FCE04]+8] is the loader the chara commands go to (read out of loadCharaByCharaSet,
// 0xBDE930). Raw read, no locks: a torn value costs one frame of a wrong answer.
#define RVA_ASYNC_LOADER 0x23FCE04
static int async_loader_type(void) {
    uintptr_t lp = g_base + RVA_ASYNC_LOADER;
    if (IsBadReadPtr((void*)lp, 4)) return -1;
    uint32_t reg = *(uint32_t*)lp;
    if (!reg || IsBadReadPtr((void*)(uintptr_t)(reg + 8), 4)) return -1;
    uint32_t ldr = *(uint32_t*)(uintptr_t)(reg + 8);
    if (!ldr || IsBadReadPtr((void*)(uintptr_t)(ldr + 0x18), 1)) return -1;
    return *(uint8_t*)(uintptr_t)(ldr + 0x18);
}

// The paradigm the pick was made in, put back on the field -- the game does it itself for some
// fights and deliberately not for others (CODE_NOTES) -- or, for a row that keeps it, the one the
// fight ended on. Only ever the DECK IN HAND is written, never the decks themselves. Must wait for
// the field to settle: the game's own restore lands at fight-over and the field's init writes
// after it, and writing into that leaves the menu showing one paradigm and the party fighting in
// another.
#define SEL_RESTORE_SETTLE_MS 1200
static void sel_restore_watch(DWORD now) {
    if (!g_selRestoreDue) return;
    if (g_bsStarted || g_bsTouchDue || overlay_in_battle()) return;   // another fight owns it
    if (field_zone_state() != 3) { g_selRestoreFrom = now; return; }  // the field is not back yet
    if ((int)(now - (g_selRestoreFrom + SEL_RESTORE_SETTLE_MS)) < 0) return;
    g_selRestoreDue = 0;
    int cur = deck_sel_live();
    if (g_selWant < 0 || cur < 0) return;                // nothing was in hand; nothing to put back
    if (cur == g_selWant) {
        (g_selKeepPick ? ff13log : ff13logv)("[FF13-SRP] battle picker: paradigm %d is still the "
                 "one in hand -- %s\n", cur,
                 g_selKeepPick ? "kept for the second half, as this row asks" : "left alone");
        return;
    }
    vm_set_optima_deck(g_selWant);
    if (g_selKeepPick)
        ff13log("[FF13-SRP] battle picker: paradigm %d back in hand for the second half -- the fight "
                "ended on it and the way back had reset it to %d\n", g_selWant, cur);
    else
        ff13log("[FF13-SRP] battle picker: paradigm back to %d (the fight ended on %d)\n",
                g_selWant, cur);
}

// The other half of the field drop in bs_start_selected. Same settle as the paradigm above: the
// field's own init is still writing when zone state first reads 3. A zone left in the meantime is
// owed nothing -- the reload that took the player there has already put the groups back.
static void field_drop_watch(DWORD now) {
    if (!g_fieldDropDue) return;
    if (g_bsStarted || g_bsTouchDue || overlay_in_battle()) { g_fieldDropFrom = now; return; }
    if (field_zone_state() != 3) { g_fieldDropFrom = now; return; }
    if ((int)(now - (g_fieldDropFrom + SEL_RESTORE_SETTLE_MS)) < 0) return;
    g_fieldDropDue = 0;
    uint32_t zoneNow = field_zone_id();
    if (zoneNow != (uint32_t)-1 && g_fieldDropZone != (uint32_t)-1 && zoneNow != g_fieldDropZone) {
        ff13logv("[FF13-SRP] battle picker: btsc%05u's field objects belong to another zone now -- "
                 "left to the reload\n", g_fieldDropFor);
        return;
    }
    int dn = 0;
    const ff13_fielddrop* fd = field_drops(g_fieldDropFor, &dn);
    for (int i = 0; i < dn; i++) {
        if (fd[i].kind == FDROP_HIDE) vm_layout_show(fd[i].name, 1);
        else                          vm_activate_area(fd[i].name);
    }
    ff13log("[FF13-SRP] battle picker: btsc%05u's field objects put back\n", g_fieldDropFor);
}

// The deferred half of the fight-over character-set restore. A chrset command sent while the
// battle's own loads are still draining freezes the AsyncLoader thread for the rest of the session,
// and the end-of-encounter work behind it never runs: the party cannot walk and every later touch
// is refused until a full reload (btsc04026, measured -- CODE_NOTES). So it goes back only with the
// field up and the loader idle. The cap is for an idle that never comes on a busy field.
#define CS_RESTORE_SETTLE_MS 800
#define CS_RESTORE_CAP_MS  15000
#define CS_RESTORE_RETRY_MS 30000   // how long a short fight's set stays loaded for the retry
#define CS_RESTORE_DRAIN_MS  2500   // the conclusion's own measured length, 1.2-1.6s, with room

// doEncountCheck's third gate is a pending-JOBS counter whose last decrement is the tail of the
// end-of-encounter work; sending a chrset into that is the freeze itself. The loader going idle is
// NOT the same question (measured: idle from 200ms after the fight, the counter still draining), so
// an unreadable counter counts as draining, never as drained.
static int enc_jobs_draining(DWORD now) {
    uint32_t bits, val; int ok;
    encount_gates(&bits, &val, &ok);
    if (!ok) return !g_csFieldBackAt || (int)(now - (g_csFieldBackAt + CS_RESTORE_DRAIN_MS)) < 0;
    return val != 0;
}

static void cs_restore_watch(DWORD now) {
    if (!g_csRestorePend[0]) return;
    if (g_bsStarted || g_bsTouchDue) { g_csRestoreStable = 0; return; }   // a pick is live: hold
    // Timed from the FIELD coming back, not the win: the result screens sit between the two and
    // their time is not the player's.
    if (g_csRestoreHold) {
        if (field_zone_state() != 3) return;
        g_csRestoreHold = 0;
        g_csRestoreEarliest = now + CS_RESTORE_RETRY_MS;
    }
    if ((int)(now - g_csRestoreEarliest) < 0) return;
    // On the field or not at all, cap or no cap: what the cap gives up on is the LOADER going idle.
    // A chrset sent into a fight is the thing this watch exists to prevent.
    if (field_zone_state() != 3) { g_csRestoreStable = 0; g_csFieldBackAt = 0; return; }
    if (!g_csFieldBackAt) g_csFieldBackAt = now;
    // ...and in the zone it was the set OF: the hold is long enough to walk out of the zone or to
    // load another save, and sending it then replaces the models the field actually has.
    uint32_t zoneNow = field_zone_id();
    if (g_csRestoreZone != (uint32_t)-1 && zoneNow != (uint32_t)-1 && zoneNow != g_csRestoreZone) {
        ff13log("[FF13-SRP] battle picker: \"%s\" is not this zone's set any more -- dropped "
                 "instead of loaded\n", g_csRestorePend);
        g_csRestorePend[0] = 0;
        g_csLoaded[0] = 0;
        g_looseLoaded = 0;
        return;
    }
    if ((int)(now - (g_csRestoreEarliest + CS_RESTORE_CAP_MS)) < 0) {
        if (async_loader_type() != 0 || enc_jobs_draining(now)) { g_csRestoreStable = 0; return; }
        if (!g_csRestoreStable) { g_csRestoreStable = now + CS_RESTORE_SETTLE_MS; return; }
        if ((int)(now - g_csRestoreStable) < 0) return;
    } else if (enc_jobs_draining(now)) {
        // The cap gives up on the LOADER, never on the conclusion: a set left loaded costs a field
        // with holes in it, sending into the conclusion costs the session.
        if (!g_csRestoreLate) {
            g_csRestoreLate = 1;
            ff13loga("[FF13-SRP] battle picker: the end-of-encounter work has not drained in %dms "
                     "-- \"%s\" waits rather than freezing the loader\n",
                     CS_RESTORE_CAP_MS, g_csRestorePend);
        }
        return;
    } else {
        ff13loga("[FF13-SRP] battle picker: the loader never went idle in %dms -- sending \"%s\" "
                 "back anyway\n", CS_RESTORE_CAP_MS, g_csRestorePend);
    }
    ff13log("[FF13-SRP] battle picker: character set back to \"%s\" (loader idle, %lums after "
             "the fight)\n", g_csRestorePend, (unsigned long)(now - g_csRestoreFrom));
    vm_load_charaset(g_csRestorePend);
    g_csRestorePend[0] = 0;
    // Only now is the fight's set really gone; while the restore was pending a retry could still
    // find it loaded and skip the reload (and the race that comes with one).
    g_csLoaded[0] = 0;
    g_looseLoaded = 0;
}

#ifdef FF13_DIAG
// Every-frame transition trace of the encounter-info slots doEncountCheck gates on: +0x234 is the
// third refusal, +0x238/+0x23C ride along (the battle-over worker writes +0x23C, the class ctor
// primes all three). Below it, which paradigm is in hand, sampled per frame so the moment it moves
// is on the record.
static void deck_sel_watch(void) {
    static int last = -2;
    int sel = deck_sel_live();
    if (sel == last) return;
    ff13loga("[FF13-SRP] DIAG deck sel: %d -> %d (zone state %d, %s, t=%lu)\n", last, sel,
             field_zone_state(), overlay_in_battle() ? "in battle" : "field",
             (unsigned long)GetTickCount());
    last = sel;
}

static void enc_slots_watch(void) {
    static uint32_t lastA = 0xDEADBEEF, lastB = 0, lastC = 0, lastBits = 0;
    uintptr_t s = g_base + RVA_ENCGATE3;
    if (IsBadReadPtr((void*)s, 4)) return;
    uint32_t scene = *(uint32_t*)s;
    if (!scene || IsBadReadPtr((void*)(uintptr_t)(scene + 0x36C), 4)) return;
    uint32_t sub = *(uint32_t*)(uintptr_t)(scene + 0x36C);
    if (!sub || IsBadReadPtr((void*)(uintptr_t)(sub + 0x234), 12)) return;
    uint32_t a = *(uint32_t*)(uintptr_t)(sub + 0x234);
    uint32_t b = *(uint32_t*)(uintptr_t)(sub + 0x238);
    uint32_t c = *(uint32_t*)(uintptr_t)(sub + 0x23C);
    uint32_t bits = 0;
    uintptr_t f = g_base + RVA_FIELDMGR;
    if (!IsBadReadPtr((void*)f, 4)) {
        uint32_t mgr = *(uint32_t*)f;
        if (mgr && !IsBadReadPtr((void*)(uintptr_t)(mgr + OFF_FIELD_BITS), 4))
            bits = *(uint32_t*)(uintptr_t)(mgr + OFF_FIELD_BITS);
    }
    int ltype = async_loader_type();
    static int lastL = -2;
    if (a == lastA && b == lastB && c == lastC && bits == lastBits && ltype == lastL) return;
    ff13loga("[FF13-SRP] DIAG encslots: +234=%08X +238=%08X +23C=%08X bits=%08X ldr=%d (zone state "
             "%d, t=%lu)\n", a, b, c, bits, ltype, field_zone_state(), (unsigned long)GetTickCount());
    lastA = a; lastB = b; lastC = c; lastBits = bits; lastL = ltype;
}

// ---- the game's leak tracker, walked the way free() (0x83FD30 -> 0xBB8550) walks it ----------
// One hash set of every tracked block's 16-byte header {vtbl, next, prev, flags}: 0x80 buckets x
// 12 bytes, chained on next. The walk is under the game's own mutex; the layout is in CODE_NOTES.
#define RVA_TRK_MUTEXP  0x241E250
#define RVA_TRK_MUTEX   0x241E238     // the object 0x83FAE0 stores at RVA_TRK_MUTEXP
#define RVA_TRK_ENABLED 0x241E254
#define RVA_TRK_TOTAL   0x241E258
#define RVA_TRK_TABLE   0x241E260
#define RVA_TRK_VTBL    0xCD5894      // VA 0x10D5894: what 0x83FC00 writes into every header
#define RVA_TRK_FIND    0x7B8550      // VA 0xBB8550: the walk this one copies
#define RVA_MTX_LOCK    0x409D30      // __thiscall(mutex, int tag)  ret 4
#define RVA_MTX_UNLOCK  0x409D80      // __thiscall(mutex)
#define TRK_BUCKETS     0x80
#define TRK_STEP_CAP    4000000
typedef void (__thiscall *mtxLock_t)(void* m, int tag);
typedef void (__thiscall *mtxUnlock_t)(void* m);
static const uint8_t P_MTX_LOCK[9]   = {0x55,0x8B,0xEC,0x83,0xEC,0x08,0x89,0x4D,0xF8};
static const uint8_t P_MTX_UNLOCK[7] = {0x55,0x8B,0xEC,0x51,0x89,0x4D,0xFC};
static const uint8_t P_TRK_FIND[9]   = {0x55,0x8B,0xEC,0x83,0xEC,0x10,0x89,0x4D,0xF0};

static int trk_verify(void) {
    static int state = 0;
    if (state) return state > 0;
    if (!g_base) return 0;
    int ok = !IsBadReadPtr((void*)(g_base + RVA_MTX_LOCK), sizeof P_MTX_LOCK)
          && !memcmp((void*)(g_base + RVA_MTX_LOCK), P_MTX_LOCK, sizeof P_MTX_LOCK)
          && !IsBadReadPtr((void*)(g_base + RVA_MTX_UNLOCK), sizeof P_MTX_UNLOCK)
          && !memcmp((void*)(g_base + RVA_MTX_UNLOCK), P_MTX_UNLOCK, sizeof P_MTX_UNLOCK)
          && !IsBadReadPtr((void*)(g_base + RVA_TRK_FIND), sizeof P_TRK_FIND)
          && !memcmp((void*)(g_base + RVA_TRK_FIND), P_TRK_FIND, sizeof P_TRK_FIND)
          && !IsBadReadPtr((void*)(g_base + RVA_TRK_MUTEXP), 4)
          && *(uintptr_t*)(g_base + RVA_TRK_MUTEXP) == g_base + RVA_TRK_MUTEX
          && !IsBadReadPtr((void*)(g_base + RVA_TRK_TABLE), TRK_BUCKETS * 12);
    state = ok ? 1 : -1;
    if (!ok)
        ff13loga("[FF13-SRP] DIAG tracker: the leak tracker is not where this build expects it -- "
                 "not walked\n");
    return ok;
}

static void trk_audit(const char* when) {
    static int saidOff = 0;
    if (!trk_verify()) return;
    if (!*(volatile uint32_t*)(g_base + RVA_TRK_ENABLED)) {
        if (!saidOff) {
            saidOff = 1;
            ff13loga("[FF13-SRP] DIAG tracker(%s): the game's leak tracker is OFF\n", when);
        }
        return;
    }
    void* mtx = *(void**)(g_base + RVA_TRK_MUTEXP);
    uint32_t goodVt = (uint32_t)(g_base + RVA_TRK_VTBL);
    ((mtxLock_t)(g_base + RVA_MTX_LOCK))(mtx, 0);
    uintptr_t table = g_base + RVA_TRK_TABLE;
    int nodes = 0, longest = 0, zombies = 0, steps = 0, zn = 0;
    char zl[3][300];
    for (int b = 0; b < TRK_BUCKETS; b++) {
        uint32_t node = *(uint32_t*)(table + b * 12);
        int len = 0;
        while (node && steps++ < TRK_STEP_CAP) {
            if (IsBadReadPtr((void*)(uintptr_t)node, 16)) {
                zombies++;
                if (zn < 3) snprintf(zl[zn++], sizeof zl[0],
                                     " -- zombie: node %08X in bucket %d is unmapped", node, b);
                break;
            }
            uint32_t vt = *(uint32_t*)(uintptr_t)node;
            if (vt != goodVt) {
                zombies++;
                if (zn < 3) {
                    char w[100], f[100];
                    uint32_t fn = IsBadReadPtr((void*)(uintptr_t)vt, 4) ? 0 : *(uint32_t*)(uintptr_t)vt;
                    addr_where(vt, w, sizeof w);
                    addr_where(fn, f, sizeof f);
                    snprintf(zl[zn++], sizeof zl[0],
                             " -- zombie: node %08X in bucket %d: vtbl=%08X (%s), slot0=%08X (%s), "
                             "next=%08X flags=%08X", node, b, vt, w, fn, f,
                             *(uint32_t*)(uintptr_t)(node + 4), *(uint32_t*)(uintptr_t)(node + 12));
                }
                break;                           // its next is not to be trusted
            }
            nodes++; len++;
            node = *(uint32_t*)(uintptr_t)(node + 4);
        }
        if (len > longest) longest = len;
    }
    uint32_t total = *(uint32_t*)(g_base + RVA_TRK_TOTAL);
    ((mtxUnlock_t)(g_base + RVA_MTX_UNLOCK))(mtx);
    // db_bt_result's sheet in use: 0 or 1 after a pick (btres_clear_for_pick), up to 3 in a story chain.
    int dbLevel = btres_level();
    ff13loga("[FF13-SRP] DIAG tracker(%s): %d nodes, longest bucket %d, %u bytes tracked, "
             "%d zombie(s), db_bt_result level %d%s%s%s%s\n", when, nodes, longest, total, zombies,
             dbLevel, steps >= TRK_STEP_CAP ? " (walk capped -- a cycle?)" : "",
             zn > 0 ? zl[0] : "", zn > 1 ? zl[1] : "", zn > 2 ? zl[2] : "");
}
static void trk_watch(DWORD now) {
    static DWORD nextAt = 0;
    if (nextAt && (int)(now - nextAt) < 0) return;
    nextAt = now + 20000;
    trk_audit("tick");
}
#endif

// ---- a pause menu that will not close --------------------------------------------------------
// A WITNESS, not a fix: a pause that will not close on START is reported but has never reproduced
// here, so this prints what could be holding it, once per episode (CODE_NOTES). 21..32 is every
// pause the state machine has -- EVENT_ON_FIELD/BATTLE, FIELD and BATTLE, each with its PAUSE /
// PAUSE_WAIT / PAUSE_END_WAIT -- and 75/76 lead out of one into a restart or a reset. The reported
// case was a BATTLE pause, so watching the field's alone is not enough.
#define PAUSE_STUCK_MS 45000
static int zone_state_is_pause(int st) {
    return (st >= 21 && st <= 32) || st == 75 || st == 76;
}
static void pause_stuck_watch(DWORD now) {
    static DWORD since = 0;
    static int said = 0;
    int st = field_zone_state();
    if (!zone_state_is_pause(st)) { since = 0; said = 0; return; }
    if (!since) { since = now; return; }
    if (said || (int)(now - (since + PAUSE_STUCK_MS)) < 0) return;
    said = 1;
    int gate = di_gate_left_ms();
    ff13loga("[FF13-SRP] the pause menu has been up %lums (%s) -- input gate %s, "
             "loader %d, field top menu %d, picker lock %d, pick %u, charaset restore %s, "
             "paradigm restore %s\n",
             (unsigned long)(now - since), zone_state_name(st),
             gate < 0 ? "off" : "ON (a pick is holding input)", async_loader_type(),
             vm_top_menu_get(), g_ctlLocked, g_bsStarted,
             g_csRestorePend[0] ? g_csRestorePend : "none",
             g_selRestoreDue ? "pending" : "none");
}

#define ZTRACE_MS 30000            // how long after a picked fight to keep watching
static volatile DWORD g_ztraceUntil = 0;
static void zone_state_trace(void) {
    static int last = -1;
    static DWORD since = 0;
    if (!g_ztraceUntil) { last = -1; return; }
    DWORD now = GetTickCount();
    if ((int)(now - g_ztraceUntil) >= 0) { g_ztraceUntil = 0; last = -1; return; }
    int st = field_zone_state();
    if (st == last) return;
    if (last >= 0)
        ff13logv("[FF13-SRP] zone state %d -> %d (after %ums)\n", last, st, (unsigned)(now - since));
    last = st; since = now;
}

static void home_watch(DWORD now) {
    if (g_homeDue != 1) return;
    if (field_zone_state() != 3) { g_homeAt = 0; return; }
    if (field_zone_id() != g_homeZone) {          // somewhere else entirely; not ours to move
        ff13logv("[FF13-SRP] battle picker: the way back landed in another zone -- the party is "
                 "left where it is\n");
        g_homeDue = 0; g_homeHave = 0;
        return;
    }
    if (home_zone_skipped(g_homeZone)) {
        ff13logv("[FF13-SRP] battle picker: the way back is off in zone %u -- the party stays at the "
                 "checkpoint ([battle_picker] return_skip_zones)\n", g_homeZone);
        g_homeDue = 0; g_homeHave = 0;
        return;
    }
    // NOT while the loading indicator is up: the restart runs one natural streaming cycle
    // (3 -> 13 -> 14 -> 15 -> 3) after the field returns, and a warp that moves the party while
    // it is still pending collides with it -- state 13 then never completes (btsc07037, measured;
    // an A/B with the warp disabled returned clean through the same cycle). Once the indicator is
    // down the field is settled and the warp streams its own cycle like an ordinary warp-key jump.
    if (g_loadUp) { g_homeAt = 0; return; }
    if (!g_homeAt) { g_homeAt = now + HOME_SETTLE_MS; return; }
    if ((int)(now - g_homeAt) < 0) return;
    g_homeDue = 2;                                  // the worker takes it from here (it blocks)
}

static void refund_watch(void) {
    if (!g_refundUntil) return;
    if (g_restartUntil) return;                  // the restart above has not even been asked for yet
    if (field_zone_state() == 3) {                  // FIELD_IDLE: whatever the way back was, it is over
        g_refundUntil = 0;
#ifdef FF13_DIAG
        trk_audit("back on the field");
#endif
        items_give_back();
        tp_give_back();
#ifdef FF13_DIAG
        libra_diag_point("after the way back", 1);   // ...and before the put-back undoes anything
#endif
        libra_put_back();
        libra_keep_apply();      // ...and for a kept fight, put its own gain back instead
        libra_zero_undo("the field is back");   // no-op when the put-back above already did it
        // The restart put the checkpoint's party back, which is not the one this fight is fought
        // with. But only where the party came with a fight the mod also stages: a fight with no door
        // is walked into like any other encounter, and re-issuing a party change on the FIELD after
        // it -- with a member whose model was fetched for a battle that is over -- is not something
        // to keep on a hunch.
        if (g_partyDoor) party_script_run("the field is back");
        else { g_partyWho = NULL; g_partyWhoN = 0; }
        return;
    }
    if ((int)(GetTickCount() - g_refundUntil) >= 0) {
        g_refundUntil = 0;
        // The item tally and the TP figure are kept: both are absolute numbers that are only ever
        // topped back up, so applying a stale one is at worst over-generous. The Libra snapshot is
        // put back by LOWERING, so a stale one would undo what an ordinary encounter legitimately
        // taught in the meantime. Dropped.
        int had = g_libraHave;
        g_libraHave = 0;
        // What THIS mod unscanned goes back regardless: it is a raise, so a stale one costs nothing.
        libra_zero_undo("the field did not come back");
        ff13logv("[FF13-SRP] battle picker: the field did not come back in %dms (state %d) -- what "
                 "the fight used is still on the tally for the next one%s\n",
                 REFUND_WAIT_MS, field_zone_state(),
                 had ? ", but what it taught about the enemies is kept" : "");
    }
}

// ================= INSTANT WIN (EXPERIMENTAL): a fight with nothing to fight ==================
//
// Victory WITHOUT THE DEATH PIPELINE, which is the one thing every earlier force-win attempt could
// not reach. The enemies are BUILT and then fail to ACTIVATE; the mod counts both halves out of the
// game's own warning printer:
//     BattleChara.cpp:736   "built the scene object <en_mon_0112><m233>"   (one per intended enemy)
//     BattleMain.cpp:3234   "activate failed: charaspec <m234> is not loaded"
// `fielded N of M` is that difference. An enemy that never activates is never on the roster the
// battle counts, so the fight is over before it starts.
//
// The whole decision is one function:
//
//   activateBattleChara 0x004DA820  __thiscall(this, uint32_t handle, int flag)  ret 8
//     0x004DA87A  flag == 0            -> straight to activate (never reaches our lever)
//     0x004DA895  call 0x006BF1A0(id)  -- is the model/resource resident?
//     0x004DA8A3  cmp [ebp-0xb8], 0    <-- THE LEVER
//     0x004DA8AA  resident             -> activate
//     0x004DA8BA  call 0x0046DDF0(obj) -- nibble6 in {0,1}? if not, activate anyway
//     0x004DA9EA  else: print the warning and RETURN, having done nothing else at all
//     0x004DAA18  call 0x0046D3D0(obj, flag)   -- the real activate; its ONLY call site
//
// So writing 0 into the residency verdict sends the game down its own not-resident path, which it
// ships with and handles gracefully. Nothing in the actor is touched, no death is declared, no VM is
// told anything -- this shrinks what the game counts instead of arguing about what it counted.
//
// esp at 0x004DA8A3 is exactly ebp - 0xb8: the entry does push ebp / mov ebp,esp / sub esp,0xb8,
// and the one push in between (0x004DA880) is taken back by `add esp,4` at 0x004DA89A. That makes
// every local reachable from the esp the hook hands us, which is why one hook covers all of it.
const uint8_t P_ACTIVATE[9]  = {0x55,0x8B,0xEC,0x81,0xEC,0xB8,0x00,0x00,0x00}; // push ebp;mov ebp,esp;sub esp,0xb8
_Static_assert(sizeof(P_ACTIVATE) == 9, "P_ACTIVATE size");
const uint8_t P_ACTGATE[7]   = {0x83,0xBD,0x48,0xFF,0xFF,0xFF,0x00};           // cmp dword [ebp-0xb8],0
_Static_assert(sizeof(P_ACTGATE) == 7, "P_ACTGATE size");
// esp-relative view of the frame at the gate (esp == ebp - 0xb8).
#define IWG_LOADED  0x00    // [ebp-0xb8] the residency verdict, about to be compared
#define IWG_OBJ     0x4C    // [ebp-0x6c] the battle actor, resolved at the top of the function
#define IWG_ID      0xB4    // [ebp-4]    the charaspec id that was looked up
#define IWG_HANDLE  0xC0    // [ebp+8]    arg1
#define IWG_FLAG    0xC4    // [ebp+0xc]  arg2
// The battle-system singleton and its handle table, so the probe can resolve `handle` at the entry
// without calling game code: 0x004ED600 is `if (handle == -1) return 0; return [[sys+0x13c] +
// (int16)handle * 4]`, with a warning but no bail-out for an index outside [0, 0x13). Expanded here
// so the range check is ours and a bad handle can never walk off the table.
#define RVA_BTLSYS      0x23FD20C    // VA 0x027FD20C: the battle-system singleton pointer
#define OFF_SYS_HANDLES 0x13C        // -> the actor handle table
#define SYS_HANDLE_MAX  0x13         // the table's own bound, from 0x004ED629

volatile int g_iwArmed    = 0;    // the toggle; OFF by default and never persisted
volatile int g_iwInstProbe = 0, g_iwInstGate = 0;
static volatile int g_iwProbeN   = 0;    // probe lines used this session
static volatile int g_iwSuppressed = 0;  // enemies suppressed since the toggle went on
#define IW_PROBE_MAX 200                 // one encounter is a handful of calls; this is a runaway cap

// Resolve an actor handle the way 0x004ED600 does, bounds-checked, without calling into the game.
static void* iw_actor(uint32_t handle) {
    if (!g_base) return NULL;
    int idx = (int)(int16_t)(uint16_t)handle;
    if (idx < 0 || idx >= SYS_HANDLE_MAX) return NULL;
    uint32_t* pSys = (uint32_t*)(g_base + RVA_BTLSYS);
    if (IsBadReadPtr(pSys, 4)) return NULL;
    uintptr_t sys = (uintptr_t)*pSys;
    if (!sys || IsBadReadPtr((void*)(sys + OFF_SYS_HANDLES), 4)) return NULL;
    uintptr_t tbl = (uintptr_t)*(uint32_t*)(sys + OFF_SYS_HANDLES);
    if (!tbl || IsBadReadPtr((void*)(tbl + (uintptr_t)idx * 4), 4)) return NULL;
    return (void*)(uintptr_t)*(uint32_t*)(tbl + (uintptr_t)idx * 4);
}

// True when this actor is on the enemy side, AT ACTIVATE TIME.
//
// NOT the team bit `[actor+0x24e] & 0x04` that the game's own enemy sweep 0x004DCC60 selects on: it
// is not set this early, and measured across one activation burst it told nobody apart.
// `guard324` is populated and split that burst exactly, and is the field the one-hit-kill cave has
// gated on since it shipped. The lifecycle nibble agreed independently; it is deliberately NOT added
// as a second condition, because a fight that populates it differently would make this a silent
// no-op. Unreadable means "not known to be an enemy", which the caller treats as do-not-touch.
static int iw_is_enemy(void* obj) {
    if (!obj || IsBadReadPtr(obj, ACTOR_SPAN)) return 0;
    return *(uint32_t*)((char*)obj + OFF_GUARD324) != 0;
}

// ---- the probe: who goes through activate, and what do they look like there? ----------------
// READ-ONLY, verbose-only, capped. It stays in the shipped DLL because the same question comes back
// for any fight that instant-win handles wrongly. Installed via ff13_install_esp_hook, so `esp` is
// the caller's stack pointer at the entry: [esp]=return address, [esp+4]=handle, [esp+8]=flag.
void __cdecl on_activate_probe(uint32_t esp) {
    if (g_logLevel < 2) return;                    // verbose diagnostic only
    if (g_iwProbeN >= IW_PROBE_MAX) return;
    if (IsBadReadPtr((void*)(uintptr_t)esp, 12)) return;
    uint32_t ret    = *(uint32_t*)(uintptr_t)(esp + 0);
    uint32_t handle = *(uint32_t*)(uintptr_t)(esp + 4);
    uint32_t flag   = *(uint32_t*)(uintptr_t)(esp + 8);
    void* obj = iw_actor(handle);
    g_iwProbeN++;
    if (!obj || IsBadReadPtr(obj, ACTOR_SPAN)) {
        ff13logv("instant-win(probe): handle=0x%X flag=%u -> actor unresolved  <- caller 0x%08X\n",
                 (unsigned)handle, (unsigned)flag, (unsigned)ret);
        return;
    }
    uint32_t lc = *(uint32_t*)((char*)obj + OFF_LIFECYCLE);
    ff13logv("instant-win(probe): handle=0x%X flag=%u actor=0x%08X team24e=0x%02X(%s) guard324=0x%X "
             "life=%u hp=%d/%d  <- caller 0x%08X\n",
             (unsigned)handle, (unsigned)flag, (unsigned)(uintptr_t)obj,
             (unsigned)*(uint8_t*)((char*)obj + OFF_TEAM), iw_is_enemy(obj) ? "enemy" : "NOT enemy",
             (unsigned)*(uint32_t*)((char*)obj + OFF_GUARD324), (unsigned)((lc >> 12) & 0xF),
             *(int*)((char*)obj + OFF_HP), *(int*)((char*)obj + OFF_MAXHP), (unsigned)ret);
}

// ---- the gate: tell the game this enemy's model is not resident ------------------------------
// Runs on the game thread, inside battle setup, immediately BEFORE the game's own `cmp` reads the
// value we are writing. Writes one dword in the caller's frame and nothing else -- popad restores
// registers only, so the write survives (the same property the worker-result hook relies on).
void __cdecl on_activate_gate(uint32_t esp) {
    if (!g_iwArmed) return;
    if (IsBadWritePtr((void*)(uintptr_t)(esp + IWG_LOADED), 4)) return;
    uint32_t* loaded = (uint32_t*)(uintptr_t)(esp + IWG_LOADED);
    if (!*loaded) return;                          // already going to fail; leave it alone
    if (IsBadReadPtr((void*)(uintptr_t)(esp + IWG_FLAG), 4)) return;
    void* obj = *(void**)(uintptr_t)(esp + IWG_OBJ);
    // Enemy-only, and failing towards doing nothing: an actor we cannot positively identify as an
    // enemy is left alone, the fight is ordinary, and the probe line says what was there instead.
    if (!iw_is_enemy(obj)) return;
    *loaded = 0;                                   // -> the game's own "not resident" path
    g_iwSuppressed++;
    g_iwHitThisFight = 1;                          // keep the picker from learning from this fight
    if (g_iwSuppressed <= IW_PROBE_MAX) {
        uint32_t lc = *(uint32_t*)((char*)obj + OFF_LIFECYCLE);
        ff13logv("[FF13-SRP] instant-win: actor 0x%08X (charaspec id %u, guard324=0x%X, "
                 "team24e=0x%02X, life=%u) told its model is not resident -- it will not be "
                 "activated (#%d this arming)\n",
                 (unsigned)(uintptr_t)obj, (unsigned)*(uint32_t*)(uintptr_t)(esp + IWG_ID),
                 (unsigned)*(uint32_t*)((char*)obj + OFF_GUARD324),
                 (unsigned)*(uint8_t*)((char*)obj + OFF_TEAM), (unsigned)((lc >> 12) & 0xF),
                 g_iwSuppressed);
    }
}

// Called from the hotkey thread and from state_load; touches nothing of the game's, so there is no
// game-thread work to defer. g_armed stays a plain byte because the one-hit-kill cave compares it
// directly.
// What the OVERLAY calls the kill mode: the player's language, dropped into T_KILL_LINE.
const char* kill_mode_name(int m) {
    return ui_text(m == KILL_INSTANT ? T_KILL_INSTANT
                 : m == KILL_ONEHIT  ? T_KILL_ONEHIT
                                     : T_KILL_OFF);
}
// ...and what the LOG calls it, which is English whatever the game is running in. Straight out of
// the English table rather than a second set of literals, so the two names cannot drift apart.
const char* kill_mode_name_en(int m) {
    return kOvlEn[m == KILL_INSTANT ? T_KILL_INSTANT
                : m == KILL_ONEHIT  ? T_KILL_ONEHIT
                                    : T_KILL_OFF];
}
void kill_mode_apply(int mode, int announce) {
    g_killMode = mode;
    g_armed    = (mode != KILL_OFF) ? 1 : 0;
    g_iwArmed  = (mode == KILL_INSTANT) ? 1 : 0;
    if (g_iwArmed) g_iwSuppressed = 0;
    if (!announce) return;
    switch (mode) {
    case KILL_INSTANT:
        ff13log("[FF13-SRP] kill mode: INSTANT WIN + ONE-HIT KILL. Instant-win acts while a fight "
                 "is being BUILT, so it has to be on BEFORE the encounter starts -- the fight you "
                 "are in right now is not affected by it, which is what the one-hit-kill underneath "
                 "is for. Party members (guard324==0) are untouched by both.%s\n",
                 g_iwInstGate ? "" : " WARNING: the instant-win gate hook is NOT INSTALLED, so only "
                                     "one-hit-kill will do anything.");
        break;
    case KILL_ONEHIT:
        ff13log("[FF13-SRP] kill mode: ONE-HIT KILL. Any enemy that takes damage has its HP forced "
                 "to 0. Party members (guard324==0) are unaffected. Do NOT summon an Eidolon while "
                 "this is on.\n");
        break;
    default:
        ff13log("[FF13-SRP] kill mode: OFF (original behaviour restored).\n");
        break;
    }
}

// Keep the zone's own two track names current. Cheap, and it is what tells the BGM learner
// which name is this area's field theme rather than a boss theme.
static void pump_zone_bgm(DWORD now) {
    static DWORD nextZoneBgm = 0;
    if ((int)(now - nextZoneBgm) >= 0) { nextZoneBgm = now + 1000; bgm_zone_refresh(); }
}

// The event door's second half: the staging has had its moment, so run the event. From here on
// the game does everything -- its own callback starts the fight.
static void pump_event_door(DWORD now) {
    if (g_evPending && (int)(now - g_evAt) >= 0) {
        const ff13_arena* e = g_evPending;
        g_evPending = NULL;
        // The override has had its window; the chain takes over from here and needs the party free.
        if (g_warpActive) {
            int walked = (e->evPos[0] != 0.0f || e->evPos[1] != 0.0f || e->evPos[2] != 0.0f);
            hold_still_end();
            if (walked) {
                float ax = 0, ay = 0, az = 0;
                int have = field_party_pos(&ax, &ay, &az);
                ff13logv("[FF13-SRP] battle picker: party is at %s(%.2f %.2f %.2f) after the walk "
                         "(wanted %.2f %.2f %.2f, moved=%u skipped=%u)\n", have ? "" : "?",
                         (double)ax, (double)ay, (double)az,
                         (double)e->evPos[0], (double)e->evPos[1], (double)e->evPos[2],
                         (unsigned)g_warpMoved, (unsigned)g_warpSkip);
            }
        }
        // The chain's cutscene must accept the skip buttons, so the input gate lifts here. The
        // party stays pinned and the menu stays shut until the battle is up.
        di_gate_lift();
        // Armed just before the event, so the chain's own first native call spends it. The window
        // is the give-up cap: past that there is no chain left to be inside of.
        if (e->evSetup) {
            g_gameTid = GetCurrentThreadId();
            g_setupUntil = GetTickCount() + EVENT_GIVEUP_CAP_MS;
            g_setupWant = e->evSetup;
            ff13logv("[FF13-SRP] battle picker: \"%.31s\" is armed for the chain to run\n",
                     e->evSetup);
        }
        vm_start_event(e->ev[0], e->ev[1], e->ev[2], e->ev[3], e->evPack2, e->evScript,
                       e->evLoad[0], e->evLoad[1]);
        // A chain run for its side effects starts no fight, so there is nothing to give up on and
        // nothing to wait for but the chain itself. Come back through the picker afterwards with
        // the door switched off, and pump_scene_light's arena branch starts the fight.
        if (e->evNoFight) {
            g_evNoFightAt = GetTickCount() + EVENT_SIDEEFFECT_MS;
            ff13logv("[FF13-SRP] battle picker: the chain is running for what it does, not for the "
                     "fight -- starting btsc%05u in %dms\n", g_bsStarted, EVENT_SIDEEFFECT_MS);
        } else {
            g_evGiveUpCap = GetTickCount() + EVENT_GIVEUP_CAP_MS;
        }
    }
}

// The fight is up: write the decks it is meant to have over the ones it generated for itself.
static void pump_decks(DWORD now) {
    if (g_decksWantN && g_decksWantFor && g_btlScene == g_decksWantFor) {
        int ok = 1;
        if (g_keepGuard) {          // captured PLAYER decks: only over the same three (see the pick)
            int cur[3];
            g_keepGuard = 0;
            ok = party_battle_ids(cur) && cur[0] == g_keepParty[0]
              && cur[1] == g_keepParty[1] && cur[2] == g_keepParty[2];
        }
        if (!ok) {
            g_decksWantFor = 0; g_decksWantN = 0; g_decksWant = NULL;
            ff13logv("[FF13-SRP] battle picker: the chain fielded a different three -- its own "
                     "paradigms stand\n");
            return;
        }
        decks_apply(g_decksWant, g_decksWantN, g_keepSel);
        g_decksAgainAt = now + 1200;      // ...and once more, in case something writes after this
        g_decksWantFor = 0;
        optima_log("paradigms set for this fight");
    } else if (g_decksRestoreFor && g_btlScene == g_decksRestoreFor) {
        g_decksRestoreFor = 0;
        // The decks themselves are left ALONE: what the game generates for the new pair is what the
        // story fight has (measured -- CODE_NOTES). Only the one in hand is set.
        ff13log("[FF13-SRP] battle picker: this fight's party was set by the picker -- opening on "
                 "the first paradigm\n");
        vm_set_optima_deck(0);
    } else if (g_decksAgainAt && (int)(now - g_decksAgainAt) >= 0) {
        g_decksAgainAt = 0;
        if (g_decksWantN && g_btlScene) {
            decks_apply(g_decksWant, g_decksWantN, g_keepSel);
            optima_log("paradigms set again");
        }
    }
    // ALWAYS, not only while a picked fight is in flight: the run that settles what the decks should
    // be is the same fight taken the story's way, which goes nowhere near the door. Deck contents
    // change only when edited and at a party change, so this stays quiet on its own.
    if (g_optimaWatchN < OPTIMA_LOG_MAX && (int)(now - g_optimaWatchAt) >= 0) {
        g_optimaWatchAt = now + 200;
        uint32_t k = party_state_key();
        if (k && k != g_optimaWatch) {
            int first = (g_optimaWatch == 0);
            g_optimaWatch = k;
            g_optimaWatchN++;
            char when[40];
            snprintf(when, sizeof when, first ? "first reading" : "change %d", g_optimaWatchN);
            optima_log(when);
        }
    }
}

// The cutscene the door was opened for is up: it is on screen, so the cover is not needed and
// the game owns the input again.
static void pump_event_giveup(DWORD now) {
    if (g_evCutSeen) {
        g_evCutSeen = 0;
        if (g_blackUntil) {
            ff13logv("[FF13-SRP] battle picker: the cutscene is up -- the cover comes off now\n");
            blackout_end();
        }
        // Ask again for the enemies no character set carries. The first ask goes out when the door
        // is opened, before the chain has loaded anything, and a chain that then loads a set of its
        // own replaces the lot. Resource.loadChara ADDS, so a second ask is free when the first
        // survived and is the whole fight when it did not. The cutscene is the time it has to land.
        if (g_bsStarted) {
            const ff13_arena* ar2 = scene_arena(g_bsStarted);
            if (ar2 && ar2->ev[0]) load_loose_specs(g_bsStarted, NULL);
        }
    }
    // ...and back through the picker, once. The door is switched off for this pass only.
    if (g_evNoFightAt && (int)(now - g_evNoFightAt) >= 0) {
        g_evNoFightAt = 0;
        g_evSkipDoor = 1;
        // The door turned the one-shot swap off on its way in, because a door consumes nothing.
        // This pass is the touch path, and the touch path is exactly what the swap is for.
        g_swapEnabled = 1; g_swapBtscId = g_bsStarted;
        snprintf(g_swapNameBuf, sizeof g_swapNameBuf, "btsc%05u", g_bsStarted);
        g_bsTouchPending = g_evTouch;
        g_bsTouchAt = now;                   // the models were waited for on the first pass
        g_bsTouchCap = 0; g_bsTouchExtra = 0;
        g_bsTouchDue = 1;
        ff13logv("[FF13-SRP] battle picker: the chain is done -- starting btsc%05u the ordinary "
                 "way, with the party and paradigms it left behind\n", g_bsStarted);
    }
    // An event that was asked for and did not happen. The event lock cannot be waited out, because
    // the player is standing in it -- so the cap, and ONLY the cap. Do NOT add a quicker way out on
    // FIELD_IDLE: that state means "the field is not in a transition", not "nothing is happening",
    // and a chain doing its staging is neither (one had already activated its enemy group and
    // started the eidolon theme when the idle test fired).
    // The cap is against a chain that is STUCK, not one that is long: every zone-state change buys
    // it the cap again, so a chain with cutscenes in front of its fight is left alone.
    if (g_evGiveUpCap) {
        static int lastSt = -1;
        int st = field_zone_state();
        if (st != lastSt) {
            if (lastSt >= 0) g_evGiveUpCap = now + EVENT_GIVEUP_CAP_MS;
            lastSt = st;
        }
        if (overlay_in_battle()) {
            g_evGiveUpCap = 0; lastSt = -1;       // the fight is up; everything releases as usual
        } else if ((int)(now - g_evGiveUpCap) >= 0) {
            lastSt = -1;
            ff13logv("[FF13-SRP] battle picker: the event started nothing in %dms (zone state %d) "
                     "-- letting go so the field is not left locked\n",
                     EVENT_GIVEUP_CAP_MS, field_zone_state());
            g_evGiveUpCap = 0;
            ctl_unlock();
            evquiet_lift();
            g_bsStarted = 0; g_arena = 0; g_placeBoss = 0;
            g_wantCP = 0; g_bsArena = 0;
            blackout_end();
        }
    }
}

// The holds and watches that want calling on every tick and decide for themselves whether there is
// anything to do. Order matters here too: the lighting below reads what the placement above did.
static void pump_holds(DWORD now) {
    ctl_hold();
#ifdef FF13_DIAG
    // Where the party is under the cover, once a second: the leader by name and the party
    // reference. If the numbers move, the lock is not holding; if they do not, what moved was
    // something else.
    if (g_blackUntil && g_bsStarted) {
        static DWORD nextCov = 0;
        if ((int)(now - nextCov) >= 0) {
            nextCov = now + 1000;
            char who[20];
            float px = 0, py = 0, pz = 0;
            int have = field_party_pos(&px, &py, &pz);
            ff13logv("[FF13-SRP] under the cover: leader \"%s\", party at %s(%.2f %.2f %.2f), "
                     "locked=%d\n", leader_name(who) ? who : "?", have ? "" : "?",
                     (double)px, (double)py, (double)pz, g_ctlLocked);
        }
    }
#endif
    arena_hold();
    aim_hold();
    // The battle has given the enemies their places, so they exist and can be lit. Still under the
    // cover: the placement runs while the fight is being built, and the cover comes off after that.
    if (!g_lightDone && g_bsStarted && g_placeBoss && g_bossGotMatrix) {
        g_lightDone = 1;
        scene_light_apply(g_bsStarted, LIGHT_CHARA, "and its enemies");
    }
    camera_follow_hold();
    camera_recentre_due();
    turn_watch_tick();
    boss_basis_probe();
    encinfo_tweak();
    restart_watch();
    restart_after_watch();
    ctl_giveback_retry();
    bg_heal_watch();
    bgm_heal_watch();
    rpflag_watch();
    menuskip_watch();
    ucon_watch();
    refund_watch();
    home_watch(GetTickCount());
    zone_state_trace();
}

// The party is on the arena by now, so the room around them is the room that gets the light. Twice:
// the second go catches whatever the streaming brought in after the first. Then the start itself.
static void pump_scene_light(DWORD now) {
    if (g_arenaPending && (int)(now - g_arenaStartAt) >= 0 && g_lightMapN < 2
        && (!g_lightMapAt || (int)(now - (g_lightMapAt + LIGHT_ROOM_AGAIN_MS)) >= 0)) {
        if (scene_light_apply(g_bsStarted, LIGHT_MAP,
                              g_lightMapN ? "the room again, for whatever arrived since"
                                          : "the room the party is now standing in"))
            g_lightMapN++;
        else
            g_lightMapN = 2;                  // this scene has no lighting of its own to set
    }
    // The rest of the room, by name, a few dozen at a time. Holds the start until it is through.
    if (g_arenaPending && g_lightMapN >= 1 && !g_sweepDone) {
        if (scene_light_sweep(g_bsStarted)) g_lightMapAt = now;   // still going; keep waiting
        else g_sweepDone = 1;
    }
    if (g_arenaPending && (int)(now - g_arenaStartAt) >= 0 && g_lightMapN >= 2
        && (!g_lightMapAt || (int)(now - (g_lightMapAt + LIGHT_EASE_MS)) >= 0)) {
        const ff13_arena* a = g_arenaPending;
        g_arenaPending = NULL;
        float ax = 0, ay = 0, az = 0;
        int have = field_party_pos(&ax, &ay, &az);
        ff13logv("[FF13-SRP] battle picker: party is at %s(%.2f %.2f %.2f) after %dms; starting "
                 "btsc%05u through \"%s\" (flags 0x%X)\n", have ? "" : "?", ax, ay, az,
                 ARENA_GATHER_MS, g_bsStarted, a->arena, a->flags);
        field_camera_recenter();
        g_bossSlotN = 0; g_placeLogged = 0; g_stageLogged = 0;
#ifdef FF13_DIAG
        g_pjLog = 0;
#endif
        g_bossGotMatrix = 0; g_placeMatN = 0;
        for (int i = 0; i < ARENA_ENEMIES; i++) g_enUsed[i] = 0;   // this fight claims them afresh
        g_lightDone = 0;
        g_bossOpenUntil = GetTickCount() + BOSS_BUILD_MS;
        // Whatever stage is left over from the last fight, so this one's can be told from it.
        g_stagePrevHave = stage_origin((float*)g_stagePrev);
        g_bossOx = a->ex; g_bossOy = a->ey; g_bossOz = a->ez; g_arena = a;
        g_placeBoss = a->arena[0] ? 1 : 0;
        // A fight that needed its arena mapset loaded starts only once that load is DONE, the way
        // the story starts it. The screen is under the cover, so the stall shows nothing.
        if (a->bg && g_bgAsked) {
            DWORD w0 = GetTickCount();
            vm_sync_wait();
            ff13logv("[FF13-SRP] battle picker: waited %ums for \"%s\" (the story's own "
                     "loadBg+syncWait)\n", (unsigned)(GetTickCount() - w0), a->bg);
        }
        g_bgAsked = 0;
        scene_light_apply(g_bsStarted, LIGHT_CHARA, "the party");
        vm_start_battle(a->arena, a->flags, g_bsStarted);
        g_encUntil = GetTickCount() + ENCTWEAK_MS;   // hold the change over the whole build
        g_encLast = 0;
        // Not here: the field is still what is on screen until the battle has something to draw.
        // battle_cam_watch lifts it when the fight is up (and holds it a moment longer if asked).
    }
#ifdef FF13_DIAG
    // Every battle, not only picked ones: an ordinary encounter is the baseline that says whether
    // enemies commit a transform through this path at all.
    {
        static int wasIn = 0;
        int nowIn = overlay_in_battle();
        if (nowIn && !wasIn) {
            g_posSnap = 0; g_posN = 0; g_camSnapAt = now + 8000;
            {
                const char* how = g_bsStarted ? (scene_arena(g_bsStarted) ? "picked, arena door"
                                                                         : "picked, field door")
                                              : "the game's own";
                encinfo_probe(how);
                batflags_probe(how);
            }
        }
        if (!nowIn) g_camSnapAt = 0;
        wasIn = nowIn;
        if (g_camSnapAt && (int)(now - g_camSnapAt) >= 0) {
            g_camSnapAt = 0;
            g_posSnap = now + 40;              // one frame's worth of commits
            // The stage too, and for the game's OWN fights: stage_place only runs for an arena
            // start, so a story fight's measured positions would otherwise have no frame to be
            // read in.
            {
                char* bm = g_base ? *(char**)(g_base + RVA_BM_PTR) : NULL;
                if (bm && !IsBadReadPtr(bm + OFF_BM_STAGE, 64)) {
                    const float* m = (const float*)(bm + OFF_BM_STAGE);
                    ff13logv("[FF13-SRP] battle stage (any fight): x(%.3f %.3f %.3f) "
                             "y(%.3f %.3f %.3f) z(%.3f %.3f %.3f) origin(%.3f %.3f %.3f)\n",
                             m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                             m[12], m[13], m[14]);
                }
            }
            ff13logv("[FF13-SRP] --- actor positions, 8s into btsc%05u (%s, zone %u, charaset "
                     "\"%s\") ---\n", g_bsStarted ? g_bsStarted : g_lastBtscId,
                     g_bsStarted ? "picked" : "the game's own", field_zone_id(),
                     g_curCharset[0] ? g_curCharset : "?");
        }
    }
#endif
}

// Ask for the track and start watching.
static void bgm_play_want(DWORD now) {
    int ok = bgm_lazy_play(g_bgmWant);
    if (ok) { strncpy(g_bgmMine, g_bgmWant, BGM_NAME_MAX); g_bgmMine[BGM_NAME_MAX] = 0; }
    g_bgmWatchUntil = now + 12000;
    g_bgmWatchLast[0] = 0;
    ff13logv("[FF13-SRP] battle picker: asking for \"%.16s\" -> %s\n", g_bgmWant,
             ok ? "accepted" : "REFUSED (not a BGM resource?) -- music left alone");
}

// The track a picked fight is meant to have: let a stale request lapse, play the one that is due,
// and watch what the game puts up afterwards so the learner has something to file.
static void pump_battle_bgm(DWORD now) {
    if (g_bgmWant[0] && (int)(g_bgmWantUntil - now) <= 0) {
        g_bgmWant[0] = 0; g_bgmDue = 0; g_bgmHoldUntil = 0;
    }
    // Done from here rather than from inside the play call, so the sound system is not re-entered
    // while it is mid-request. Some fights never start a track of their own, so this cannot wait for
    // the game's battle theme. The wait is on g_btlStatAt, not g_btlScene: g_btlScene can still hold
    // the PREVIOUS fight, and the fallback would then fire while the player is still in the field.
    if (g_bgmWant[0] && !g_bgmDue && overlay_in_battle()
        && (int)(g_btlStatAt - g_bgmArmedAt) >= 0
        && (int)(now - (g_btlStatAt + 1200)) >= 0
        && (int)(g_bgmWantUntil - now) > 0) {
        char cur[BGM_NAME_MAX + 1];
        cur[0] = 0;
        if (bgm_current(cur) && bgm_is_outcome_track(cur)) {
            // The fanfare, the result screen or a game over: the fight this request belongs to is
            // OVER. The takeover below only asks "is something other than our track playing", and
            // after a win the answer is yes twice, which is how a won fight came to play its battle
            // theme over the results. Nothing to take over from here.
            g_bgmWant[0] = 0; g_bgmDue = 0; g_bgmSkipHold = 0; g_bgmHoldUntil = 0;
            ff13logv("[FF13-SRP] battle picker: \"%.16s\" means the fight has ended -- the track "
                     "request is dropped rather than played over it\n", cur);
        } else if (cur[0] && strncmp(cur, g_bgmWant, BGM_NAME_MAX) != 0) {
            strncpy(g_bgmTheirs, cur, BGM_NAME_MAX);
            g_bgmTheirs[BGM_NAME_MAX] = 0;
            g_bgmDue = 1;
            // No hold on this route: this branch is already past the battle state coming up, so
            // holding again would only add a second of silence to a fight that is under way.
            g_bgmSkipHold = 1;
            ff13logv("[FF13-SRP] battle picker: no battle theme of its own; taking over from "
                     "\"%.16s\"\n", g_bgmTheirs);
        } else if (cur[0] == 0) {
            // Nothing is playing at all. The branch above is a takeover and needs something to take
            // over from, so without this a fight started in a silent field stays silent.
            g_bgmTheirs[0] = 0;
            g_bgmDue = 1;
            g_bgmSkipHold = 1;
            ff13logv("[FF13-SRP] battle picker: nothing was playing here -- starting \"%.16s\" "
                     "outright\n", g_bgmWant);
        }
    }
    if (g_bgmDue && (int)(g_bgmWantUntil - now) > 0) {
        g_bgmDue = 0;
        // Take the ordinary theme away first, then ask for ours: starting ours does not displace
        // it (on hardware both played at once), and stopping by name is cleanest while that name is
        // still the one the manager has recorded.
        if (g_bgmTheirs[0]) bgm_stop(g_bgmTheirs, 0.0f);
        g_bgmHushed[0] = 0;
        int skip = g_bgmSkipHold;
        g_bgmSkipHold = 0;
        if (g_bgmHoldMs > 0 && !skip) {
            g_bgmHoldUntil = now + (DWORD)g_bgmHoldMs;
            if (!g_bgmHoldUntil) g_bgmHoldUntil = 1;   // 0 is "no hold"; the tick counter does wrap
            ff13logv("[FF13-SRP] battle picker: %s%.16s%s \"%.16s\" is held for %dms\n",
                     g_bgmTheirs[0] ? "stopped \"" : "nothing to stop;",
                     g_bgmTheirs[0] ? g_bgmTheirs : "", g_bgmTheirs[0] ? "\";" : "", g_bgmWant,
                     g_bgmHoldMs);
        } else {
            bgm_play_want(now);
        }
    }
    // The hold. Keeping it quiet is the work: the game is free to ask again through lazyPlay, which
    // does not come through on_bgm_play at all, so the manager is asked every tick what it thinks is
    // playing and anything that is not ours is stopped again. Polling is the only way to see it.
    if (g_bgmHoldUntil) {
        char cur[BGM_NAME_MAX + 1];
        if (bgm_current(cur) && cur[0] && strncmp(cur, g_bgmWant, BGM_NAME_MAX) != 0) {
            // ...and only once there IS a fight to be over: the same jingle is left sounding in the
            // field by a battle fought a moment ago, and it dropped the pick's music (measured).
            if (bgm_is_outcome_track(cur) && g_bsInFight) {
                // The fight is already over. Whatever the hold was waiting to play belongs to a
                // fight that is not there any more; the field-side tidy-up is pump_battle_live's.
                g_bgmHoldUntil = 0;
                ff13logv("[FF13-SRP] battle picker: \"%.16s\" ended the fight during the hold -- "
                         "\"%.16s\" is dropped\n", cur, g_bgmWant);
            } else {
                bgm_stop(cur, 0.0f);
                if (strncmp(cur, g_bgmHushed, BGM_NAME_MAX) != 0) {
                    strncpy(g_bgmHushed, cur, BGM_NAME_MAX); g_bgmHushed[BGM_NAME_MAX] = 0;
                    ff13logv("[FF13-SRP] battle picker: \"%.16s\" started during the hold -- "
                             "stopped again\n", cur);
                }
            }
        }
        if (g_bgmHoldUntil && (int)(now - g_bgmHoldUntil) >= 0) {
            g_bgmHoldUntil = 0;
            bgm_play_want(now);
        }
    }
    // Report what the manager says is playing while that request settles: an unloaded track does not
    // fall back to silence -- the previous one simply keeps going -- so a request that never lands
    // would otherwise look identical in the log to one that worked.
    if ((int)(g_bgmWatchUntil - now) > 0) {
        static DWORD nextWatch = 0;
        if ((int)(now - nextWatch) >= 0) {
            nextWatch = now + 500;
            char cur[BGM_NAME_MAX + 1];
            if (bgm_current(cur) && strncmp(cur, g_bgmWatchLast, BGM_NAME_MAX) != 0) {
                strncpy(g_bgmWatchLast, cur, BGM_NAME_MAX); g_bgmWatchLast[BGM_NAME_MAX] = 0;
                ff13logv("[FF13-SRP] BGM now playing: \"%.16s\"\n", cur);
            }
        }
    }
}

// The cover's deadline. Three ways out, and which one is taken decides whether the picked fight is
// dropped -- getting that wrong is silent and expensive, so each branch says why it is that branch.
static void pump_cover_lapse(DWORD now) {
    if (g_blackUntil && (int)(now - g_blackUntil) >= 0) {
        if (overlay_in_battle() || g_bsInFight) {
            // The clock ran out over a LIVE fight. That is the cover's normal end, and nothing may
            // be dropped here: this branch once fell through to the drop below, which cleared
            // g_wantCP 150ms into every arena fight and took the checkpoint ending with it.
            g_blackUntil = 0;
        } else if (g_evGiveUpCap || g_evNoFightAt) {
            // An event door is still in flight and now owns how this ends: it has its own cap.
            // Dropping the pick here would take the fight away from a chain that is still working.
            ff13logv("[FF13-SRP] blackout: deadline reached while the event door is still open -- "
                     "the cover comes off, btsc%05u is left alone\n", g_bsStarted);
            g_blackUntil = 0;
        } else {
            ff13logv("[FF13-SRP] blackout: deadline reached and no battle started -- btsc%05u "
                     "dropped\n", g_bsStarted);
            g_blackUntil = 0;
            g_bsTouchDue = 0; g_bsTouchPending = 0;
            g_bsStarted = 0;
            g_bgmWant[0] = 0; g_bgmDue = 0; g_bgmHoldUntil = 0;
            // If an arena was mid-staging, none of it may leak into whatever fight comes next: the
            // checkpoint conversion would end that fight at the checkpoint, and the boss placement
            // would hand its unregistered actors our stage.
            g_arenaPending = NULL;
            g_wantCP = 0; g_bsArena = 0; g_placeBoss = 0;
            g_swapEnabled = 0; g_swapBtscId = 0;   // nor may the armed swap outlive the drop
            ctl_unlock();
            evquiet_lift();
        }
    }
}

// Returns 1 when the rest of the frame must be skipped: every step below reads state
// this one is still in the middle of building. It is the only step that can stop the pump.
static int pump_touch_due(DWORD now) {
    if (g_bsTouchDue) {
#ifdef FF13_DIAG
        // Heartbeat while the picker waits for the character set: the frame tick drives it, so a
        // hang in this window shows up as the heartbeat stopping.
        {
            static DWORD nextHb = 0;
            if ((int)(now - nextHb) >= 0) {
                nextHb = now + 1000;
                ff13logv("[FF13-SRP] picker pending: btsc%05u, %ums until the cap\n",
                         g_bsStarted, (int)(g_bsTouchAt - now) > 0 ? g_bsTouchAt - now : 0);
            }
        }
#endif
        // Start as soon as the models are actually there; the configured wait is only the cap.
        const char* missing = NULL;
        int ready = scene_resident(g_bsStarted, &missing);
        int timeUp = (int)(now - g_bsTouchAt) >= 0;
        // An immediate yes is believed: charaspec_resident asks the game's own isRead, and a yes
        // from it means the model IS in memory. A short settle first, so a load that is only just
        // beginning has had time to show as "not read". A party change is the one wait that cannot
        // be cut short -- no probe here answers for a party member's model, and starting without
        // one took the game down.
        DWORD armedAt = g_bsTouchAt - (DWORD)g_bsTouchCap;
        if (ready == 1 && !g_bsProbeMoved) {
            if (g_bsTouchFull) ready = -1;                       // party change: wait it out
            else if ((int)(now - armedAt) < BS_PROBE_SETTLE_MS) return 1;
            else if (!g_bsProbeLogged) {
                g_bsProbeLogged = 1;
                ff13logv("[FF13-SRP] battle picker: everything this fight needs was already there "
                         "-- starting %ums in, not at the %dms cap\n",
                         (unsigned)(now - armedAt), g_bsPreloadMs);
            }
        }
        if (ready == 0) g_bsProbeMoved = 1;            // it has been seen saying no: it is real
        if (ready != 1 && !timeUp) return 1;             // still loading, or cannot tell -- wait
        if (g_bsTouchCap && ready == 0 && g_bsTouchExtra < BS_PRELOAD_EXTRA_MAX_MS) {
            // ready==0 is always a trusted no: an untrusted yes was coerced to -1 above.
            if (!g_bsTouchExtra)
                ff13logv("[FF13-SRP] battle picker: \"%s\" still loading at the %dms cap -- "
                         "waiting up to %dms more\n", missing ? missing : "?", g_bsTouchCap,
                         BS_PRELOAD_EXTRA_MAX_MS);
            g_bsTouchExtra += BS_PRELOAD_STEP_MS;
            g_bsTouchAt = now + BS_PRELOAD_STEP_MS;
            g_bsStartedAt += BS_PRELOAD_STEP_MS;   // the give-up clock times the fight, not the wait
            return 1;
        }
        if (ready == 1 && !g_bsTouchReady) {
            g_bsTouchReady = 1;
            ff13logv("[FF13-SRP] battle picker: models resident after %dms\n",
                     g_bsTouchCap + g_bsTouchExtra - (int)(g_bsTouchAt - now));
        }
        if (ready == 0)
            ff13logv("[FF13-SRP] battle picker: \"%s\" still not resident after %dms -- starting "
                     "anyway\n", missing ? missing : "?", g_bsTouchCap + g_bsTouchExtra);
        loose_specs_report();     // the hand-fetched ones, now that the waiting is over
        // The drop's second pass, now that the field has had the whole wait to settle.
        if (g_fieldDropDue && g_fieldDropFor == g_bsStarted)
            field_drops_apply(g_bsStarted);
        uint32_t touch = g_bsTouchPending;
        g_bsTouchDue = 0;
        g_bsTouchPending = 0;
        // A scene whose story flow uses an arena is started through that door instead: no touched
        // object, no field encounter, the game's own scripted call.
        const ff13_arena* a = scene_arena(g_bsStarted);
        // An entry with an event does none of the staging below. The whole point of it is that the
        // game stages this fight, exactly as it does for the story, and its callback starts it.
        if (a && a->ev[0] && !g_evSkipDoor) {
            g_evTouch = touch;                       // kept for the second pass, if there is one
            g_swapEnabled = 0; g_swapBtscId = 0;     // the touch path armed it; nothing consumes it
            g_arena = a;
            g_bsArena = a->restartAfterWin ? 1 : 0;
            g_wantCP = g_bsArena && g_restartOnArena;
            g_restartBitSet = 0;
            g_placeBoss = 0;                         // the event places everything
            g_apN = 0; g_apReady = 0; g_apUntil = 0; // ...and nobody is warped or held
            g_bsTouchPending = 0; g_bsTouchDue = 0;
            // The cover STAYS. It used to come down here, and between this line and the cutscene
            // there is however long the chain takes, during which a lit field with a movable party
            // showed through and the pause menu could be opened over a party being rebuilt. It comes
            // down when a cutscene starts, when the fight starts, or on the cover's own deadline.
            g_evCutSeen = 0;
            ff13logv("[FF13-SRP] battle picker: btsc%05u through its own event \"%s\" -- the event "
                     "starts the fight (callback \"%s\"). Skip the cutscene when it plays.\n",
                     g_bsStarted, a->ev[0], a->ev[1] ? a->ev[1] : "(none)");
            // A chain will not run a second time while the scenario flag says it already has, so put
            // the flag back to what it was before this fight. The other way round for an evNoFight
            // entry: there it is set EXACTLY, in whichever direction, because it is being used to
            // shut a gate rather than open one.
            if (a->evFlag) {
                int cur = vm_get_work(0);
                // The ORIGINAL value comes back at fight-over for an evNoFight gate (the field
                // re-inits off it) and for an evEndRp fight (the re-marked checkpoint must carry
                // the player's own progress, not the chain's).
                if (a->evNoFight || a->evEndRp) { g_evFlagWas = cur; g_evFlagRestore = 1; }
                if (a->evNoFight ? (cur != a->evFlag) : (cur > a->evFlag)) {
                    vm_set_work(0, a->evFlag);
                    ff13logv("[FF13-SRP] battle picker: scenario flag %d -> %d, so this fight's "
                             "chain %s\n", cur, a->evFlag,
                             a->evNoFight ? "stops before its cutscene" : "will run again");
                }
            }
            if (a->evQuiet) {
                int w = vm_get_work(1);
                if (!(w & 1)) vm_set_work(1, w | 1);
                g_evQuietOn = 1;
                ff13logv("[FF13-SRP] battle picker: auto music off (Party work 1 bit 0), as the "
                         "story stages this fight -- the chain's track carries through\n");
            }
            // Every party change in the game goes through common's sfSetUpParty, which ends
            // restoreOptimaDeckAll() -- so the chain already makes that call and the decks are still
            // wrong; the answer is not to make it again. Watch what each step turns them into.
            optima_log("before the door");
            if (a->evEvFlag) {
                vm_set_event_flag(a->evEvFlag, 1);
                // That branch also opens the preparation menu, stopped at its own native.
                g_menuSkip = 1;
                g_menuSkipUntil = GetTickCount() + MENUSKIP_MS;
                // Read back rather than trust: the chain's restart branch is all-or-nothing, so if
                // none of it happens the first question is whether the flag is even up.
                ff13logv("[FF13-SRP] battle picker: event flag \"%s\" raised, reads back %d -- the "
                         "chain's own restart branch fades the screen back in and offers the "
                         "preparation menu\n", a->evEvFlag, vm_get_event_flag(a->evEvFlag));
            }
            for (int i = 0; i + 1 < 8 && a->evChara[i]; i += 2)
                vm_create_field_chara(a->evChara[i], a->evChara[i + 1]);
            for (int i = 0; i < 2 && a->evArea[i]; i++)
                vm_activate_area(a->evArea[i]);
            vm_party_marker(a->evMarker ? a->evMarker : -1);
            // Some of these events do not place the party at all: the story reaches them by the
            // player walking into a trigger box, so the event's own move branch is empty and nothing
            // in the game moves anybody. Picked from anywhere else the party is simply in the wrong
            // place, so walk them there with the warp the hotkeys use rather than a native of the
            // game's -- armed here and let go of when the event is started, the same shape the arena
            // door's warp has.
            if (a->evPos[0] != 0.0f || a->evPos[1] != 0.0f || a->evPos[2] != 0.0f) {
                float cx = 0, cy = 0, cz = 0;
                if (field_party_pos(&cx, &cy, &cz)) {
                    warp_flag_cluster(cx, cy, cz);
                    g_refX = cx; g_refY = cy; g_refZ = cz;
                    g_leaderIdx = 0xFFFFFFFF;
                    g_ovrX = a->evPos[0]; g_ovrY = a->evPos[1]; g_ovrZ = a->evPos[2];
                    g_warpMoved = 0; g_warpSkip = 0;
                    g_warpActive = 1;
                    ff13logv("[FF13-SRP] battle picker: walking the party to %.2f %.2f %.2f, where "
                             "this event expects to find them\n",
                             (double)a->evPos[0], (double)a->evPos[1], (double)a->evPos[2]);
                }
            }
            // The staging above is asked for, not done: the story's own "before" function waits on
            // it (sfWaitActivateArea) before the cutscene runs. Give it the same moment rather than
            // blocking the frame the game is drawing.
            g_evPending = a;
            g_evAt = GetTickCount() + EVENT_STAGE_MS;
            return 1;
        }
        // ...and neither does the second pass of one: an entry that has a door and nothing else has
        // all-zero arena fields, so the arena door would stand its party on another fight's
        // formation. It comes out right through the ordinary field encounter below.
        if (a && !g_evSkipDoor) {
            // Walk the party onto the arena first, the way the story event does, then start the
            // fight from there. The warp is the field one the warp hotkeys use -- it holds the
            // committed position for a moment and lets go, so the party lands and stays.
            float cx = 0, cy = 0, cz = 0;
            if (!field_party_pos(&cx, &cy, &cz)) {
                ff13logv("[FF13-SRP] battle picker: no party reference -> arena start skipped\n");
            } else {
                // The one-shot swap was armed for the touch path, and this door never consumes it.
                // Left armed it outlived the fight and turned the NEXT ordinary encounter into the
                // picked boss (measured).
                g_swapEnabled = 0; g_swapBtscId = 0;
                g_arena = a;
                g_bsArena = a->restartAfterWin ? 1 : 0;   // ...if this one can end that way
                g_wantCP = g_bsArena && g_restartOnArena;
                g_restartBitSet = 0;
                g_apN = 0; g_apReady = 0; g_apRefX = cx; g_apRefZ = cz;
                g_apRot[0] = g_apRot[1] = g_apRot[2] = 0;
                arena_recentre_reset();
                memset(g_apName, 0, sizeof g_apName);
                g_arenaPending = a;
                g_arenaStartAt = now + ARENA_GATHER_MS;
                // A derived entry cannot place the party until the battle has a stage, which is
                // after the call, so the hold has to reach well past it. A measured one is on the
                // arena before the call and needs only the settle.
                g_apUntil = now + ARENA_GATHER_MS
                          + (arena_has_party(a) ? ARENA_SETTLE_MS : ARENA_DERIVED_MS);
                // Ours only: the encounter coroutine samples the camera for every fight, and this
                // window is the only thing that says the next sample is the one we asked for.
                g_arenaCamUntil = now + ARENA_GATHER_MS + 2500;
#ifdef FF13_DIAG
                g_faceLogN = 0;
#endif
                field_camera_recenter();
                // Only a measured entry knows where the party is going before it gets there.
                if (arena_has_party(a))
                    camera_follow_carry(a->wp[1][0] - cx, a->wp[1][1] - cy, a->wp[1][2] - cz);
                ff13logv("[FF13-SRP] battle picker: staging btsc%05u on \"%s\" (party reference "
                         "%.2f %.2f %.2f); recognising the party\n",
                         g_bsStarted, a->arena, cx, cy, cz);
                return 1;
            }
        }
        uint32_t mgr = field_mgr();
        if (mgr && !IsBadWritePtr((void*)(uintptr_t)(mgr + OFF_TOUCHED_ID), 4)) {
            *(uint32_t*)(uintptr_t)(mgr + OFF_TOUCHED_ID) = touch;
            // The stall deadline runs from HERE, not from the pick: staging routinely outlasts it,
            // and a pick that waited 2500ms for its models read as a wedged touch in the log.
            if (g_touchAt) { g_touchAt = now; g_touchSeq = g_encIdSeq; }
            blackout_end();
        } else {
            ff13logv("[FF13-SRP] battle picker: field manager gone while loading -> fight dropped\n");
            g_bsStarted = 0;
            blackout_end();
        }
    }
    return 0;
}

// File what the game said about the fight it just built.
//
// This only ever hears from SUBSTITUTED fights: "BattleChara.cpp:736 -- the battle side created a
// scene object" is printed when the battle side has to BUILD the enemy, which a substitution always
// makes it do. Walk into a fight and the field actor you touched is carried over instead, and the
// game says nothing at all -- so a pick that lands on the scene the field was already going to run
// measures as silence too. Filing is gated on want > 0 below, so the silent ones leave no record
// rather than a false zero.
// Wait for the game to finish reporting rather than counting a fixed time from the start: the
// encounter presentation, and the character-set wait before it, can push the enemy-building messages
// well past three seconds -- and then nothing was filed at all.
static void pump_battle_report(DWORD now) {
    if (g_btlScene && (((g_btlCreated > 0 || g_btlNotLoaded > 0)
                        && (int)(now - (g_btlLastMsgAt + 1500)) >= 0)
                       || (int)(now - (g_btlStatAt + 25000)) >= 0)) {
        uint32_t scene = g_btlScene;
        int got = g_btlCreated - g_btlFailed, want = g_btlCreated;
        g_btlScene = 0;
        // The count is only filed when the game reported building something: some story fights build
        // no scene objects at all, they carry the actor over from the field, and gating the whole
        // block on that count stopped their music being read as well.
        // g_btlShort is what THIS fight did, kept apart from what the scene has ever managed --
        // mark_field_result remembers the best showing on purpose, so reading the shortfall back out
        // of it missed a fight that failed now but had worked before. Short if the game said an
        // actor failed, OR if it said a charaspec was not loaded: those arrive per charaspec rather
        // than per actor and cannot be subtracted from the count.
        g_btlShort = ((want > 0 && got < want) || g_btlNotLoaded > 0) ? 1 : 0;
        if (g_btlNotLoaded > 0 && g_btlFailed == 0) {
            // How many actually appeared is not knowable from these lines alone; say so rather
            // than filing a number that would be a guess.
            ff13logv("[FF13-SRP] btsc%05u came up short: %d charaspec(s) not loaded%s%s\n",
                     scene, g_btlNotLoaded, g_btlBad[0] ? ", first: " : "",
                     g_btlBad[0] ? g_btlBad : "");
        } else if (want > 0 && !g_iwHitThisFight) {
            mark_field_result(scene, got < 0 ? 0 : got, want);
        } else if (want > 0) {
            // Instant-win emptied this fight, so what it fielded says nothing about the zone. Left
            // unfiled: otherwise a run with instant-win on would teach the picker that every fight
            // it ends is a scene this zone cannot field.
            ff13log("[FF13-SRP] btsc%05u fielded %d of %d, not filed -- instant-win emptied it\n",
                     scene, got < 0 ? 0 : got, want);
        }
        // What this fight is ACTUALLY playing, asked of the manager rather than watched for: a fight
        // that inherits its music from the cutscene that starts it calls nothing, so there is no
        // event to catch. Skipped while we are substituting the music ourselves -- that would only
        // read back our own choice. Logged, nothing more.
        {
            char cur[BGM_NAME_MAX + 1];
            if (!g_bgmWant[0] && bgm_current(cur) && cur[0]) {
                ff13logv("[FF13-SRP] btsc%05u battle music: \"%.16s\"%s\n", scene, cur,
                         bgm_is_battle_track(cur)
                         ? "" : "  (not a battle arrangement -- carried in from a cutscene?)");
            }
        }
        // The table did not expect this to work here and it did: the model was resident by some
        // route the character-set data does not describe. Logged only -- "it worked while C was
        // loaded" is not proof that C is what made it work. Only when we did NOT load a set for
        // this fight, since g_curCharset would then be the field's set and credit the wrong thing.
        const ff13_scene_models* sm = scene_models(scene);
        const ff13_charaset* cur = charaset_by_name(g_curCharset);
        // ...and only when the game built something: a fight that builds nothing satisfies
        // got == want trivially, and "fielded all 0" is not evidence of anything.
        if (want > 0 && got == want && sm && !g_csRestore[0] && (!cur || !charaset_covers(cur, sm)))
            ff13logv("[FF13-SRP] note: btsc%05u fielded all %d while \"%s\" was loaded, which the "
                     "table does not credit for it -- another route to residency\n",
                     scene, want, g_curCharset[0] ? g_curCharset : "?");
    }
}

// A picked fight that ends almost immediately spawned nothing -- remember that scene so the
// list can warn about it next time.
static void pump_battle_live(DWORD now) {
    if (g_bsStarted) {
        int inb = overlay_in_battle();
        bm_timeline(now);
        cam_pump(g_bsStarted, now);
        if (inb) {
            if (!g_bsInFight) {
                ctl_unlock();                  // the battle owns input now; the field gets its back
#ifdef FF13_DIAG
                trk_audit("battle up");
#endif
            }
            g_bsInFight = 1;
            // Read while the fight is up, not at fight-over: the game's own put-back lands at
            // fight-over, and whether that is before or after the battle state clears is not
            // settled, so the last reading from inside the fight is the one that counts.
            { int s = deck_sel_live(); if (s >= 0) g_selLast = s; }
            if (g_statFor == g_bsStarted) stat_pump();
            // Any targetable enemy HP means something spawned. Timing cannot be used here: with
            // one-hit-kill or game-speed on, a perfectly normal fight is over in a few seconds.
            if (read_via_ptr(RVA_TARGETHP_PTR, OFF_TARGETHP) > 0) g_bsSawEnemy = 1;
        }
        else if (g_bsInFight) {
            uint32_t fought = g_bsStarted;
#ifdef FF13_DIAG
            trk_audit("fight over");
#endif
            // Only when the game itself said nothing -- its own count is better than this guess.
            // Same reason as mark_field_result above: a fight we emptied ourselves is not evidence
            // about what this zone can field.
            if (!g_bsSawEnemy && !g_iwHitThisFight && !field_rec(g_bsStarted, NULL, NULL))
                mark_empty_scene(g_bsStarted);
            g_bsStarted = 0; g_bsInFight = 0; g_bsSawEnemy = 0;
            aipin_battle_over();
            stat_battle_over();
#ifdef FF13_DIAG
            // Read here as well as after the way back: this is the last moment before the restart,
            // so a scan that is present now and gone later was taken by the restart, not by us.
            libra_diag_point("as the fight ended", 0);
#endif
            // ...which is exactly why a kept fight's scan is read HERE and written again later.
            if (g_libraKeepPick) libra_keep_take(fought);
            if (g_homeHave && !g_homeNoWarp) { g_homeDue = 1; g_homeAt = 0; }   // same for where you stood
            // Put an evNoFight gate's flag back BEFORE the restart reloads: the field init that
            // runs during it reads the flag, and the gate value stages the fight's own tableau.
            if (g_evFlagRestore) {
                g_evFlagRestore = 0;
                int cur = vm_get_work(0);
                vm_set_work(0, g_evFlagWas);
                ff13logv("[FF13-SRP] battle picker: scenario flag %d -> %d (put back for the "
                         "restart's field init)\n", cur, g_evFlagWas);
            }
            // ...and arm the STAGE17 append for the restart's OWN establish, so the reconcile that
            // runs during the reload streams the field's ground back. A chain-staged fight narrows
            // what the streamer thinks this place needs (btsc07050: only the plaza came back --
            // void beyond it); the cells captured while standing at the pick spot are the answer.
            // Arming only before the way-back warp was too late: establish had already run.
            // Sticky: the return's own establishes re-narrow desired[] and evict them again
            // (btsc07050, measured), so keep re-appending for as long as the walk back may take.
            if (g_homeCellsN > 0) save_arm_reconcile_sticky(g_homeCells, g_homeCellsN, 120000);
            save_mapset_census("at fight-over");
            // An evNoFight chain's cutscene never played, so its tidy-up never ran either: dispose
            // the schedule and pack the pick loaded, exactly as the story's own callback would --
            // resident cutscene prep rides the retry snapshot into the post-fight field. A chain
            // whose cutscene DID play disposed its own; doing it again here would double-free.
            {
                const ff13_arena* fa = scene_arena(fought);
                if (fa && fa->evNoFight && fa->evLoad[0] && fa->evLoad[0][0])
                    vm_dispose_event(fa->evLoad[0], fa->evLoad[1], NULL);
            }
            // A fight staged on an arena is a fight the story owns. Left alone, winning it hands
            // the run straight to what the story does next; watch for the way back and take the
            // checkpoint instead. See restart_watch.
            if (g_bsArena) {
                g_bsArena = 0;
                if (g_restartOnArena && g_trigOk) {
                    // A chain that marked its checkpoint inside itself must not be reloaded into:
                    // re-mark the checkpoint at a plain restart point NOW (the flag was already
                    // put back above, so the snapshot carries the player's own progress). The
                    // reload is then a full field load, so the quick-retry aftercare stands down:
                    // no mapset heal over it and no way-back warp (crashed together, 2026-08-16).
                    {
                        const ff13_arena* ga = scene_arena(fought);
                        if (ga && ga->evEndRp) {
                            vm_restart_save(ga->evEndRp);
                            ff13logv("[FF13-SRP] battle picker: checkpoint re-marked at \"%s\" -- "
                                     "the reload lands there, outside the chain\n", ga->evEndRp);
                            // The reload is a complete field load, so no mapset heal over it --
                            // but the way back stays armed: the reload's spot is the checkpoint's,
                            // not where the fight was picked.
                            g_homeBg[0] = 0;
                        }
                        if (ga && ga->evGate) {
                            g_gateReplayVal   = ga->evGate;
                            g_gateReplayUntil = GetTickCount() + 45000;
                            g_uconDue = 0;
                            g_homeBg[0] = 0;
                            g_homeDue = 0; g_homeHave = 0;
                        }
                    }
                    g_restartUntil = GetTickCount() + RESTART_WATCH_MS;
                    g_restartLast  = -1;
                    // Remembered HERE and not read back later: by the time the restart finishes the
                    // fight it belongs to is over and g_bsStarted has been cleared, so asking then
                    // which fight this was is asking zero.
                    g_ctlGiveBack = scene_win_at_checkpoint(fought);
                    ff13logv("[FF13-SRP] battle picker: btsc%05u was staged on an arena -- ending "
                             "it at the checkpoint rather than in the story flow\n", fought);
                } else if (!g_trigOk) {
                    ff13loga("[FF13-SRP] battle picker: btsc%05u was staged on an arena, but the "
                             "state-machine trigger is not available -- the story flow takes it "
                             "from here\n", fought);
                }
            }
            // However that fight ended -- won, lost, or sent back to the checkpoint by the block
            // above -- what it cost gets put back once the field is there to put it back on.
            g_ztraceUntil = GetTickCount() + ZTRACE_MS;   // say which restart the way back gets
            g_pickedEndAt = GetTickCount();               // ...and whose a game over now would be
            int tally = (g_refundItems && g_itemOk) ? g_itemLogN : 0;
            // g_libraHave is in the test because it is the one of the three that can be the only
            // thing to put back: a fight that spent nothing still taught something.
            if (tally > 0 || g_tpBefore >= 0 || g_libraHave) {
                g_refundUntil = GetTickCount() + REFUND_WAIT_MS;
                ff13log("[FF13-SRP] battle picker: btsc%05u ran up a tally of %d item%s (TP was "
                         "%d) -- waiting for the field to put it back\n",
                         fought, tally, tally == 1 ? "" : "s", g_tpBefore);
            }
            // Put the field's own set back: the fight's set is missing models the field is using, so
            // leaving it loaded is how you get a field with holes in it.
            // Deliberately not an automatic retry: a battle restarting by itself reads as a bug to
            // anyone who does not know why.
            int wasShort = g_btlShort;   // ...and a short fight HOLDS the set (see the arming below)
            if (g_btlShort) {
                g_btlShort = 0;
                ff13logv("[FF13-SRP] battle picker: btsc%05u came up short -- pick it again to "
                         "retry; the models it needed are loaded now\n", fought);
                // On screen as well: the log is not where someone finds out a fight they just
                // watched was missing an enemy.
                char msg[80];
                snprintf(msg, sizeof msg, "btsc%05u came up short -- pick it again", fought);
                flash_say(msg);
            }
            g_placeBoss = 0; g_bossSlotN = 0; g_apN = 0; g_apReady = 0;
            cam_disarm();                  // the record is shared -- do not leave the bit set
            // Everything below runs on the frame tick, so a call that blocks blocks here and nowhere
            // else -- which is why the tidy-up is timed. The trip back sits in STATE_ENCOUNT_BACK
            // for three to four seconds and how much of that is ours is not yet settled.
            DWORD tidyAt = GetTickCount(), bgmMs = 0;
            // Whatever paradigm this fight ended on, the field gets the pick's own back -- or, for
            // a pick that keeps it, the one the fight ended on, for the second half to open on.
            g_selWant = g_selKeepPick ? g_selLast : g_selBefore;
            g_selRestoreDue = (g_selWant >= 0);
            g_selRestoreFrom = tidyAt;
            // NOT issued here: a chrset sent while the encounter is still concluding freezes the
            // AsyncLoader for the session. cs_restore_watch sends it once the field is back.
            if (g_csRestore[0]) {
                strncpy(g_csRestorePend, g_csRestore, sizeof g_csRestorePend - 1);
                g_csRestorePend[sizeof g_csRestorePend - 1] = 0;
                g_csRestore[0] = 0;
                g_csRestoreFrom = GetTickCount();
                g_csRestoreZone = field_zone_id();
                g_csRestoreStable = 0;
                g_csRestoreLate = 0;
                g_csFieldBackAt = 0;
                // A short fight holds its set: "pick it again -- the models are loaded now" is only
                // true while they are, and a retry that finds them loaded has no load to race.
                // g_csLoaded stays set for the same reason; the watch clears it when it fires.
                g_csRestoreEarliest = g_csRestoreFrom;
                g_csRestoreHold = wasShort;
                ff13log("[FF13-SRP] battle picker: fight over -> \"%s\" goes back once the field "
                         "has settled%s\n", g_csRestorePend,
                         wasShort ? " (held while a retry can still use this fight's set)" : "");
            } else if (g_looseLoaded && g_curCharset[0]) {
                // A fight whose enemies came one at a time rather than as a set had nothing to put
                // back, so those models simply stayed loaded -- in the one zone that streams its
                // ground a cell at a time and has the least room to spare. Loading the field's own
                // set again takes them out, since a set replaces what is resident.
                strncpy(g_csRestorePend, g_curCharset, sizeof g_csRestorePend - 1);
                g_csRestorePend[sizeof g_csRestorePend - 1] = 0;
                g_csRestoreFrom = GetTickCount();
                g_csRestoreZone = field_zone_id();
                g_csRestoreStable = 0;
                g_csRestoreLate = 0;
                g_csFieldBackAt = 0;
                g_csRestoreEarliest = g_csRestoreFrom;
                g_csRestoreHold = wasShort;
                ff13log("[FF13-SRP] battle picker: fight over -> \"%s\" will be reloaded once the "
                         "field has settled, to take out the models this fight asked for one at a "
                         "time%s\n", g_csRestorePend,
                         wasShort ? " (held while a retry can still use them)" : "");
            }
            // A held track must not outlive the fight it was held for -- one-hit-kill ends fights
            // well inside the hold. The field's music has to be asked for here: the hold took it
            // away and the game will not put it back, for the same reason its end-of-fight stop is a
            // no-op below.
            if (g_bgmHoldUntil) {
                const char* back = g_bgmFieldNow[0] ? g_bgmFieldNow : g_zoneField;
                g_bgmHoldUntil = 0;
                ff13logv("[FF13-SRP] battle picker: fight over before \"%.16s\" was due -- dropped"
                         "%s%.16s%s\n", g_bgmWant,
                         back[0] ? ", field music back to \"" : " (no field track known)",
                         back[0] ? back : "", back[0] ? "\"" : "");
                if (back[0]) { DWORD at = GetTickCount(); bgm_lazy_play(back);
                               bgmMs += GetTickCount() - at; }
            }
            // ...and the music, which does not come back on its own: the game ends a fight by
            // stopping the track IT started, and ours is not that track, so its stop is a no-op and
            // the battle theme plays on out in the field. Only while ours is still the one playing --
            // anything else means something has already moved on and must not be interrupted.
            if (g_bgmMine[0]) {
                char cur[BGM_NAME_MAX + 1];
                if (bgm_current(cur) && strncmp(cur, g_bgmMine, BGM_NAME_MAX) == 0) {
                    const char* back = g_bgmFieldNow[0] ? g_bgmFieldNow : g_zoneField;
                    DWORD at = GetTickCount();
                    bgm_stop(g_bgmMine, 0.0f);
                    ff13log("[FF13-SRP] battle picker: fight over -> stopped \"%.16s\"%s%.16s%s\n",
                             g_bgmMine, back[0] ? ", field music back to \"" : " (no field track known)",
                             back[0] ? back : "", back[0] ? "\"" : "");
                    if (back[0]) bgm_lazy_play(back);
                    bgmMs += GetTickCount() - at;
                }
                g_bgmMine[0] = 0;
            }
            sound_give_room_back();
            {
                DWORD tidyMs = GetTickCount() - tidyAt;
                if (tidyMs >= 32)
                    ff13log("[FF13-SRP] battle picker: fight over -> the tidy-up held the frame for "
                             "%lums (music %lums)\n",
                             (unsigned long)tidyMs, (unsigned long)bgmMs);
            }
        } else if (GetTickCount() - g_bsStartedAt > BS_GIVEUP_MS) {
            // A pick that never becomes a fight leaves the event lock on, and the event lock is what
            // makes the party unable to walk -- so this is the way out of being stuck, not a
            // tidy-up. A slow-disk extension is added to this clock as it is granted, so what is
            // timed here is always the same window: the fight coming up AFTER the staging.
            cam_disarm();
            ff13logv("[FF13-SRP] battle picker: btsc%05u never became a fight in %dms -- giving the "
                     "field back\n", g_bsStarted, BS_GIVEUP_MS);
            flash_say(ui_text(T_PICK_NEVER_STARTED));
            g_bsStarted = 0;                   // never got into a fight; forget it
            aipin_battle_over();
            g_csRestore[0] = 0;
            staging_disarm();
            ctl_unlock();
            evquiet_lift();
        }
    }
}

// The picker is a field tool: everything it can do only means anything while you can walk around, so
// close it the moment that stops being true -- a fight, a cutscene, a menu, a load. Mounting a
// chocobo counts; the leader read costs a party-manager lock, so it is only asked while the list is
// actually up.
static void pump_picker_input(void) {
    if (g_bsRememberDue) {
        g_bsRememberDue = 0;
        if (g_bsCount > 0 && g_bsSel >= 0 && g_bsSel < g_bsCount)
            zb_remember_sel(field_zone_id(), g_bsList[g_bsSel], g_bsVar[g_bsSel]);
    }
    if (g_bsOpen && (field_zone_state() != 3 || riding_chocobo())) {
        if (g_bsCount > 0 && g_bsSel >= 0 && g_bsSel < g_bsCount)
            zb_remember_sel(field_zone_id(), g_bsList[g_bsSel], g_bsVar[g_bsSel]);
        g_bsOpen = 0;
        g_bsReq = 0;
        return;
    }
    int req = g_bsReq;
    if (!req) return;
    g_bsReq = 0;
    if (req == 1 || req == 3) {
        if (field_zone_state() != 3) {               // same rule for opening it in the first place
            ff13logv("[FF13-SRP] battle picker: not in the field (zone_state=%d) -> not opened\n",
                     field_zone_state());
            flash_say(ui_text(T_PICK_FIELD_ONLY));
            return;
        }
        if (riding_chocobo()) {
            ff13logv("[FF13-SRP] battle picker: on a chocobo -> not opened (dismount first)\n");
            flash_say(ui_text(T_PICK_CHOCOBO));
            return;
        }
        // Moved here from the hotkey thread: this reads the zone id off a pointer chain, which is
        // game state and belongs on the game thread.
        uint32_t zoneNow = field_zone_id();
        if (!g_bsOpen && !g_lastBtscId && zb_lookup(zoneNow) == 0xFFFFFFFFu) {
            // The zone has no scene band, so there is nothing to list. A shipped build is meant to
            // know every zone already, so this is a gap in the table rather than a chore for whoever
            // found it: say what is wrong and give them the one number that makes it fixable.
            ff13logv("[FF13-SRP] battle picker: zone %u has no scene band -> not in this build's "
                     "table\n", zoneNow);
            char msg[224];
            snprintf(msg, sizeof msg, ui_text(T_PICK_TEACH_ONE), zoneNow);
            flash_say(msg);
            return;
        }
        bs_scan();
        if (req == 3) {
            // A hold with the list shut starts the row it would have opened on. Only an empty list
            // opens instead, so a long press is never a dead key.
            if (g_bsCount > 0 && g_bsSel >= 0 && g_bsSel < g_bsCount) {
                bs_start_selected();
                return;
            }
            ff13logv("[FF13-SRP] battle picker: held shut but the list is empty -> opening it "
                     "instead\n");
        }
        g_bsOpen = 1;
    }
    else if (req == 2) bs_start_selected();
}

// Runs on the game thread (from the frame tick). Kept trivial when there is nothing to do.
// The work is the pump_* steps above, called in this order and only this order: they are the
// stages of one per-frame state machine, not independent features, and several of them read
// state a step above them wrote on the same tick.
void bs_pump(void) {
    DWORD now = GetTickCount();
    // First, before anything reads a deadline: a stall the game just came out of has to be given
    // back to the windows that cover a build, and the hooks that read them can fire before this
    // pump runs again. See frame_gap_watch.
    frame_gap_watch(now);
    touch_stall_probe(now);
    cs_restore_watch(now);
    sel_restore_watch(now);
    field_drop_watch(now);
    pause_stuck_watch(now);
    preempt_probe(overlay_in_battle(), g_bsStarted);
    pump_zone_bgm(now);
    pump_event_door(now);
    pump_decks(now);
    pump_event_giveup(now);
    pump_holds(now);
    pump_scene_light(now);
    pump_battle_bgm(now);
    pump_cover_lapse(now);
#ifdef FF13_DIAG
    form_watch();                      // ahead of the early return: a fight outlives the load
    enc_slots_watch();
    trk_watch(now);
    deck_sel_watch();
#endif
    if (pump_touch_due(now)) return;   // still loading the pick -- the steps below would read half of it
    pump_battle_report(now);
    pump_battle_live(now);
    pump_picker_input();
}
