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
static const int         g_bsPreloadMs = BS_PRELOAD_CAP_MS;
// The touch id and the "one is pending" fact must stay separate: object id 0 is a perfectly valid
// live object, so 0 cannot double as "none".
static volatile int      g_bsTouchDue = 0;
static volatile uint32_t g_bsTouchPending = 0;
static volatile DWORD    g_bsTouchAt = 0;
static volatile int      g_bsTouchReady = 0;
static volatile int      g_bsProbeMoved = 0;   // the probe has been seen to say no
static volatile int      g_bsProbeLogged = 0;
static char g_csRestore[40];                   // set to put back when the fight ends
volatile uint32_t g_bsStarted = 0;      // scene the picker last started
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
    for (int i = 0; i < g_bgmMapN; i++)
        fprintf(f, "bgm %u %s\n", g_bgmMap[i].btsc, g_bgmMap[i].track);
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
        } else if (!strcmp(kw, "bgm") && fscanf(f, "%u", &z) == 1) {
            char tr[BGM_NAME_MAX + 1] = "";
            // Same test on the way in, so a file written before that check existed repairs itself.
            if (fscanf(f, "%16s", tr) == 1 && tr[0] && !bgm_is_battle_track(tr)) {
                ff13logv("[FF13-SRP] btsc%05u: dropping remembered \"%.16s\" -- not a fight's "
                         "music\n", z, tr);
            } else if (tr[0] && g_bgmMapN < BGMMAP_MAX) {
                g_bgmMap[g_bgmMapN].btsc = z;
                strncpy(g_bgmMap[g_bgmMapN].track, tr, BGM_NAME_MAX);
                g_bgmMap[g_bgmMapN].track[BGM_NAME_MAX] = 0;
                g_bgmMapN++;
            }
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
// Do NOT try to steer the roll instead: the AI's unused-form pool could not be located in memory.
const uint8_t P_SPECREPL[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x20,0x02,0x00,0x00};
#define SPEC_NAME_LEN 16             // the game's own fixed key width, NUL-padded

// The fights that offer a form, and the charaspec each form is. Picker mode 1..n indexes `forms`;
// the label in btsc_picker.tsv and the charaspec installed here come from the same row.
// `acts` is the same forms one step upstream: the ability the AI runs to change form. Redirect the
// ABILITY, not just the charaspec -- a charaspec redirected on its own leaves the element the player
// sees unchanged, because that comes from the ability's own effects.
// `aim` names the part a buff should land on, for the fights where that is a separate question from
// which buff it is. NULL leaves the AI's own choice alone.
static const struct { uint16_t btsc; const char* base;
                      const char* forms[FORMS_MAX]; const char* acts[FORMS_MAX];
                      const char* aim[FORMS_MAX]; } kAiForms[] = {
    // Mode N means "the Nth entry here", and that binding is the only thing btsc_picker.tsv has to
    // keep. Row order in the file decides where a row is shown, so these can stay in any order.
    { 6046, "m097", { "m097_i",     "m097_f",     "m097_t",     "m097_w"     },   // ice / fire /
                    { "m097_ac510", "m097_ac500", "m097_ac520", "m097_ac530" },   // lightning / water
                    { NULL, NULL, NULL, NULL } },
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
                    { "m301",       "m303",       NULL,         NULL, NULL, NULL } },
};
#define AI_FORMS_N ((int)(sizeof(kAiForms)/sizeof(kAiForms[0])))

// How many choices this fight offers, 0 for "it has none".
static int aimode_count(uint32_t btsc) {
    for (int i = 0; i < AI_FORMS_N; i++) {
        if (kAiForms[i].btsc != btsc) continue;
        int n = 0;
        while (n < FORMS_MAX && kAiForms[i].acts[n]) n++;
        return n;
    }
    return 0;
}

// Long enough to cover the preload cap plus the minutes of fighting before the form comes up, short
// enough that a pin can never reach a later fight.
#define AIPIN_ARM_MS 300000
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
// The aim's own clock, started by the first target taken: the resolve can come long after the bend,
// so it cannot hang off PIN_ACT_MS. Holds one decision's pair; decisions are a minute apart.
#define AIM_HOLD_MS 3000
static volatile DWORD    g_formAimUntil = 0;  // when the aim stands down, 0 = it has not fired yet
// ...and aim_hold's state, declared here because aipin_arm clears it.
static volatile DWORD    g_aimHoldUntil = 0;
static volatile DWORD    g_aimHoldFrom  = 0;
static volatile int      g_aimHoldSaid  = 0;
// The battle's actor-id tag (see actor_id_slot); cleared here so one battle's never reaches another.
static volatile uint32_t g_actorTag   = 0;
static volatile int      g_actorTagOk = 0;

// Arm the pin for a fight the picker is about to start. mode 0 (or a fight with no forms) clears it,
// so a plain pick disarms whatever an earlier one left behind.
static void aipin_arm(uint32_t btsc, int mode) {
    g_formWant[0] = 0; g_formAct[0] = 0; g_formAim[0] = 0; g_formBase[0] = 0;
    g_formScene = 0; g_formMode = 0;
    g_formActSaid = 0; g_formActBent = 0; g_formSwapEnds = 0; g_formAimUntil = 0;
    g_aimHoldUntil = 0; g_aimHoldFrom = 0; g_aimHoldSaid = 0;
    g_actorTag = 0; g_actorTagOk = 0;   // the tag belongs to a battle; never carry one across
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
}

// The battle actor table. The index into it IS the actor id a conditional target is named by, so
// this is both how a part is found and how one is named.
#define RVA_BTL_ACTORS   0x23FD20C   // VA 0x27FD20C -- the battle actor manager (0x4ED600)
#define OFF_ACTOR_TABLE  0x13C       // -> array of actor pointers
#define ACTOR_SLOTS      0x13        // the bound 0x4ED600 itself checks the id against
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

// -> the actor table, or NULL.
static uint32_t* actor_table(void) {
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
// the high half is the battle's number (0 in a session's first fight, which is why a bare index
// ever seemed to work). Read with actor_id_slot, write back with the tag.
#define ACTOR_ID_TAG   0xFFFF0000u
#define actor_id_slot(id)  ((int)(int16_t)((uint32_t)(id) & 0xFFFFu))

// Learned from an id the game wrote; 0 is a real tag, hence the separate "seen one" flag.
static void actor_tag_learn(uint32_t id) {
    if ((int32_t)id < 0) return;                    // -1 = the field's empty state, no tag in it
    g_actorTag = id & ACTOR_ID_TAG;
    g_actorTagOk = 1;
}

void __cdecl on_cond_target(uint32_t ecx, uint32_t kind, uint32_t* pActorId, uint32_t* pOut) {
    (void)ecx; (void)pOut;
    if (kind != TGT_KIND_COND || !pActorId) return;
    if (!g_formBase[0]) return;                      // only while a pinned fight is up
    uint32_t* tbl = actor_table();
    if (!tbl) return;
    actor_tag_learn(*pActorId);
    int id = actor_id_slot(*pActorId);
    if (id < 0 || id >= ACTOR_SLOTS) return;
    uint32_t a = tbl[id];
    if (!a || IsBadReadPtr((void*)(uintptr_t)(a + OFF_AI_CONDTARGET), 4)) return;
    char nm[SPEC_NAME_LEN + 1];
    if (!actor_name_of(a, nm) || strncmp(nm, g_formBase, 3)) return;   // not this fight's cast
    int32_t tgtRaw = *(int32_t*)(uintptr_t)(a + OFF_AI_CONDTARGET);
    actor_tag_learn((uint32_t)tgtRaw);
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
        if (want >= 0 && want != tgt) {
            *(uint32_t*)(uintptr_t)(a + OFF_AI_CONDTARGET) = g_actorTag | (uint32_t)want;
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
    actor_tag_learn(was);
    // Never written before the tag has been seen: a bare index resolves but matches no id the game
    // compares against, and the tag is only knowable from one the game wrote.
    if (!g_actorTagOk) return;
    int wasSlot = ((int32_t)was < 0) ? -1 : actor_id_slot(was);
    if (wasSlot == want) return;
    *f = g_actorTag | (uint32_t)want;
    if (g_aimHoldSaid < AIM_WRITE_LOG) {
        g_aimHoldSaid++;
        char tn[SPEC_NAME_LEN + 1];
        tn[0] = 0;
        if (wasSlot >= 0 && wasSlot < ACTOR_SLOTS) actor_name_of(tbl[wasSlot], tn);
        ff13logv("[FF13-SRP] cond target: +%lums the field read %08X (slot %d%s%s) -- held on slot "
                 "%d (%s), tag %04X\n", (unsigned long)(now - g_aimHoldFrom), was, wasSlot,
                 tn[0] ? " = " : "", tn[0] ? tn : "", want, g_formAim,
                 (unsigned)(g_actorTag >> 16));
    }
}

// ---- the ability door, one step upstream of the charaspec -----------------------------------
// 0x4943B0 takes an ability label (a C string, [esp+4]) and queues that ability. Redirecting the
// label there swaps the whole mode-change action -- its effects and its presentation, not just the
// charaspec it happens to install.
const uint8_t P_ABILLABEL[9] = {0x55,0x8B,0xEC,0x81,0xEC,0xAC,0x00,0x00,0x00};

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

void __cdecl on_ability_label(uint32_t ecx, uint32_t label) {
    (void)ecx;
    if (!label) return;
    char cur[ACT_NAME_LEN + 1];
    if (!read_cstr_safe(label, cur, (int)sizeof cur)) return;
    if (!g_formBase[0]) return;
    size_t n = strlen(g_formBase);
    if (strncmp(cur, g_formBase, n) || cur[n] != '_') return;      // some other actor's ability
#ifdef FF13_DIAG
    // Only the mode-change ones: logging every ability this enemy reaches for buries them.
    if (strstr(cur, "_ac5"))
        ff13logv("[FF13-SRP] form pin: ability \"%s\"\n", cur);
#endif
    if (!g_formAct[0]) return;
    if ((int)(GetTickCount() - g_formUntil) >= 0) { g_formAct[0] = 0; return; }   // window lapsed
    // The pin normally stands down when the change lands, which for a shape-changer is the
    // charaspec swap. A fight that swaps nothing has no such moment, so the bend is also given a
    // life of its own: one decision long, so the next one is the game's own again.
    if (!g_formSwapEnds && g_formActBent && (int)(GetTickCount() - g_formActBent) >= 0) {
        ff13logv("[FF13-SRP] form pin: btsc%05u mode %d -- done; the abilities are the game's own "
                 "again\n", g_formScene, g_formMode);
        g_formAct[0] = 0; g_formActBent = 0;
        return;
    }
    int isForm = 0;
    for (int i = 0; i < AI_FORMS_N && !isForm; i++) {
        if (kAiForms[i].btsc != g_formScene) continue;
        for (int m = 0; m < FORMS_MAX; m++)
            if (kAiForms[i].acts[m] && !strcmp(cur, kAiForms[i].acts[m])) { isForm = 1; break; }
    }
    if (!isForm) return;
    char want[ACT_NAME_LEN + 1];
    strncpy(want, g_formAct, ACT_NAME_LEN);
    want[ACT_NAME_LEN] = 0;
    if (!strcmp(cur, want)) {                       // the game reached for it by itself
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
    if (!g_formActSaid) {
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
    (void)ecx;
    if (!name || IsBadWritePtr((void*)(uintptr_t)name, SPEC_NAME_LEN)) return;
    char* dst = (char*)(uintptr_t)name;
    char cur[SPEC_NAME_LEN + 1];
    memcpy(cur, dst, SPEC_NAME_LEN);
    cur[SPEC_NAME_LEN] = 0;
    for (int i = 0; cur[i]; i++)
        if ((unsigned char)cur[i] < 0x20 || (unsigned char)cur[i] > 0x7E) { cur[i] = 0; break; }
#ifdef FF13_DIAG
    ff13logv("[FF13-SRP] form pin: charaspec swap -> \"%s\"\n", cur);
#endif
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
static void tp_sample(void);        // reads the party's TP; defined with the refund work below
static void libra_sample(void);     // ...and the Datalog's Libra arrays, likewise
static int  libra_keeps(uint32_t btsc);      // ...and whether this fight is one that keeps its scan
static void libra_keep_take(uint32_t btsc);  // ...and holds what it scanned across the way back
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
    // An arena fight stages under the blackout cover, so root the party the way the story's own
    // staging does (ucOff) and shut the field menu -- otherwise the party can be walked off the mark
    // in the dark, or the pause menu opened over a party that is being rebuilt. Every picked fight,
    // not only arena ones; every way the start can end -- battle up, deadline, give-up, back on the
    // field -- lifts both.
    ctl_lock();
    g_evNoFightAt = 0; g_evSkipDoor = 0;   // a fresh pick always goes through the door first
    g_bsStarted = want; g_bsStartedAt = GetTickCount(); g_bsInFight = 0; g_bsSawEnemy = 0;
    g_touchAt = GetTickCount(); g_touchSeq = g_encIdSeq;   // see touch_stall_probe
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
    tp_sample();               // TP as it stands now: what the fight gets back afterwards
    // ...and what is known about the enemies. Taking no snapshot is what leaves a kept fight's scan
    // alone; any snapshot still pending from an earlier fight is dropped with it, because putting
    // THAT one back after this fight would undo this fight's scan by proxy.
#ifdef FF13_DIAG
    libra_diag_before(want);   // an independent snapshot, so the write is visible either way
#endif
    // Taken for every fight, kLibraKeep or not: for an ordinary fight it is what the scan is put
    // back TO, and for a kept one it is what the fight's own gain is measured against.
    libra_sample();
    g_homeHave = 0; g_homeDue = 0; g_homeAt = 0;
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
        strncpy(g_homeBgm, g_bgmFieldNow, BGM_NAME_MAX);
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
        return;
    }
    load_loose_specs(want, NULL);   // no set was loaded, so everything is missing
    // A fight that changes the party has one more thing to wait for that is in no scene's model
    // list: the member's own model. Handing the touch over at once put the member in the party but
    // not in the battle, and the game came down. The readiness probe cannot answer for a party
    // member either, so this is the full window rather than "until it is there".
    if (g_partyWho && g_partyWhoN > 0) {
        ff13logv("[FF13-SRP] battle picker: this fight brings its own party -- starting in %dms so "
                 "their models can arrive\n", g_bsPreloadMs);
        g_bsTouchPending = touch;
        g_bsTouchDue = 1;
        g_bsTouchAt = GetTickCount() + (DWORD)g_bsPreloadMs;
        return;
    }
    // A fight the story stages goes through the arena door whether or not a character set had to be
    // loaded first: the arena is chosen in bs_pump, on the pending touch, so hand it over as pending
    // with the wait already expired. (Standing in the fight's own zone used to lose the arena.)
    if (ar) {
        g_bsTouchPending = touch;
        g_bsTouchDue = 1;
        g_bsTouchAt = GetTickCount();          // nothing to load: due immediately
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
        if (g_ctlGiveBack) { g_ctlGiveBack = 0; ctl_force_on(); }
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
    if (!strcmp(g_curBg, g_homeBg)) return;
    ff13loga("[FF13-SRP] battle picker: the restart left the field on \"%s\" (the fight's own "
             "mapset) -- asking for \"%s\" back\n", g_curBg, g_homeBg);
    vm_load_bg(g_homeBg);
    // Ours does not update g_curBg (by design); after the heal the field really is on it again.
    strncpy(g_curBg, g_homeBg, sizeof g_curBg - 1);
    g_curBg[sizeof g_curBg - 1] = 0;
    g_homeBg[0] = 0;
}

// A checkpoint restart starts the music the STORY has for that checkpoint, and the end-of-fight
// restore only puts back tracks the mod started. Settled first: a chain speaks after FIELD_IDLE.
#define BGM_HEAL_SETTLE_MS 800
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
const uint8_t P_PLACEJOIN[5] = {0xB8,0x02,0x00,0x00,0x00};
#define PLACE_EBP_OFF  0x520         // ebp = esp + this, at that instruction
#define PLACE_OUT_OFF  0x70          // ...and the matrix is at ebp - this
#define PLACE_THIS_OFF 0x51C
static volatile int g_placeMatrix = 1;
static volatile int g_placeMatN = 0;
volatile int g_bossGotMatrix = 0;
static volatile int g_lightDone = 0;         // ...and the fight has been lit once they exist

void __cdecl on_place_join(uint32_t esp) {
    if (!g_placeMatrix || !g_placeBoss) return;          // only while we are staging an arena fight
    uintptr_t f = (uintptr_t)esp + PLACE_EBP_OFF;        // = ebp
    if (IsBadReadPtr((void*)(f - PLACE_THIS_OFF), 4)) return;
    uint32_t self = *(uint32_t*)(f - PLACE_THIS_OFF);
    if (!self || IsBadReadPtr((void*)(uintptr_t)(self + OFF_ACTOR_NAME), 16)) return;
    if (IsBadWritePtr((void*)(f - PLACE_OUT_OFF), 0x40)) return;
    float* out = (float*)(f - PLACE_OUT_OFF);
    // The one the lookup could not answer for. A character always has a place, so this is the
    // enemy the fight never registered.
    if (out[12] != 0.0f || out[13] != 0.0f || out[14] != 0.0f) return;
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

// Fights whose Libra gain is KEPT. Where the scan is the thing being practised, undoing it is
// backwards. Asked for by name, since which fights these are is a property of the fights rather
// than a preference.
static const uint16_t kLibraKeep[] = { 7037 };
static int libra_keeps(uint32_t btsc) {
    for (size_t i = 0; i < sizeof(kLibraKeep)/sizeof(kLibraKeep[0]); i++)
        if (kLibraKeep[i] == (uint16_t)btsc) return 1;
    return 0;
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

#ifdef FF13_DIAG
// What a fight actually WROTE into the Datalog, by species index: the put-back is a net effect and
// says nothing about which species moved, which is the question when two fights field what looks
// like the same enemy (the game keeps four separate monster_book entries for m019). Taken
// independently of the snapshot the put-back uses, so it works for a fight in kLibraKeep too.
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

static void touch_stall_probe(DWORD now) {
    if (!g_touchAt) return;
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
    // A nonzero [+0x234] is momentary in ordinary play (it flickers several times a second), so one
    // that has outlasted the whole stall window is wedged -- measured after winning btsc04026,
    // whose STORY continues into a second fight: the interrupted follow-up staging left the slot
    // set, and the game then threw every later touch away (picked or walked into alike). Fold the
    // staging and give the field back promptly instead of holding the lock for the long give-up.
    // Do NOT write 0 into the slot: measured, that un-wedges the touch gate while the staging
    // behind it stays inconsistent, and the re-accepted touch then hangs the zone in state 5.
    if (ok && val && g_bsStarted) {
        cam_disarm();
        flash_say(ui_text(T_PICK_NEVER_STARTED));
        ff13loga("[FF13-SRP] battle picker: the pending-touch slot is wedged (a won fight whose "
                 "story continues?) -- giving the field back; reload the save if fights stay "
                 "unstartable\n");
        g_bsStarted = 0;
        g_csRestore[0] = 0;
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
        items_give_back();
        tp_give_back();
#ifdef FF13_DIAG
        libra_diag_point("after the way back", 1);   // ...and before the put-back undoes anything
#endif
        libra_put_back();
        libra_keep_apply();      // ...and for a kept fight, put its own gain back instead
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
                     "locked=%d/%d\n", leader_name(who) ? who : "?", have ? "" : "?",
                     (double)px, (double)py, (double)pz, g_ctlLocked, g_ctlN);
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
            if (bgm_is_outcome_track(cur)) {
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
        // The probe is only believed once it has actually been seen to change: Resource.isLoadedChara
        // reports that a charaspec is DEFINED, not that its model is in memory, so an answer of
        // "present" on the very first poll is not a load test. Fall back to the full wait then.
        if (ready == 1 && !g_bsProbeMoved) {
            if (!g_bsProbeLogged) {
                g_bsProbeLogged = 1;
                ff13logv("[FF13-SRP] battle picker: readiness probe said yes immediately -- not a "
                         "load test; waiting the full %dms instead\n", g_bsPreloadMs);
            }
            ready = -1;                                // treat as "cannot tell"
        }
        if (ready == 0) g_bsProbeMoved = 1;            // it has been seen saying no: it is real
        if (ready != 1 && !timeUp) return 1;             // still loading, or cannot tell -- wait
        if (ready == 1 && !g_bsTouchReady) {
            g_bsTouchReady = 1;
            ff13logv("[FF13-SRP] battle picker: models resident after %ums\n",
                     (unsigned)(now - (g_bsTouchAt - (DWORD)g_bsPreloadMs)));
        }
        if (ready == 0)
            ff13logv("[FF13-SRP] battle picker: \"%s\" still not resident after %dms -- starting "
                     "anyway\n", missing ? missing : "?", g_bsPreloadMs);
        loose_specs_report();     // the hand-fetched ones, now that the waiting is over
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
        // read back our own choice. Only boss/event/story fights are remembered.
        {
            char cur[BGM_NAME_MAX + 1];
            const ff13_btsc_info* bi = btsc_info(scene);
            int worth = (bi && (bi->kind & (FF13_BTSC_BOSS | FF13_BTSC_EVENT))) || is_boss_scene(scene);
            // ...but never from a fight the game itself started for us. An event door hands the
            // story its chain back when the fight ends, and the chain plays its own music -- so
            // what is running by the time this asks is the next thing, not this fight's.
            {
                const struct ff13_arena_s* ea = scene_arena(scene);
                if (ea && ea->ev[0]) worth = 0;
            }
            if (!g_bgmWant[0] && bgm_current(cur) && cur[0]) {
                ff13logv("[FF13-SRP] btsc%05u battle music: \"%.16s\"%s\n", scene, cur,
                         bgm_is_battle_track(cur)
                         ? "" : "  (not a battle arrangement -- carried in from a cutscene?)");
                if (worth) bgm_learn(scene, cur);
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
            if (!g_bsInFight) ctl_unlock();    // the battle owns input now; the field gets its back
            g_bsInFight = 1;
            // Any targetable enemy HP means something spawned. Timing cannot be used here: with
            // one-hit-kill or game-speed on, a perfectly normal fight is over in a few seconds.
            if (read_via_ptr(RVA_TARGETHP_PTR, OFF_TARGETHP) > 0) g_bsSawEnemy = 1;
        }
        else if (g_bsInFight) {
            uint32_t fought = g_bsStarted;
            // Only when the game itself said nothing -- its own count is better than this guess.
            // Same reason as mark_field_result above: a fight we emptied ourselves is not evidence
            // about what this zone can field.
            if (!g_bsSawEnemy && !g_iwHitThisFight && !field_rec(g_bsStarted, NULL, NULL))
                mark_empty_scene(g_bsStarted);
            g_bsStarted = 0; g_bsInFight = 0; g_bsSawEnemy = 0;
#ifdef FF13_DIAG
            // Read here as well as after the way back: this is the last moment before the restart,
            // so a scan that is present now and gone later was taken by the restart, not by us.
            libra_diag_point("as the fight ended", 0);
#endif
            // ...which is exactly why a kept fight's scan is read HERE and written again later.
            if (libra_keeps(fought)) libra_keep_take(fought);
            if (g_homeHave) { g_homeDue = 1; g_homeAt = 0; }   // and the same for where you stood
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
            DWORD tidyAt = GetTickCount(), csMs = 0, bgmMs = 0;
            if (g_csRestore[0]) {
                ff13log("[FF13-SRP] battle picker: fight over -> character set back to \"%s\"\n",
                         g_csRestore);
                DWORD at = GetTickCount();
                vm_load_charaset(g_csRestore);
                csMs += GetTickCount() - at;
                g_csRestore[0] = 0;
                g_csLoaded[0] = 0;
                g_looseLoaded = 0;         // the set replaces what was loaded, loose ones included
            } else if (g_looseLoaded && g_curCharset[0]) {
                // A fight whose enemies came one at a time rather than as a set had nothing to put
                // back, so those models simply stayed loaded -- in the one zone that streams its
                // ground a cell at a time and has the least room to spare. Loading the field's own
                // set again takes them out, since a set replaces what is resident.
                ff13log("[FF13-SRP] battle picker: fight over -> reloading \"%s\" to take out the "
                         "models this fight asked for one at a time\n", g_curCharset);
                DWORD at = GetTickCount();
                vm_load_charaset(g_curCharset);
                csMs += GetTickCount() - at;
                g_looseLoaded = 0;
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
            {
                DWORD tidyMs = GetTickCount() - tidyAt;
                if (tidyMs >= 32)
                    ff13log("[FF13-SRP] battle picker: fight over -> the tidy-up held the frame for "
                             "%lums (character set %lums, music %lums)\n",
                             (unsigned long)tidyMs, (unsigned long)csMs, (unsigned long)bgmMs);
            }
        } else if (GetTickCount() - g_bsStartedAt > BS_GIVEUP_MS) {
            // A pick that never becomes a fight leaves the event lock on, and the event lock is what
            // makes the party unable to walk -- so this is the way out of being stuck, not a
            // tidy-up. The staging waits are 8s at the outside, so a fight that has not started in
            // twenty is not coming.
            cam_disarm();
            ff13logv("[FF13-SRP] battle picker: btsc%05u never became a fight in %dms -- giving the "
                     "field back\n", g_bsStarted, BS_GIVEUP_MS);
            flash_say(ui_text(T_PICK_NEVER_STARTED));
            g_bsStarted = 0;                   // never got into a fight; forget it
            g_csRestore[0] = 0;
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
#endif
    if (pump_touch_due(now)) return;   // still loading the pick -- the steps below would read half of it
    pump_battle_report(now);
    pump_battle_live(now);
    pump_picker_input();
}
