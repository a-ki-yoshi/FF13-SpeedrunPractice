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

// ============================ BATTLE SWAP (fight a different enemy set) ======
// Encounters name their enemy set by "btsc%05d". The scripted entry startBattle_l (0xBE3DF0)
// reads its arguments off the VM operand stack, not the C stack: [vmctx+0x114] -> an array of
// 8-byte tagged slots {tag @+0, value @+4}, and it pops slot0 = btsc name (char*), slot1 = flags,
// slot2 = scene number before doing anything else. So the entry hook only has to overwrite
// slot0.value; the substitute string must live in this DLL's data segment, because the game copies
// it into the encounter-info struct.
// ASSET LIMIT: the btsc must already be loaded in the DataBattle DB, or getBattleScene (0xB41640)
// fails. RE: common/docs/EVENT_BATTLE_RE.md EN-0..EN-6.
#define SB_ARGSTACK_OFF 0x114        // [vmctx+0x114] = VM operand-stack pointer
#define SB_SLOT         8            // one tagged VM slot: {tag @+0, value @+4}
// push ebp; mov ebp,esp; sub esp,0x290
const uint8_t P_SB[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x90,0x02,0x00,0x00};

char g_swapNameBuf[32] = "";               // normalized "btscNNNNN" (the VM-name path)
volatile uint32_t g_swapBtscId = 0;        // the same target as a number (the symbol path)
volatile unsigned char g_swapEnabled = 0;
// While this tick is in the future, getBattleScene lookups are redirected to g_swapBtscId. Opened
// when an encounter starts building; one encounter performs several lookups (7 on hardware).
volatile DWORD g_swapWindowUntil = 0;
// 15s, not 5: a boss fight's build takes longer than an ordinary encounter's, and a window that
// closes mid-build leaves later lookups reading the original scene.
#define SWAP_WINDOW_MS 15000
// The music path is independent of the battle scene. onPlay_ reads a FIXED 16-byte inline name
// buffer, so any substitute name must be zero-padded to that size.
static volatile int g_bgmLogN = 0;
// Learned pairs: which track a battle scene played when it was fought for real.
struct ff13_bgm_pair g_bgmMap[BGMMAP_MAX];
volatile int      g_bgmMapN = 0;
static volatile uint32_t g_bgmLearnFor = 0;   // btsc whose battle track we are waiting to hear
// A lead-in cutscene's track passes every battle-track test, so the learner keeps the LAST track
// that started and commits once the fight has audibly been running (bgm_learn_tick).
static char              g_bgmLearnCand[BGM_NAME_MAX + 1];
// Story boss fights are started by name and have no scene number to pair with, so what is kept
// from them is the TRACK. ORDER: the theme plays during the run-up and startBattle_l is called
// AFTER it, so arming a "learn the next track" flag there is too late -- every track is stamped as
// it plays and startBattle_l takes the one playing just before it.
static volatile unsigned char g_bgmLearnScripted = 0;
// ...and only for a few seconds: without a deadline the flag was still armed when the FIELD theme
// returned at the end of the fight and overwrote the learned boss track with it.
#define BGM_LEARN_WINDOW_MS 8000
static volatile DWORD g_bgmLearnUntil = 0;
static char g_bgmScripted[BGM_NAME_MAX + 1] = "";
static char g_bgmLastPlayed[BGM_NAME_MAX + 1] = "";
// The current zone's own two default tracks, read out of the game (see bgm_zone_refresh).
char g_zoneField[BGM_NAME_MAX + 1] = "";
static char g_zoneBattle[BGM_NAME_MAX + 1] = "";
// The generic boss theme: a fallback that always sounds, since lazyPlay can start any track.
#define BGM_MISSION      "music_40monst_b"  // measured; see the mission branch below
#define BGM_DEFAULT_BOSS "music_bossa"

static const ff13_bgm_scene* bgm_scene_row(uint32_t btsc) {
    for (int i = 0; i < FF13_BGM_SCENE_N; i++)
        if (kBgmScene[i].btsc == btsc) return &kBgmScene[i];
    return NULL;
}

static volatile DWORD g_bgmLastPlayedAt = 0;
#define BGM_LOOKBACK_MS 60000
// What a picked fight switches to, and the ordering that gets it there. ORDER MATTERS: let the
// game start its own battle theme first, then ask for ours from a later frame tick. Asking for
// both at once cancels out -- lazyPlay notifies onPlay_ (0xACE7F0) immediately, so our track is
// recorded as "playing" before its load finishes and both requests come to nothing.
// The hold delays ours further, because the game asks for its battle theme BEFORE the battle state
// comes up (see on_bgm_play). It is a silent gap by design: the ordinary theme is stopped at the
// front of it and kept stopped.
#define BGM_HOLD_DEFAULT_MS 1200
char g_bgmWant[BGM_NAME_MAX + 1] = "";     // the track a picked fight wants
volatile DWORD g_bgmWantUntil = 0;
volatile unsigned char g_bgmDue = 0;       // the game's battle theme started; ours is next
char g_bgmTheirs[BGM_NAME_MAX + 1] = "";   // ...and what that theme was, so it can be stopped
volatile int   g_bgmHoldMs = BGM_HOLD_DEFAULT_MS;   // [battle_swap] bgm_delay_ms
volatile DWORD g_bgmHoldUntil = 0;         // ...theirs is stopped, ours is due at this tick
volatile unsigned char g_bgmSkipHold = 0;  // this hand-off is already late; play at once
char g_bgmHushed[BGM_NAME_MAX + 1] = "";   // last track the hold silenced, to log it once
// ...and afterwards watch what the manager reports playing: a request that goes nowhere leaves the
// previous track running rather than falling back to silence, so a failure is otherwise invisible.
volatile DWORD g_bgmWatchUntil = 0;
char g_bgmWatchLast[BGM_NAME_MAX + 1] = "";

// Not a battle track: the game-over jingle, the fanfare and the results screen (a name learned
// from one of those produced silence when replayed), nor the field theme.
int bgm_is_battle_track(const char* n) {
    if (!n || strncmp(n, "music_", 6) != 0) return 0;
    if (g_zoneField[0] && strncmp(n, g_zoneField, BGM_NAME_MAX) == 0) return 0;
    static const char* const kNot[] = { "gameover", "fanfare", "result", "title", "ending" };
    for (size_t i = 0; i < sizeof(kNot)/sizeof(kNot[0]); i++)
        if (strstr(n, kNot[i])) return 0;
    return 1;
}
// How a fight ENDS. The hold must not silence these (one-hit-kill ends a fight well inside it).
// Deliberately NOT bgm_is_battle_track's inverse: that one passes a field theme as a battle track,
// and a field theme is something the hold does have to take away.
int bgm_is_outcome_track(const char* n) {
    if (!n || !n[0]) return 0;
    static const char* const kEnd[] = { "gameover", "fanfare", "result" };
    for (size_t i = 0; i < sizeof(kEnd)/sizeof(kEnd[0]); i++)
        if (strstr(n, kEnd[i])) return 1;
    return 0;
}
// A boss theme specifically: remembering the zone's ordinary battle track as "the boss music here"
// makes every picked fight sound ordinary.
static int bgm_is_boss_track(const char* n) {
    if (!bgm_is_battle_track(n)) return 0;
    if (g_zoneBattle[0] && strncmp(n, g_zoneBattle, BGM_NAME_MAX) == 0) return 0;
    return 1;
}

static const char* bgm_lookup(uint32_t btsc) {
    for (int i = 0; i < g_bgmMapN; i++) if (g_bgmMap[i].btsc == btsc) return g_bgmMap[i].track;
    return NULL;
}
static const char* bgm_pick_track3(uint32_t btsc, const char** origin, const char** tag,
                                   int useLearned);
void bgm_learn(uint32_t btsc, const char* track) {
    if (!btsc || !track || !track[0]) return;
    // Sampling the manager at the end of a fight catches the game-over jingle, or the field theme
    // once a checkpoint restart has put it back.
    if (!bgm_is_battle_track(track)) {
        ff13logv("[FF13-SRP] btsc%05u: \"%.16s\" is not a fight's music -- not remembered\n",
                 btsc, track);
        return;
    }
    // EXCEPTIONS only -- 48 slots against a playthrough's hundreds of fights: a track the rules
    // below already answer is not stored, and a stored entry they catch up with is retired.
    const char* dorigin = NULL;
    const char* derived = bgm_pick_track3(btsc, &dorigin, NULL, 0);
    if (derived && strncmp(derived, track, BGM_NAME_MAX) == 0) {
        for (int i = 0; i < g_bgmMapN; i++) {
            if (g_bgmMap[i].btsc != btsc) continue;
            g_bgmMap[i] = g_bgmMap[--g_bgmMapN];
            ff13logv("[FF13-SRP] btsc%05u: \"%.16s\" now matches the derived answer -- learned "
                     "entry retired\n", btsc, track);
            zb_save();
            return;
        }
        ff13logv("[FF13-SRP] btsc%05u: \"%.16s\" matches the derived answer%s -- not stored\n",
                 btsc, track, dorigin ? dorigin : "");
        return;
    }
    for (int i = 0; i < g_bgmMapN; i++) {
        if (g_bgmMap[i].btsc != btsc) continue;
        if (strncmp(g_bgmMap[i].track, track, BGM_NAME_MAX) == 0) return;
        ff13logv("[FF13-SRP] btsc%05u now plays \"%.16s\" (was \"%.16s\")\n",
                 btsc, track, g_bgmMap[i].track);
        strncpy(g_bgmMap[i].track, track, BGM_NAME_MAX);
        g_bgmMap[i].track[BGM_NAME_MAX] = 0;
        zb_save();
        return;
    }
    if (g_bgmMapN >= BGMMAP_MAX) return;
    g_bgmMap[g_bgmMapN].btsc = btsc;
    strncpy(g_bgmMap[g_bgmMapN].track, track, BGM_NAME_MAX);
    g_bgmMap[g_bgmMapN].track[BGM_NAME_MAX] = 0;
    g_bgmMapN++;
    ff13logv("[FF13-SRP] learned: btsc%05u plays \"%.16s\" (%d pairs known, kept across launches)\n",
             btsc, track, g_bgmMapN);
    zb_save();
}
static void bgm_learn_close(void) {
    if (g_bgmLearnFor && g_bgmLearnCand[0]) bgm_learn(g_bgmLearnFor, g_bgmLearnCand);
    g_bgmLearnFor = 0; g_bgmLearnCand[0] = 0;
}
static void bgm_learn_note(const char* name) {
    if (!g_bgmLearnFor || strncmp(g_bgmLearnCand, name, BGM_NAME_MAX) == 0) return;
    strncpy(g_bgmLearnCand, name, BGM_NAME_MAX);
    g_bgmLearnCand[BGM_NAME_MAX] = 0;
    ff13logv("[FF13-SRP] learn candidate: btsc%05u \"%.16s\"\n", g_bgmLearnFor, name);
}
void bgm_learn_tick(DWORD now) {
    if (!g_bgmLearnFor) return;
    // Late enough that the fight's own track has replaced a lead-in cutscene's; early enough that
    // a mid-fight phase change usually has not happened yet.
    if (overlay_in_battle() && g_btlStatAt && (int)(now - (g_btlStatAt + 3000)) >= 0)
        bgm_learn_close();
}

volatile uint32_t g_lastBtscId = 0;        // last btsc ID asked for (field-symbol path)
volatile uint32_t g_lastTouchedId = 0;     // last touched field-object id (battle picker)
volatile uint32_t g_fieldMgr = 0;          // doEncountCheck's ecx (owns the pending-touch slot)
static volatile uint32_t g_curZone = (uint32_t)-1; // watched so per-zone state is dropped on a move
static volatile int g_sbLogN = 0;                 // cap the log lines

// __cdecl handler (ff13_install_stackarg_hook with stackOff=4, so it receives arg1 = the VM
// context). Reads the three pending VM arguments and, while armed, redirects the btsc name.
// popad restores registers only, so the value we write into the VM operand stack survives.
void __cdecl on_start_battle(uint32_t vmctx) {
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadWritePtr((void*)(uintptr_t)args, 3 * SB_SLOT)) return;

    uint32_t*   pName = (uint32_t*)(uintptr_t)(args + 4);              // slot0.value = btsc name
    const char* name  = (const char*)(uintptr_t)*pName;
    uint32_t    flags = *(uint32_t*)(uintptr_t)(args + SB_SLOT + 4);   // slot1.value
    uint32_t    arg3  = *(uint32_t*)(uintptr_t)(args + 2 * SB_SLOT + 4); // slot2.value
    int nameOk = (name && !IsBadReadPtr((void*)name, 16));

    // Take the track that was already playing (the boss theme starts during the run-up), and also
    // arm the forward case for scripted fights that start their music later.
    if (g_bgmLastPlayed[0] && (GetTickCount() - g_bgmLastPlayedAt) < BGM_LOOKBACK_MS
        && bgm_is_boss_track(g_bgmLastPlayed)) {
        strncpy(g_bgmScripted, g_bgmLastPlayed, BGM_NAME_MAX);
        g_bgmScripted[BGM_NAME_MAX] = 0;
        ff13logv("[FF13-SRP] learned scripted-battle track: \"%.16s\" (usable by picked fights)\n",
                 g_bgmScripted);
    }
    g_bgmLearnScripted = 1;
    g_bgmLearnUntil = GetTickCount() + BGM_LEARN_WINDOW_MS;
    if (arg3 && arg3 < 13000) boss_learn(arg3, nameOk ? name : NULL);
    int swapped = 0;
    if (g_swapEnabled && g_swapNameBuf[0]) {
        *pName = (uint32_t)(uintptr_t)g_swapNameBuf;
        swapped = 1;
        g_swapEnabled = 0;                       // one-shot: the NEXT encounter is vanilla again
    }
    if (g_sbLogN < 200) {
        g_sbLogN++;
        // arg3 is the scene number; the string argument is the arena/script name, not the enemy set.
        ff13logv("[FF13-SRP] scripted battle: btsc%05u  (arena \"%.20s\", flags=0x%X)%s\n",
                 arg3, nameOk ? name : "(unreadable)", flags,
                 swapped ? "  [name substituted]" : "");
    }
}

// ---- THE field-encounter path (what ordinary enemy symbols actually use) -------------------
// Ordinary encounters do NOT reach startBattle_l (that is the scripted entry). They go through
// doEncountCheck (0x5CEE00) -> 0xB500A0(id) -> 0xB50270(id) on the encounter manager [0x282B190],
// which resolves the battle-scene number and stores it:
//     0xB5031E   call 0xB52BB0(this+0x230)      ; resolve the battle scene for this symbol
//     0xB50326   mov ecx, [esp+0x1E8]           ; ecx = the manager
//     0xB5032D   mov [ecx+0x354], eax           ; <- store it. THE hook site; unique in the image.
// The cave calls the handler with the original id and writes the handler's return value over the
// EAX slot pushad saved ([esp+0x20] there), so popad restores OUR id and the game's own store
// writes it. Same asset limit as above: the scene must already be loaded.
// RE: /workspace/.logs/toolH_battleswap_impl.md.
const uint8_t P_ENC[6] = {0x89,0x81,0x54,0x03,0x00,0x00};
void* g_encCave = NULL;

// __cdecl handler: gets the battle-scene id the game resolved, returns the id to actually use.
static int btsc_is_loaded(uint32_t id);          // defined with the collection hooks below
// Scene ids are "btsc%05d" and the highest band in the data is 30xxx, so anything above this is
// not a scene -- most often -1, which is what a touch on a non-enemy object resolves to.
#define BTSC_ID_MAX 65535

uint32_t __cdecl on_encount_scene(uint32_t origId) {
    g_lastBtscId = origId;
    uint32_t useId = origId;
    int swapped = 0, notLoaded = 0;
    if (g_swapEnabled && g_swapBtscId) {
        // Refuse to substitute a scene the DataBattle DB does not hold: that is what silently
        // produced "nothing changed" on hardware, and it keeps a bad ini from wedging a fight.
        if (btsc_is_loaded(g_swapBtscId)) {
            useId = g_swapBtscId;
            swapped = 1;
            // Open the lookup-rewrite window: the store below only reaches some of the paths that
            // build the fight, so the rest are caught at getBattleScene itself.
            g_swapWindowUntil = GetTickCount() + SWAP_WINDOW_MS;
            g_swapEnabled = 0;                   // one-shot: the NEXT encounter is vanilla again
        } else {
            notLoaded = 1;                       // stay armed: another area may load it
        }
    }
    if (g_sbLogN < 200) {
        g_sbLogN++;
        if (swapped)
            ff13logv("[FF13-SRP] field encounter btsc%05u -> SWAPPED to btsc%05u  (swap now OFF)\n",
                     origId, useId);
        else if (notLoaded)
            ff13logv("[FF13-SRP] field encounter btsc%05u -- btsc%05u is NOT loaded here, swap "
                     "skipped (still armed)\n", origId, g_swapBtscId);
        else
            ff13logv("[FF13-SRP] field encounter btsc%05u\n", origId);
    }
    // Only for a fight the game itself brought about: a substituted scene plays the zone's
    // ORDINARY battle theme, and learning that as the boss's track poisons the pair table.
    g_bgmLearnFor = swapped ? 0 : useId;
    g_bgmLearnCand[0] = 0;
    // Remember the band and the touch id, so the picker works in this zone on later launches.
    // Guarded: a non-enemy touch resolves to -1, and learning that as the band left the picker
    // permanently empty (the record persists across launches).
    if (origId && origId <= BTSC_ID_MAX)
        zb_learn(field_zone_id(), origId / 1000, g_lastTouchedId);
    return useId;
}

// ---- collection hooks: where the enemy set REALLY comes from -------------------------------
// Read-only, capped logging: getBattleScene (0xB41640) logged WITH ITS CALLER, and 0xB500A0(id),
// the touched-object id doEncountCheck hands the encounter manager. They change nothing.
const uint8_t P_GBS[9]  = {0x55,0x8B,0xEC,0x81,0xEC,0xBC,0x00,0x00,0x00};
const uint8_t P_ENCID[7] = {0x55,0x8B,0xEC,0x8B,0x45,0x08,0x50};
// push ebp ; mov ebp,esp ; cmp dword [0x27FD208],0  -- the whole of startBattle's prologue, 10 bytes
const uint8_t P_SBTL[10] = {0x55,0x8B,0xEC,0x83,0x3D,0x08,0xD2,0x7F,0x02,0x00};
static volatile int g_gbsLogN = 0, g_encIdLogN = 0;

// ---- VM natives -----------------------------------------------------------------------------
// They take their arguments off the VM operand stack ([vmctx+0x114], 8-byte tagged slots), exactly
// like startBattle_l. loadSoundResource pops TWO: (String resource, String group), the group being
// a sound bus name. The read-only hook below logs the real argument pair.
const uint8_t P_LSND[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x70};
static volatile int g_lsndLogN = 0;
static volatile unsigned char g_inSndLoad = 0;    // reentrancy guard: our own call

// Called with a FORGED VM context: the native reads its arguments only through [ctx+0x114] and
// ADVANCES that pointer, which is why the context must be ours and not the game's. It touches no
// other field and ends in a plain `ret` -> cdecl. Groups seen: "general" music, "CategoryFieldSe".
typedef void (__cdecl *vmNative1_t)(void* ctx);

void vm_load_sound(const char* name, const char* group) {
    if (!g_base || !name || !group) return;
    uint8_t   ctx[0x200];
    uint32_t  args[4];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)(uintptr_t)name;    // slot0 {tag, value}
    args[2] = 0; args[3] = (uint32_t)(uintptr_t)group;   // slot1 {tag, value}
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    g_inSndLoad = 1;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_LOADSND))(ctx);
    g_inSndLoad = 0;
}

// Same collection idea for character sets: log what the game itself asks for.
const uint8_t P_LCS[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x34};
static volatile int g_lcsLogN = 0;

char g_curCharset[CSET_NAME_MAX + 1];      // the set the GAME last asked for
static volatile unsigned char g_inCsLoad = 0;     // our own call, so it is not mistaken for the game's
char g_csLoaded[CSET_NAME_MAX + 1];        // the set WE asked for, until it is put back

// A script to run by name on the first VM thread that comes past, and the window it is good for.
// Armed when a door fires, spent once. See vm_exec_script for why it cannot simply be called.
const char* volatile g_setupWant = NULL;
volatile DWORD g_setupUntil = 0;
volatile DWORD g_gameTid = 0;              // for the log: which thread the picker runs on
static int  vm_script_exists(const char* name);
static void vm_exec_script(const char* name);
static void setup_spend(int ours);   // defined just below on_load_charaset
static void party_log(const char* when);

void __cdecl on_load_charaset(uint32_t vmctx) {
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, SB_SLOT)) return;
    const char* n = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(args + 4);
    int ok = n && !IsBadStringPtrA(n, CSET_NAME_MAX + 1);
    // Only the game's own requests are remembered: putting the fight's set back afterwards needs
    // the name that was there before we interfered.
    if (ok && !g_inCsLoad) {
        strncpy(g_curCharset, n, CSET_NAME_MAX);
        g_curCharset[CSET_NAME_MAX] = 0;
    }
    if (g_lcsLogN < 60) {
        g_lcsLogN++;
        ff13logv("[FF13-SRP] loadCharaByCharaSet(\"%.39s\")%s\n", ok ? n : "(unreadable)",
                 g_inCsLoad ? "  <- ours" : "");
    }
    // Not on our own call: that one is made from the game thread, which is what this exists to
    // get out of.
    setup_spend(g_inCsLoad);
}

// Spend the armed script on whichever native the chain reaches first -- NOT on one particular
// native: a chain whose set is already resident never calls loadCharaByCharaSet at all.
static void setup_spend(int ours) {
    if (g_setupWant && !ours) {
        const char* s = (const char*)g_setupWant;
        g_setupWant = NULL;                          // before the call: it will come back through
        if ((int)(GetTickCount() - g_setupUntil) >= 0) {
            ff13logv("[FF13-SRP] battle picker: too late to run \"%.31s\" -- the chain never called "
                     "a native we watch\n", s);
        } else {
            ff13logv("[FF13-SRP] battle picker: running \"%.31s\" from the chain's own thread "
                     "(tid %lu, the picker's is %lu), script %s\n", s,
                     GetCurrentThreadId(), g_gameTid,
                     vm_script_exists(s) ? "registered" : "NOT REGISTERED");
            vm_exec_script(s);
            party_log("after the setup script");
        }
    }
}

// One VM slot and nothing else off the context, so a forged one is enough.
void vm_load_charaset(const char* name) {
    if (!g_base || !name || !name[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)(uintptr_t)name;      // slot0 {tag, value}
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    g_inCsLoad = 1;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_LOADCHARASET))(ctx);
    g_inCsLoad = 0;
}

// ...and ask for ONE character, the only way to reach the ones no set lists (loading a SET is
// all-or-nothing and evicts what was resident; this is not). Resource.loadChara(String) has the
// same shape as loadCharaByCharaSet, so the same forged context serves.
const uint8_t P_LCH[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x30};
volatile int g_lchOk = 0;     // the bytes there are the function we read

// Resource.isLoadedChara returns NOTHING IN EAX: it pops its argument slot and pushes
// {tag 1, result} back into it, so the answer comes back where the argument went out.
const uint8_t P_ILC[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x34};
volatile int g_ilcOk = 0;
static volatile uint32_t g_looseScene = 0;   // whose loose specs are still to be reported on
volatile int g_looseLoaded = 0;       // ...and whether any were actually asked for

static void vm_load_chara(const char* spec) {
    if (!g_base || !g_lchOk || !spec || !spec[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)(uintptr_t)spec;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_LOADCHARA))(ctx);
}

static int vm_chara_loaded(const char* spec) {
    if (!g_base || !g_ilcOk || !spec || !spec[0]) return -1;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)(uintptr_t)spec;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_ISLOADEDCHARA))(ctx);
    return args[1] ? 1 : 0;                   // the slot the argument was in, now the answer
}

// Per-scene charaspecs no generated table can know about, because build_charaset_table.py only
// covers a scene's ENEMY list. Hand-owned here: the generator would drop these on its next run.
// Loaded AFTER the character set, never before -- a set arriving later would evict them.
static const struct { uint16_t btsc; const char* spec; } kSceneExtraSpecs[] = {
    { 7010, "s101" },        // Odin: the cutscene's actor player asks for these two, and no
    { 7010, "s102" },        // character set in this chapter holds either
};
#define FF13_EXTRA_SPEC_N ((int)(sizeof(kSceneExtraSpecs)/sizeof(kSceneExtraSpecs[0])))

void load_loose_specs(uint32_t btsc, const ff13_charaset* cs) {
    int n = 0;
    for (int i = 0; i < FF13_EXTRA_SPEC_N; i++) {
        if (kSceneExtraSpecs[i].btsc != btsc) continue;
        vm_load_chara(kSceneExtraSpecs[i].spec);
        n++;
        ff13logv("[FF13-SRP] battle picker: \"%s\" is on this fight's party side -- asked for it "
                 "directly\n", kSceneExtraSpecs[i].spec);
    }
    for (int i = 0; i < FF13_LOOSE_N; i++) {
        if (kLooseSpecs[i].btsc != btsc) continue;
        vm_load_chara(kLooseSpecs[i].spec);
        n++;
        ff13logv("[FF13-SRP] battle picker: \"%s\" is in no character set -- asked for it "
                 "directly\n", kLooseSpecs[i].spec);
    }
    // ...and whatever the set that WAS loaded does not hold. Some fights are covered by no single
    // set, so the search fails and nothing at all is loaded; one at a time has no such limit.
    // cs is the set being loaded, or NULL when none was found.
    const ff13_scene_models* sm = scene_models(btsc);
    if (sm) {
        for (int i = 0; i < sm->n; i++) {
            const char* nm = kModelName[kModelIdx[sm->first + i]];
            if (cs) {
                int held = 0;
                for (int j = 0; j < cs->n && !held; j++)
                    if (kCharasetIdx[cs->first + j] == kModelIdx[sm->first + i]) held = 1;
                if (held) continue;
            }
            vm_load_chara(nm);
            n++;
            ff13logv("[FF13-SRP] battle picker: \"%s\" is not in %s -- asked for it directly\n",
                     nm, cs ? cs->name : "any set this fight could use");
        }
    }
    if (n && !g_lchOk)
        ff13loga("[FF13-SRP] battle picker: ...except the per-character loader never matched, so "
                 "none of that happened\n");
    g_looseScene = n ? btsc : 0;      // read back once the wait is up, not now (async load)
    if (n) g_looseLoaded = 1;         // ...and something has to take them back out again
}

// Asked only once the wait is done: the load is asynchronous, and asking straight after the
// request answered "still not loaded" for models the fight then fielded perfectly well.
void loose_specs_report(void) {
    uint32_t btsc = g_looseScene;
    if (!btsc) return;
    g_looseScene = 0;
    for (int i = 0; i < FF13_LOOSE_N; i++) {
        if (kLooseSpecs[i].btsc != btsc) continue;
        int was = vm_chara_loaded(kLooseSpecs[i].spec);
        ff13loga("[FF13-SRP] battle picker: \"%s\" %s\n", kLooseSpecs[i].spec,
                 was < 0 ? "cannot be asked whether it loaded"
                         : was ? "loaded" : "did NOT load -- expect it to be missing");
    }
}

// Some chains mark their checkpoint INSIDE themselves, so a converted end's reload replays the
// chain and re-fights. Armed at fight-over (evGate), applied on the reload's own loadBg -- which
// is before the replay reaches its gate check.
volatile int   g_gateReplayVal   = 0;
volatile DWORD g_gateReplayUntil = 0;
volatile int   g_uconDue         = 0;      // ...and once the field is back, com_ucon_ev (picker)

// Resource.loadBg(String) -> the native load_privBgByMapSet, one string slot like the charaset
// load. The name to pass is the scene's own bt_scene.wdb cell[3].
const uint8_t P_LBG[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x2C};
static volatile int g_lbgLogN = 0;
static volatile unsigned char g_inBgLoad = 0;    // our own call, so it reads apart from the game's
char g_curBg[40];                         // the mapset the GAME last asked for
volatile int g_bgAsked = 0;               // ...and whether this fight had to ask for another

// Read-only log, so a picked fight and the story's own can be compared load for load.
void __cdecl on_load_bg(uint32_t vmctx) {
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, SB_SLOT)) return;
    const char* n = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(args + 4);
    int ok = n && !IsBadStringPtrA(n, 40);
    // Only the game's own requests say which mapset the field is on; counting ours would make
    // that test always agree. Kept up past the log cap: bg_heal_watch reads it all session.
    if (ok && !g_inBgLoad) {
        strncpy(g_curBg, n, sizeof g_curBg - 1);
        g_curBg[sizeof g_curBg - 1] = 0;
        // The converted end's reload announcing itself: shut the replay's gate now, well before
        // the chain reaches its own check (loads + syncWait sit between).
        if (g_gateReplayVal && (int)(GetTickCount() - g_gateReplayUntil) < 0) {
            int v = g_gateReplayVal;
            g_gateReplayVal = 0;
            vm_set_work(0, v);
            g_uconDue = 1;
            ff13loga("[FF13-SRP] battle picker: the reload is in (\"%.39s\") -- scenario flag -> %d "
                     "so the replay stops before its fight; control follows once the field is up\n",
                     n, v);
        }
    }
    if (g_lbgLogN < 60) {
        g_lbgLogN++;
        ff13logv("[FF13-SRP] loadBg(\"%.39s\")%s\n", ok ? n : "(unreadable)",
                 g_inBgLoad ? "  <- ours" : "");
    }
    setup_spend(g_inBgLoad);      // ...and here, for a chain that loads no character set
}

void vm_load_bg(const char* mapset) {
    if (!g_base || !mapset || !mapset[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)(uintptr_t)mapset;    // slot0 {tag, value}
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    g_inBgLoad = 1;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_LOADBG))(ctx);
    g_inBgLoad = 0;
}

static void vm_slot_str(uint32_t* slot, const char* s);

// For where the mod stops a flow between a script's fade-out and the fade-in it never reaches.
#define RVA_FADEWITHID 0x7E88E0      // VA 0xBE88E0 Window.startFadeWithId_l(id,int,float,int)
// The slots are NOT (id, layer, ms, 0) whatever the name says: 0xBE88E0 reads slot0 clamped to
// 0..2, slot1 as a FLOAT, slot2 as the fade call's first argument, slot3 as the id it dispatches
// on. This exact set is what was measured to fade back in -- do not reason about it from the name.
void vm_fade_in(void) {
    if (!g_base) return;
    uint8_t  ctx[0x200];
    uint32_t args[8];
    memset(ctx, 0, sizeof ctx);
    memset(args, 0, sizeof args);
    args[1] = 4;
    args[3] = 0;
    args[5] = 0x447A0000u;
    args[7] = 0;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_FADEWITHID))(ctx);
}

// Snapshots the game NOW as the live checkpoint under a named restart point. Re-marking at
// fight-over is how a converted end escapes a chain that marked its checkpoint inside itself.
#define RVA_DORESTARTSAVE 0x7E63D0   // VA 0xBE63D0 White.doRestartSave(String)
void vm_restart_save(const char* rp) {
    if (!g_base || !rp || !rp[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    vm_slot_str(args, rp);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_DORESTARTSAVE))(ctx);
}

// The game's own control-return: startCinema(0, "com_ucon_ev", "suc_fx2_on_flda") and nothing
// else -- no schedule or pack. What a completed restart runs last to give the field back.
void vm_field_ucon(void) {
    if (!g_base) return;
    uint8_t  ctx[0x200];
    uint32_t args[12];
    memset(ctx, 0, sizeof ctx);
    memset(args, 0, sizeof args);
    args[0] = 0; args[1] = 0;                        // slot0 = the second Vector, null
    args[2] = 0; args[3] = 0;                        // slot1 = the first Vector, null
    vm_slot_str(args + 4, "");                       // slot2 = the callback (none)
    vm_slot_str(args + 6, "suc_fx2_on_flda");        // slot3 = the success label
    vm_slot_str(args + 8, "com_ucon_ev");            // slot4 = the event
    args[10] = 0; args[11] = 0;                      // slot5 = mode 0, a script event
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_STARTCINEMA))(ctx);
}


// ---- lighting a fight the way its story flow lights it ------------------------------------------
// FF13 lights a scene through a "draw env": map light groups carry one, characters carry one, and a
// script can overwrite either. Both natives read their arguments in REVERSE declaration order with
// `this` last (0xBE0EF0 / 0xBC9450):
//     Scene.overwriteDrawEnvManually_l(String bg, String from, String to, int mode, float t)
//     Chara.setDrawEnvManually_l(String env, int mode, float t)     -- this = the character's name
// A string argument is a tagged slot: type 5, length in the top half of the tag, pointer as value.
static void vm_slot_str(uint32_t* slot, const char* s) {
    slot[0] = 5u | ((uint32_t)strlen(s) << 16);
    slot[1] = (uint32_t)(uintptr_t)s;
}
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

// Log every draw-env call the story flow makes, so ours can be subtracted from the story's.
static volatile int g_deLogN = 0;
static volatile unsigned char g_inDrawEnv = 0;
static volatile unsigned char g_inSweep = 0;   // the room sweep, which must not bury the log
// Reads WITHOUT disturbing the argument pointer: the native has consumed nothing at its prologue.
static const char* de_str(uint32_t args, int slot) {
    uintptr_t p = (uintptr_t)args + (uintptr_t)slot * 8 + 4;
    if (IsBadReadPtr((void*)p, 4)) return "(?)";
    const char* s = (const char*)(uintptr_t)*(uint32_t*)p;
    if (!s || IsBadStringPtrA(s, 40)) return "(none)";
    return s;
}

void __cdecl on_drawenv_bg(uint32_t vmctx) {
    if (g_inSweep || g_deLogN >= 80) return;
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t a = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!a || IsBadReadPtr((void*)(uintptr_t)a, 5 * 8)) return;
    g_deLogN++;
    float t; memcpy(&t, (const void*)(uintptr_t)(a + 4), sizeof t);
    ff13logv("[FF13-SRP] drawEnv bg \"%.39s\": \"%.39s\" -> \"%.39s\" mode %d time %.2f%s\n",
             de_str(a, 4), de_str(a, 3), de_str(a, 2), *(int*)(uintptr_t)(a + 8 + 4), (double)t,
             g_inDrawEnv ? "  <- ours" : "");
}

void __cdecl on_drawenv_chara(uint32_t vmctx) {
    if (g_deLogN >= 80) return;
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t a = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!a || IsBadReadPtr((void*)(uintptr_t)a, 4 * 8)) return;
    g_deLogN++;
    float t; memcpy(&t, (const void*)(uintptr_t)(a + 4), sizeof t);
    ff13logv("[FF13-SRP] drawEnv chara \"%.39s\": -> \"%.39s\" mode %d time %.2f%s\n",
             de_str(a, 3), de_str(a, 2), *(int*)(uintptr_t)(a + 8 + 4), (double)t,
             g_inDrawEnv ? "  <- ours" : "");
}

// The other half of a room's lighting: trigger volumes carrying a screen env ("scr_env"), applied
// to whoever stands inside them. Read-only log of the switching and the camera's own screen-env.
static volatile int g_envLogN = 0;
static volatile unsigned char g_inBox = 0;
static void box_log(uint32_t vmctx, const char* what) {
    if (g_envLogN >= 240) return;
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t a = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!a || IsBadReadPtr((void*)(uintptr_t)a, 8)) return;
    const char* s = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(a + 4);
    if (!s || IsBadStringPtrA(s, 40)) return;
    if (strncmp(s, "bx_", 3) == 0) return;      // the zone's trigger boxes, hundreds of them
    g_envLogN++;
    ff13logv("[FF13-SRP] %s \"%.39s\"%s\n", what, s, g_inBox ? "  <- ours" : "");
}
void __cdecl on_box_on (uint32_t c) { box_log(c, "object on   "); }
void __cdecl on_box_off(uint32_t c) { box_log(c, "object off  "); }
void __cdecl on_layout_on (uint32_t c) { box_log(c, "layout show "); }
void __cdecl on_layout_off(uint32_t c) { box_log(c, "layout hide "); }
void __cdecl on_screen_env(uint32_t vmctx) {
    if (g_envLogN >= 60) return;
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t a = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!a || IsBadReadPtr((void*)(uintptr_t)a, 8)) return;
    g_envLogN++;
    ff13logv("[FF13-SRP] auto screen env -> %u\n", *(uint32_t*)(uintptr_t)(a + 4));
}

static void vm_drawenv_bg(const char* bg, const char* from, const char* to, int mode, float t) {
    if (!g_base || !bg || !from || !to) return;
    uint8_t  ctx[0x200];
    uint32_t args[10];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = f2u(t);                // slot0 = t
    args[2] = 0; args[3] = (uint32_t)mode;        // slot1 = mode
    vm_slot_str(args + 4, to);                    // slot2 = the env to put on
    vm_slot_str(args + 6, from);                  // slot3 = the one being replaced
    vm_slot_str(args + 8, bg);                    // slot4 = the light group
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    g_inDrawEnv = 1;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_DRAWENV_BG))(ctx);
    g_inDrawEnv = 0;
}

// sfBoxEnable/sfBoxDisable = Scene.validateObject / Scene.invalidateObject, one string slot each.
// Standing in the box is what applies its env, and the party is warped into the middle of these.
static void vm_box(const char* name, int on) {
    if (!g_base || !name || !name[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    vm_slot_str(args, name);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    g_inBox = 1;
    ((vmNative1_t)(uintptr_t)(g_base + (on ? RVA_BOX_ON : RVA_BOX_OFF)))(ctx);
    g_inBox = 0;
}

static void vm_drawenv_chara(const char* who, const char* env, int mode, float t) {
    if (!g_base || !who || !who[0] || !env) return;
    uint8_t  ctx[0x200];
    uint32_t args[8];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = f2u(t);                // slot0 = t
    args[2] = 0; args[3] = (uint32_t)mode;        // slot1 = mode
    vm_slot_str(args + 4, env);                   // slot2 = the env
    vm_slot_str(args + 6, who);                   // slot3 = this, the character
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    g_inDrawEnv = 1;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_DRAWENV_CHARA))(ctx);
    g_inDrawEnv = 0;
}

// ---- the party a fight is meant to be fought with -------------------------------------------
// Some fights change the party in the cutscene that leads into them, and the whole member change is
// one White.execScript by name. Ask White.isScriptExist first: the picker can be pointed at a scene
// from any zone, and a script the zone's database does not hold is not something to run.
#define RVA_EXECSCRIPT   0x7E4940    // VA 0xBE4940  White.execScript
#define RVA_SCRIPTEXISTS 0x7E48A0    // VA 0xBE48A0  White.isScriptExist
// A native that answers does NOT answer in eax: 0xBE48A0 reads its argument, steps the argument
// pointer BACK one slot and writes {tag 1, result} there. Reading eax gets whatever the previous
// call left behind, which made this guard say yes to scripts that do not exist.
static int vm_script_exists(const char* name) {
    if (!g_base || !name || !name[0]) return 0;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    vm_slot_str(args, name);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_SCRIPTEXISTS))(ctx);
    return args[1] != 0;                      // the slot the argument was in, now the answer
}
static void vm_exec_script(const char* name) {
    if (!g_base || !name || !name[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    vm_slot_str(args, name);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_EXECSCRIPT))(ctx);
}

// THREADING: execScript cannot be called while no script is executing. 0xBB6440 -> 0xBB6480 asks
// 0xBB6540(-1) for the CURRENT VM THREAD first, and that slot ([pool+4], via 0x809920 -> 0xBB75C0
// -> 0xBB8960) is empty except while a script runs. Inside one of our VM-native hooks a script IS
// executing, so the name is armed when a door fires and spent from the chain's own next call
// (g_setupWant / setup_spend), never called directly from the game thread.
//
// The party itself is six natives and can be built by hand -- common.sfSetUpParty is:
//     Party.noticePartyChange(0); Party.breakPartyAll();
//     Party.joinToParty(names); Party.joinToBattleParty(names);
//     Party.setPartyLeader(names[0]); Party.noticePartyChange(1);
//     Party.waitPartyChange(); Party.setStandPos(names[i], i);
// The list argument needs no object: 0xBDBF00 reads one tagged slot, takes the byte length out of
// the TOP HALF of the tag word, divides by four and walks that many char* out of the value -- a
// plain C array of pointers with a count where a string slot's length would go.
// waitPartyChange is deliberately NOT called: it blocks the CALLING thread on work the VM drives,
// so from the game thread it never ends.
#define RVA_PARTY_NOTICE  0x7DA910   // VA 0xBDA910  Party.noticePartyChange(int)
#define RVA_PARTY_BREAK   0x7DACE0   // VA 0xBDACE0  Party.breakPartyAll()
#define RVA_PARTY_JOIN    0x7DBF00   // VA 0xBDBF00  Party.joinToParty(String[])
#define RVA_PARTY_JOINBTL 0x7DBFC0   // VA 0xBDBFC0  Party.joinToBattleParty(String[])
#define RVA_PARTY_LEADER  0x7DC050   // VA 0xBDC050  Party.setPartyLeader(String)
#define RVA_PARTY_STAND   0x7DC6F0   // VA 0xBDC6F0  Party.setStandPos(String, int)
// The scenario flag is Party.getWork(0) / Party.setWork(0, n). Story chains are gated on it
// (sfStartCutEventWithCheck runs only while the flag is below the value the chain ends by setting),
// so a chain will not run twice unless the flag is put back first.
#define RVA_PARTY_GETWORK 0x7DC3D0   // VA 0xBDC3D0  Party.getWork(int)
#define RVA_PARTY_SETWORK 0x7DC0C0   // VA 0xBDC0C0  Party.setWork(int, int)
int vm_get_work(int idx) {
    if (!g_base) return -1;
    uint8_t  ctx[0x200];
    uint32_t a[2];
    memset(ctx, 0, sizeof ctx);
    a[0] = 0; a[1] = (uint32_t)idx;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)a;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_PARTY_GETWORK))(ctx);
    return (int)a[1];                          // answered where the argument was
}
void vm_set_work(int idx, int val) {
    if (!g_base) return;
    uint8_t  ctx[0x200];
    uint32_t a[4];
    memset(ctx, 0, sizeof ctx);
    a[0] = 0; a[1] = (uint32_t)val;            // slot0 = the value  (reverse order, as always)
    a[2] = 0; a[3] = (uint32_t)idx;            // slot1 = which work
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)a;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_PARTY_SETWORK))(ctx);
}

// Party.setEventFlag(String, boolean). Work 0 is the story's coarse position; these named flags
// are the fine one. See the arena table's evEvFlag.
#define RVA_PARTY_SETEVFLAG 0x7DCA60   // VA 0xBDCA60  Party.setEventFlag(String, boolean)
void vm_set_event_flag(const char* name, int on) {
    if (!g_base || !name) return;
    uint8_t  ctx[0x200];
    uint32_t a[4];
    memset(ctx, 0, sizeof ctx);
    a[0] = 0; a[1] = (uint32_t)(on ? 1 : 0);            // slot0 = the value  (reverse order)
    a[2] = 0; a[3] = (uint32_t)(uintptr_t)name;         // slot1 = which flag
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)a;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_PARTY_SETEVFLAG))(ctx);
}

// Like isLoadedChara, the answer comes back in the slot the argument went out in.
#define RVA_PARTY_GETEVFLAG 0x7DC9D0   // VA 0xBDC9D0  Party.getEventFlag(String) -> boolean
int vm_get_event_flag(const char* name) {
    if (!g_base || !name) return -1;
    uint8_t  ctx[0x200];
    uint32_t a[2];
    memset(ctx, 0, sizeof ctx);
    a[0] = 0; a[1] = (uint32_t)(uintptr_t)name;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)a;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_PARTY_GETEVFLAG))(ctx);
    return (int)a[1];
}

// Party.callPartyNative(op, a, b, c, d) -- the catch-all the deck calls go through. op 4 is
// restoreOptimaDeckAll, which common's sfSetUpParty issues on every party change.
#define RVA_PARTY_CALLNATIVE 0x7DD7A0  // VA 0xBDD7A0  Party.callPartyNative(int,int,int,int,int)
#define PARTY_NATIVE_RESTORE_OPTIMA_ALL 4
static void vm_party_native(int op, int a1, int a2, int a3, int a4) {
    if (!g_base) return;
    uint8_t  ctx[0x200];
    uint32_t a[10];
    memset(ctx, 0, sizeof ctx);
    a[0] = 0; a[1] = (uint32_t)a4;             // slot0 = the LAST argument, as everywhere
    a[2] = 0; a[3] = (uint32_t)a3;
    a[4] = 0; a[5] = (uint32_t)a2;
    a[6] = 0; a[7] = (uint32_t)a1;
    a[8] = 0; a[9] = (uint32_t)op;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)a;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_PARTY_CALLNATIVE))(ctx);
}

// registOptimaDeckNoStore, deliberately: the plain and *Store variants REWRITE THE PLAYER'S SAVED
// decks. NoStore sets only what the party is holding right now. Do not change this.
#define PARTY_NATIVE_REGIST_DECK_NOSTORE 5
static void vm_regist_deck(int deck, int r0, int r1, int r2) {
    vm_party_native(PARTY_NATIVE_REGIST_DECK_NOSTORE, deck, r0, r1, r2);
}

static void vm_slot_list(uint32_t* slot, const char* const* names, int n) {
    slot[0] = ((uint32_t)(n * 4) << 16);      // only the top half is read: the length in bytes
    slot[1] = (uint32_t)(uintptr_t)names;
}
static void vm_call1(uint32_t rva, const uint32_t* slot) {
    uint8_t ctx[0x200];
    memset(ctx, 0, sizeof ctx);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)slot;
    ((vmNative1_t)(uintptr_t)(g_base + rva))(ctx);
}

// Which deck the fight starts held. Its own native, NOT reached through callPartyNative.
#define RVA_SETOPTIMADECK 0x7DCBC0   // VA 0xBDCBC0  Party.setOptimaDeck(int)
static void vm_set_optima_deck(int deck) {
    if (!g_base) return;
    uint32_t a[2];
    a[0] = 0; a[1] = (uint32_t)deck;
    vm_call1(RVA_SETOPTIMADECK, a);
}

// common's sfSetUpParty with three lines deliberately missing; all three were tried on hardware
// and must stay out (details in the notes):
//   waitPartyChange()      -- FREEZES the game when called from the game thread
//   restoreOptimaDeckAll() -- produced the same generated decks and destabilised arena staging
//   execScript(...)        -- needs a VM thread
// The paradigms are therefore written after the fight starts instead; see decks_apply/kSceneDecks.
// One difference on purpose: the game passes two lists (whole party, the three who fight) and this
// passes the same list for both.
static void vm_party_setup(const char* const* names, int n) {
    if (!g_base || !names || n <= 0) return;
    uint32_t a[4];
    a[0] = 0; a[1] = 0;                       vm_call1(RVA_PARTY_NOTICE, a);   // notice(0)
    vm_call1(RVA_PARTY_BREAK, a);
    vm_slot_list(a, names, n);                vm_call1(RVA_PARTY_JOIN, a);
    vm_slot_list(a, names, n);                vm_call1(RVA_PARTY_JOINBTL, a);
    vm_slot_str(a, names[0]);                 vm_call1(RVA_PARTY_LEADER, a);
    a[0] = 0; a[1] = 1;                       vm_call1(RVA_PARTY_NOTICE, a);   // notice(1)
    for (int i = 0; i < n; i++) {
        a[0] = 0; a[1] = (uint32_t)i;         // slot0 = the position, slot1 = who (reverse order)
        vm_slot_str(a + 2, names[i]);
        vm_call1(RVA_PARTY_STAND, a);
    }
}

// The party manager's live record; see common/docs/PARTY_LEADER.md for the walk to it.
#define RVA_PARTY_ROOT   0x242B230   // VA 0x282B230
#define OFF_PARTY_LIVE   0xBC0       // which of the four records is live
#define OFF_PARTY_RECS   0x3C
#define PARTY_REC_STRIDE 0x1FC
#define OFF_REC_PARTY    0x2C        // six charIDs: who is with you at all
#define OFF_REC_BATTLE   0x44        // three of them: who fights. -1 for an empty place
#define OFF_REC_LEADER   0x50
static void party_log(const char* when) {
    static const char* const kWho[6] =
        { "lightning", "sazz", "snow", "hope", "vanira", "fang" };
    if (!g_base || IsBadReadPtr((void*)(g_base + RVA_PARTY_ROOT), 4)) return;
    uint32_t pm = *(uint32_t*)(g_base + RVA_PARTY_ROOT);
    if (!pm || IsBadReadPtr((void*)(uintptr_t)(pm + OFF_PARTY_LIVE), 4)) return;
    uint32_t idx = *(uint32_t*)(uintptr_t)(pm + OFF_PARTY_LIVE);
    if (idx > 3) return;
    uintptr_t rec = pm + OFF_PARTY_RECS + (uintptr_t)idx * PARTY_REC_STRIDE;
    if (IsBadReadPtr((void*)(rec + OFF_REC_LEADER), 4)) return;
    const int* pt = (const int*)(rec + OFF_REC_PARTY);
    const int* bt = (const int*)(rec + OFF_REC_BATTLE);
    int ld = *(const int*)(rec + OFF_REC_LEADER);
    #define WHO(v) (((v) >= 0 && (v) < 6) ? kWho[(v)] : "-")
    // Both arrays and the record index: six dashes alone cannot distinguish an empty party from
    // the wrong record.
    ff13logv("[FF13-SRP] party (%s): battle %s/%s/%s  with %s/%s/%s/%s/%s/%s  leader %s  [rec %u]\n",
             when, WHO(bt[0]), WHO(bt[1]), WHO(bt[2]),
             WHO(pt[0]), WHO(pt[1]), WHO(pt[2]), WHO(pt[3]), WHO(pt[4]), WHO(pt[5]),
             WHO(ld), (unsigned)idx);
    #undef WHO
}

// The paradigm decks live in the same live party record: entry n = (record+0x78) + 4 + n*0x30,
// eight of them; within an entry the three roles are at +4, +8, +0xC, one byte each, 0xFF = empty.
// Role numbering is fake/Party.java's ROLE_* constants: 0 SEN 1 COM 2 RAV 3 SYN 4 SAB 5 MED.
#define OFF_REC_DECKS   0x7C     // record + 0x78, +4 for the container's own header
#define DECK_STRIDE     0x30
#define DECK_COUNT      8
#define R_SEN 0
#define R_COM 1
#define R_RAV 2
#define R_SYN 3
#define R_SAB 4
#define R_MED 5
static const char* role_name(int r) {
    static const char* const kR[6] = { "SEN", "COM", "RAV", "SYN", "SAB", "MED" };
    return (r >= 0 && r < 6) ? kR[r] : "--";
}
// Every record, not just the live one: the live index can point elsewhere while a change is in
// flight, and six dashes cannot be told from an empty party.
void optima_log(const char* when) {
    static const char* const kWho[6] =
        { "lightning", "sazz", "snow", "hope", "vanira", "fang" };
    if (!g_base || IsBadReadPtr((void*)(g_base + RVA_PARTY_ROOT), 4)) return;
    uint32_t pm = *(uint32_t*)(g_base + RVA_PARTY_ROOT);
    if (!pm || IsBadReadPtr((void*)(uintptr_t)(pm + OFF_PARTY_LIVE), 4)) return;
    uint32_t live = *(uint32_t*)(uintptr_t)(pm + OFF_PARTY_LIVE);
    ff13logv("[FF13-SRP] decks (%s): live record %u\n", when, (unsigned)live);
    for (uint32_t r = 0; r < 4; r++) {
        uintptr_t rec = pm + OFF_PARTY_RECS + (uintptr_t)r * PARTY_REC_STRIDE;
        if (IsBadReadPtr((void*)rec, PARTY_REC_STRIDE)) continue;
        const int* bt = (const int*)(rec + OFF_REC_BATTLE);
        #define WHO(v) (((v) >= 0 && (v) < 6) ? kWho[(v)] : "-")
        char decks[DECK_COUNT * 20 + 1];
        int at = 0;
        for (int d = 0; d < DECK_COUNT; d++) {
            const uint8_t* e = (const uint8_t*)(rec + OFF_REC_DECKS + (uintptr_t)d * DECK_STRIDE);
            int r0 = e[4], r1 = e[8], r2 = e[12];
            if (r0 == 0xFF && r1 == 0xFF && r2 == 0xFF) continue;   // an unused slot says nothing
            at += snprintf(decks + at, sizeof(decks) - at, "%s%d:%s/%s/%s", at ? " " : "", d,
                           role_name(r0 == 0xFF ? -1 : r0), role_name(r1 == 0xFF ? -1 : r1),
                           role_name(r2 == 0xFF ? -1 : r2));
            if (at >= (int)sizeof(decks) - 20) break;
        }
        ff13logv("[FF13-SRP]   rec %u%s  battle %s/%s/%s  %s\n", (unsigned)r,
                 r == live ? " (live)" : "      ",
                 WHO(bt[0]), WHO(bt[1]), WHO(bt[2]), at ? decks : "(no decks)");
        #undef WHO
    }
}

// TIMING: must run once the battle has BEGUN. Every one of these fights rebuilds its decks on the
// way in, more than once, and the last write wins; this is the first moment nothing else writes.
// setOptimaDeck(0) after the write is required too -- a fight with no cutscene never makes that
// call itself and comes up with the right decks and the wrong one in hand.
void decks_apply(const signed char (*decks)[3], int n, int sel) {
    if (!g_base || !decks || n <= 0) return;
    for (int d = 0; d < n && d < DECK_COUNT; d++)
        vm_regist_deck(d, decks[d][0], decks[d][1], decks[d][2]);
    for (int d = n; d < DECK_COUNT; d++)
        vm_regist_deck(d, -1, -1, -1);          // ...and nothing left over from the generated set
    vm_set_optima_deck((sel > 0 && sel < n) ? sel : 0);
}

// The live record's three battle members, for asking whether a chain changed who fights.
int party_battle_ids(int out[3]) {
    if (!g_base || IsBadReadPtr((void*)(g_base + RVA_PARTY_ROOT), 4)) return 0;
    uint32_t pm = *(uint32_t*)(g_base + RVA_PARTY_ROOT);
    if (!pm || IsBadReadPtr((void*)(uintptr_t)(pm + OFF_PARTY_LIVE), 4)) return 0;
    uint32_t idx = *(uint32_t*)(uintptr_t)(pm + OFF_PARTY_LIVE);
    if (idx > 3) return 0;
    uintptr_t rec = pm + OFF_PARTY_RECS + (uintptr_t)idx * PARTY_REC_STRIDE;
    if (IsBadReadPtr((void*)(rec + OFF_REC_BATTLE), 12)) return 0;
    const int* bt = (const int*)(rec + OFF_REC_BATTLE);
    out[0] = bt[0]; out[1] = bt[1]; out[2] = bt[2];
    return bt[0] >= 0;
}

// Which deck is in hand: the low byte of [record+0x5C] -- 0xB63940 (setOptimaDeck's writer)
// stores it there. -1 = could not read.
#define OFF_REC_SEL 0x5C
int deck_sel_live(void) {
    if (!g_base || IsBadReadPtr((void*)(g_base + RVA_PARTY_ROOT), 4)) return -1;
    uint32_t pm = *(uint32_t*)(g_base + RVA_PARTY_ROOT);
    if (!pm || IsBadReadPtr((void*)(uintptr_t)(pm + OFF_PARTY_LIVE), 4)) return -1;
    uint32_t idx = *(uint32_t*)(uintptr_t)(pm + OFF_PARTY_LIVE);
    if (idx > 3) return -1;
    uintptr_t rec = pm + OFF_PARTY_RECS + (uintptr_t)idx * PARTY_REC_STRIDE;
    if (IsBadReadPtr((void*)(rec + OFF_REC_SEL), 4)) return -1;
    return (int)(*(uint32_t*)(rec + OFF_REC_SEL) & 0xFF);
}

// The live record's decks -- the leading run of non-empty entries, in decks_apply's shape.
int decks_read_live(signed char out[][3], int maxn) {
    if (!g_base || IsBadReadPtr((void*)(g_base + RVA_PARTY_ROOT), 4)) return 0;
    uint32_t pm = *(uint32_t*)(g_base + RVA_PARTY_ROOT);
    if (!pm || IsBadReadPtr((void*)(uintptr_t)(pm + OFF_PARTY_LIVE), 4)) return 0;
    uint32_t idx = *(uint32_t*)(uintptr_t)(pm + OFF_PARTY_LIVE);
    if (idx > 3) return 0;
    uintptr_t rec = pm + OFF_PARTY_RECS + (uintptr_t)idx * PARTY_REC_STRIDE;
    if (IsBadReadPtr((void*)rec, PARTY_REC_STRIDE)) return 0;
    int n = 0;
    for (int d = 0; d < maxn && d < DECK_COUNT; d++) {
        const uint8_t* e = (const uint8_t*)(rec + OFF_REC_DECKS + (uintptr_t)d * DECK_STRIDE);
        if (e[4] == 0xFF && e[8] == 0xFF && e[12] == 0xFF) break;
        out[d][0] = (signed char)(e[4]  == 0xFF ? -1 : e[4]);
        out[d][1] = (signed char)(e[8]  == 0xFF ? -1 : e[8]);
        out[d][2] = (signed char)(e[12] == 0xFF ? -1 : e[12]);
        n = d + 1;
    }
    return n;
}

// All four records at once -- who fights and what their decks are -- as one number, so a change
// anywhere trips it. Keying on the live record alone misses changes in flight. 0 = nothing read.
uint32_t party_state_key(void) {
    if (!g_base || IsBadReadPtr((void*)(g_base + RVA_PARTY_ROOT), 4)) return 0;
    uint32_t pm = *(uint32_t*)(g_base + RVA_PARTY_ROOT);
    if (!pm) return 0;
    uint32_t h = 2166136261u;
    for (uint32_t r = 0; r < 4; r++) {
        uintptr_t rec = pm + OFF_PARTY_RECS + (uintptr_t)r * PARTY_REC_STRIDE;
        if (IsBadReadPtr((void*)rec, PARTY_REC_STRIDE)) return 0;
        const uint8_t* bt = (const uint8_t*)(rec + OFF_REC_BATTLE);
        for (int i = 0; i < 12; i++) { h ^= bt[i]; h *= 16777619u; }
        for (int d = 0; d < DECK_COUNT; d++) {
            const uint8_t* e = (const uint8_t*)(rec + OFF_REC_DECKS + (uintptr_t)d * DECK_STRIDE);
            h ^= e[4];  h *= 16777619u;
            h ^= e[8];  h *= 16777619u;
            h ^= e[12]; h *= 16777619u;
        }
    }
    return h ? h : 1;
}

// The party a fight is meant to be fought with, for fights whose party NOTHING ELSE sets. RULE: a
// fight whose own chain sets its party up must NOT be listed here -- setting the party at pick time
// rebuilds the deck list ahead of the chain and the chain's rebuild is then not the clean one the
// story gets. Order is the leader's: sfSetUpParty makes the first of the battle party the leader.
// This is a separate table from kSceneArena on purpose: an entry in kSceneArena takes a fight off
// the field-encounter path whatever its other fields say.
static const char* const kPartySnowAlone[1] = { "snow" };
// Fights whose END has to be taken to the checkpoint although the mod does not stage them: an
// event fight ends by handing the run to the story, and a picked one has no story to hand it to.
// "End", not "win": btsc03036 runs the same scripted ending either way, so the outcome is not the
// thing to key on. The arena table says this with restartAfterWin.
static const uint16_t kSceneWinAtCheckpoint[] = { 3036 };
int scene_win_at_checkpoint(uint32_t btsc) {
    for (int i = 0; i < (int)(sizeof kSceneWinAtCheckpoint / sizeof kSceneWinAtCheckpoint[0]); i++)
        if (kSceneWinAtCheckpoint[i] == btsc) return 1;
    return 0;
}

static const struct { uint16_t btsc; const char* const* who; int n; } kScenePartyScript[] = {
    { 3036, kPartySnowAlone, 1 },   // the PSICOM four, where the story's Snow-alone starts
    { 3037, kPartySnowAlone, 1 },   // Shiva -- same party, set a fight upstream of the door
    // btsc07010 has a door and the door is not enough: its chain's script sets the decks but the
    // BATTLE party does not follow, so the party change belongs to a step the door enters past.
    { 7010, kPartySnowAlone, 1 },
};

// The paradigms a fight is meant to hold, written once it has started. Kept out of the arena table
// on purpose: a fight does not need an arena entry to need its decks fixed.
// Roles are per battle slot, in the same order as the party above. All rows are MEASURED off the
// story fight; see the notes file rather than re-deriving them.
static const struct { uint16_t btsc; int n; signed char decks[8][3]; } kSceneDecks[] = {
    { 11100, 6, { { R_RAV, R_MED, R_SEN },
                  { R_SYN, R_MED, R_SEN },
                  { R_MED, R_COM, R_SEN },
                  { R_MED, R_MED, R_SEN },
                  { R_RAV, R_COM, R_SEN },
                  { R_RAV, R_RAV, R_COM } } },
    // Bahamut. Slots are fang / lightning / vanira, in that order.
    { 10060, 6, { { R_COM, R_RAV, R_RAV },
                  { R_COM, R_RAV, R_MED },
                  { R_SEN, R_COM, R_RAV },
                  { R_SEN, R_MED, R_MED },
                  { R_SAB, R_COM, R_RAV },
                  { R_SAB, R_COM, R_MED } } },
    // btsc11412 needs no entry: its door produces the right paradigms already.
    // btsc03036, Snow alone: three decks, one slot each, -1 for the two nobody is standing in.
    { 3036, 3, { { R_COM, -1, -1 },
                 { R_SEN, -1, -1 },
                 { R_RAV, -1, -1 } } },
};
const signed char (*scene_decks(uint32_t btsc, int* n))[3] {
    for (int i = 0; i < (int)(sizeof kSceneDecks / sizeof kSceneDecks[0]); i++)
        if (kSceneDecks[i].btsc == btsc) { *n = kSceneDecks[i].n; return kSceneDecks[i].decks; }
    *n = 0;
    return NULL;
}

// ---- what a cutscene sets the paradigms to (READ-ONLY logging) ------------------------------
// Slot counts read off each native (how many times it advances [ctx+0x114] by 8), and whether it
// hands a slot to the string constructor at 0x4112B0:
//     registOptimaDeck(int,int,int,int)   setOptimaDeck(int)      clearOptimaDeck(int)
//     clearAllOptimaDeck()                addRole(String,String)
const uint8_t P_OPTREG[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x20};
const uint8_t P_OPTSET[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x08};
const uint8_t P_OPTCLR[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x08};
const uint8_t P_OPTALL[3] = {0x55,0x8B,0xEC};
const uint8_t P_ADDROLE[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x30};
static volatile int g_optLogN = 0;

// The arguments are still on the VM stack at entry: four tagged 8-byte slots, value at +4 of each.
void __cdecl on_optima_regist(uint32_t vmctx) {
    if (!vmctx || g_optLogN >= 200) return;
    if (IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, 4 * SB_SLOT)) return;
    g_optLogN++;
    ff13logv("[FF13-SRP] registOptimaDeck(%d, %d, %d, %d)%s\n",
             *(int*)(uintptr_t)(args + 0 * SB_SLOT + 4), *(int*)(uintptr_t)(args + 1 * SB_SLOT + 4),
             *(int*)(uintptr_t)(args + 2 * SB_SLOT + 4), *(int*)(uintptr_t)(args + 3 * SB_SLOT + 4),
             g_bsStarted ? "  <- during a picked fight" : "");
}

void __cdecl on_optima_clear(uint32_t vmctx) {
    if (!vmctx || g_optLogN >= 200) return;
    if (IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, SB_SLOT)) return;
    g_optLogN++;
    ff13logv("[FF13-SRP] clearOptimaDeck(%d)%s\n", *(int*)(uintptr_t)(args + 4),
             g_bsStarted ? "  <- during a picked fight" : "");
}

void __cdecl on_optima_clrall(uint32_t vmctx) {
    (void)vmctx;
    if (g_optLogN >= 200) return;
    g_optLogN++;
    ff13logv("[FF13-SRP] clearAllOptimaDeck()%s\n",
             g_bsStarted ? "  <- during a picked fight" : "");
}

void __cdecl on_add_role(uint32_t vmctx) {
    if (!vmctx || g_optLogN >= 200) return;
    if (IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, 2 * SB_SLOT)) return;
    const char* who = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(args + 4);
    const char* role = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(args + SB_SLOT + 4);
    g_optLogN++;
    ff13logv("[FF13-SRP] addRole(\"%.15s\", \"%.15s\")%s\n",
             (who && !IsBadStringPtrA(who, 16)) ? who : "?",
             (role && !IsBadStringPtrA(role, 16)) ? role : "?",
             g_bsStarted ? "  <- during a picked fight" : "");
}

void __cdecl on_optima_set(uint32_t vmctx) {
    if (!vmctx || g_optLogN >= 200) return;
    if (IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, SB_SLOT)) return;
    g_optLogN++;
    ff13logv("[FF13-SRP] setOptimaDeck(%d)%s\n", *(int*)(uintptr_t)(args + 4),
             g_bsStarted ? "  <- during a picked fight" : "");
}

const char* const* g_partyWho = NULL;   // the party this fight wants, until another is picked
int g_partyWhoN = 0;
int g_partyDoor = 0;                    // ...and whether that fight is one the mod stages

const char* const* scene_party_who(uint32_t btsc, int* n) {
    for (int i = 0; i < (int)(sizeof kScenePartyScript / sizeof kScenePartyScript[0]); i++)
        if (kScenePartyScript[i].btsc == btsc) { *n = kScenePartyScript[i].n;
                                                return kScenePartyScript[i].who; }
    *n = 0;
    return NULL;
}

// Setting the party is not the same as having it: a member whose model is not resident brings the
// game down. So ask for the charaspec (p_<name>) one at a time through Resource.loadChara, and wait
// for it in the picker's own preload window -- nothing answers "is it there yet".
static void party_script_load(const char* const* who, int n) {
    for (int i = 0; i < n; i++) {
        char spec[24];
        snprintf(spec, sizeof spec, "p_%s", who[i]);
        vm_load_chara(spec);
        ff13logv("[FF13-SRP] battle picker: asked for \"%s\" -- this fight's party is not the one "
                 "loaded\n", spec);
    }
}

void party_script_run(const char* when) {
    if (!g_partyWho || g_partyWhoN <= 0) return;
    party_log("before");
    party_script_load(g_partyWho, g_partyWhoN);
    vm_party_setup(g_partyWho, g_partyWhoN);
    ff13logv("[FF13-SRP] battle picker: party set for this fight (%s)\n", when);
    party_log("after");
}

// ---- starting a fight the way the story starts it: by running its event ------------------------
// Rather than imitate the staging, replay the event: load its schedule and data pack, start it, and
// let its own callback call the fight.
//   Resource.loadCutSceneScheduleData(String)                          0xBDF290
//   Resource.loadCutScenePackData(String, String)                      0xBDF0D0
//   White.startCinema_l(int, String, String, String, Vector, Vector)   0xBE33D0
// All three read their arguments in the usual reverse order; startCinema takes six slots, the last
// two being the null Vectors, and the int mode is the last one read.
#define RVA_LOADEVSCHED 0x7DF290     // VA 0xBDF290
#define RVA_LOADEVPACK  0x7DF0D0     // VA 0xBDF0D0
#define RVA_UNLOADEVSCHED 0x7DF350   // VA 0xBDF350  Resource.unloadCutSceneScheduleData(String)
#define RVA_UNLOADEVPACK  0x7DF1D0   // VA 0xBDF1D0  Resource.unloadCutScenePackData(String, String)
// The success label is per fight and comes from the entry; this is only the common default.
#define EVENT_SUC_DEFAULT "suc_ev_on_fldd"
const struct ff13_arena_s* g_evPending = NULL;
volatile DWORD g_evAt = 0;
// Give-up cap for a door that silently did not start: everything the picker holds is released by
// the fight starting, so without this the player is left in a field they cannot move in.
// A PLAIN CAP, deliberately -- do not make this conditional on field state. Testing FIELD_IDLE
// failed in both directions: a really-failed door left the field in some other state so the test
// never fired, and on btsc03037 it fired mid-staging and threw away a fight about to start.
volatile DWORD g_evGiveUpCap = 0;
// How long a side-effect-only chain is given before the picker starts the fight itself.
volatile DWORD g_evNoFightAt = 0;    // when to come back through the picker, 0 = not
volatile int   g_evSkipDoor = 0;     // ...with the door switched off for that one pass
volatile uint32_t g_evTouch = 0;     // the touched object the first pass was given
// Field.createChara_l(name, model, 0x4000). Three slots, the int first as always.
#define RVA_CREATECHARA 0x7CC290     // VA 0xBCC290  Field.createChara_l
#define FIELDCHARA_FLAGS 0x4000      // ...ignoring whether the field is active

// Scene.activateAreaIgnoreFieldActive(group), one string slot.
#define RVA_ACTIVATEAREA 0x7E05B0    // VA 0xBE05B0  Scene.activateAreaIgnoreFieldActive
void vm_activate_area(const char* group) {
    if (!g_base || !group || !group[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    vm_slot_str(args, group);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_ACTIVATEAREA))(ctx);
}

// Party.setPositionByPositionMarkerGroup(n), one slot: how a zone's "read" function stands the
// party where an event expects them.
#define RVA_PARTYMARKER 0x7DC970     // VA 0xBDC970  Party.setPositionByPositionMarkerGroup
void vm_party_marker(int group) {
    if (!g_base || group < 0) return;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)group;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_PARTYMARKER))(ctx);
}

void vm_create_field_chara(const char* name, const char* model) {
    if (!g_base || !name || !name[0] || !model || !model[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[6];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = FIELDCHARA_FLAGS;      // slot0 = the flags
    vm_slot_str(args + 2, model);                 // slot1 = the model
    vm_slot_str(args + 4, name);                  // slot2 = the name
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_CREATECHARA))(ctx);
}

// The story's own tidy-up after a cutscene: dispose the schedule (twice, as the script does) and
// unload the pack. A picked evNoFight chain never reaches it, and the residue rides the retry
// snapshot into the post-fight field.
void vm_dispose_event(const char* name, const char* pack, const char* pack2) {
    if (!g_base || !name || !name[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[4];
    const char* packs[3] = { pack, pack2, NULL };
    for (int i = 0; i < 2; i++) {
        const char* p = packs[i];
        if (!p || !p[0]) continue;
        memset(ctx, 0, sizeof ctx);
        vm_slot_str(args + 0, p);                    // slot0 = the second argument
        vm_slot_str(args + 2, name);                 // slot1 = the first
        *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
        ((vmNative1_t)(uintptr_t)(g_base + RVA_UNLOADEVPACK))(ctx);
    }
    for (int i = 0; i < 2; i++) {                    // twice, as sfCutSceneCallBackBasic does
        memset(ctx, 0, sizeof ctx);
        vm_slot_str(args, name);
        *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
        ((vmNative1_t)(uintptr_t)(g_base + RVA_UNLOADEVSCHED))(ctx);
    }
    ff13logv("[FF13-SRP] battle picker: cutscene \"%.31s\" disposed (schedule + pack), as the "
             "story's own callback would\n", name);
}

// loadName/loadPack: the schedule and data pack to bring in when they are not the event being
// started (a script-event root often needs a cutscene further down the chain loaded first).
void vm_start_event(const char* name, const char* cb, const char* pack, const char* suc,
                           const char* pack2, int script,
                           const char* loadName, const char* loadPack) {
    if (!g_base || !name || !name[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[12];

    const char* ln = (loadName && loadName[0]) ? loadName : name;
    memset(ctx, 0, sizeof ctx);
    vm_slot_str(args, ln);
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_LOADEVSCHED))(ctx);

    // Both packs belong to whichever event was loaded above. pack2 must NOT be dropped when a
    // load name is given: btsc03037 needs two packs (a00 and b00) for one loaded cutscene.
    const char* packs[3] = { (loadName && loadName[0]) ? loadPack : pack, pack2, NULL };
    for (int i = 0; i < 2; i++) {
        const char* p = packs[i];
        if (!p || !p[0]) continue;
        memset(ctx, 0, sizeof ctx);
        vm_slot_str(args + 0, p);                    // slot0 = the second argument
        vm_slot_str(args + 2, ln);                   // slot1 = the first
        *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
        ((vmNative1_t)(uintptr_t)(g_base + RVA_LOADEVPACK))(ctx);
    }

    memset(ctx, 0, sizeof ctx);
    memset(args, 0, sizeof args);
    args[0] = 0; args[1] = 0;                        // slot0 = the second Vector, null
    args[2] = 0; args[3] = 0;                        // slot1 = the first Vector, null
    vm_slot_str(args + 4, cb && cb[0] ? cb : "");    // slot2 = the callback
    vm_slot_str(args + 6, suc && suc[0] ? suc : EVENT_SUC_DEFAULT);   // slot3 = the success label
    vm_slot_str(args + 8, name);                     // slot4 = the event
    // Mode 1 is a cutscene, mode 0 a script event; startCinema is the same native for both.
    args[10] = 0; args[11] = script ? 0u : 1u;       // slot5 = the mode
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_STARTCINEMA))(ctx);
}

// When a CUTSCENE actually begins, whoever asked for it: the picker's cover must stay up until
// something else is on screen, and dropping it when the door opened let a controllable lit field
// show through (opening the pause menu there could crash the game).
// The same native starts cutscenes and script events, so the mode is the test: slot5 == 1.
volatile int g_evCutSeen = 0;      // ...noticed here, acted on by bs_pump
void __cdecl on_start_cinema(uint32_t vmctx) {
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, 6 * SB_SLOT)) return;
    uint32_t mode = *(uint32_t*)(uintptr_t)(args + 5 * SB_SLOT + 4);   // slot5 = the mode
    if (mode == 1) g_evCutSeen = 1;
}

// Resource.syncWait, called right after loadBg exactly as the zone script does: loadBg only QUEUES
// the map request, and without this the terrain streams in under the party's feet.
// BLOCKING, on the game thread, like the script's own call; the cover hides the stall. Reads
// nothing from the context, so a forged one is enough.
// DO NOT try to shorten the picker's preload wait with resource-system calls. Three attempts all
// cost more than the wait: syncWait after our own charaset load froze, Party.waitPartyChange froze
// the same way, and polling 0x4486D0 once a frame worked as a signal but killed all audio for the
// rest of the session (it takes the sound manager's lock sixty times a second).
#define RVA_SYNCWAIT 0x7DE670        // VA 0xBDE670 Resource.syncWait()
void vm_sync_wait(void) {
    if (!g_base) return;
    uint8_t ctx[0x200];
    memset(ctx, 0, sizeof ctx);
    ((vmNative1_t)(uintptr_t)(g_base + RVA_SYNCWAIT))(ctx);
}

// Hold the party still the way an event does (the zone scripts' Chara(leader).switchControl(false)).
// NOT through Party.getPartyLeader: that native allocates its result out of the CONTEXT'S OWN arena
// (0xBEF050 walks heap descriptors at ctx+0xF0), which a forged context does not have. Instead
// 0xB25BE0 writes the leader's 16-byte name key into a caller buffer with no VM involved, and
// slot1 is forged to 0xBF6900's string-variant encoding {tag16=5, len16, char*}.
#define RVA_LEADERNAME 0x725BE0      // VA 0xB25BE0 leader name key -> 16-byte buffer (cdecl)
#define RVA_SWITCHCTL  0x7C61B0     // VA 0xBC61B0 Chara.switchControl(boolean)

// The 16-byte roster name key of the current leader. 0xB25BE0 is Party.getPartyLeader()'s own data
// path: the live record's leader charID at record+0x50, resolved to the roster record's key at +0x30.
int leader_name(char out[20]) {
    memset(out, 0, 20);
    if (!g_base) return 0;
    ((void (__cdecl*)(char*))(uintptr_t)(g_base + RVA_LEADERNAME))(out);
    out[16] = 0;
    return out[0] != 0;
}

// The game decides this the same way (fld/com.java's sfIsChocoboRide compares the leader against
// "cho_vn"/"cho_lt"/"cho_sz"/"cho_ho"/"cho_sn"/"cho_fa"), so a leader key beginning "cho_" is the
// mount and nothing else in the roster is. Picking a fight while mounted crashed on hardware.
int riding_chocobo(void) {
    char name[20];
    if (!leader_name(name)) return 0;      // unknown leader: not a reason to lock the picker out
    return memcmp(name, "cho_", 4) == 0;
}

static void vm_switch_control_name(const char* name, int on, int say) {
    if (!g_base || !name || !name[0]) return;
    {
        uint8_t  ctx[0x200];
        uint32_t args[4];
        uint32_t len = (uint32_t)strlen(name);
        memset(ctx, 0, sizeof ctx);
        args[0] = 0; args[1] = on ? 1u : 0u;             // slot0 {tag, on} -- value read raw
        args[2] = 5u | (len << 16);                      // slot1 = string variant, 0xBF6900's shape
        args[3] = (uint32_t)(uintptr_t)name;
        *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
        ((vmNative1_t)(uintptr_t)(g_base + RVA_SWITCHCTL))(ctx);
    }
    if (say)
        ff13logv("[FF13-SRP] battle picker: field control %s for \"%s\" (the event lock)\n",
                 on ? "back on" : "off", name);
}
// switchControl does NOT stop the pause menu; that is a separate gate,
// White.setFieldTopMenuOpenFlag(7, false) at 0xBE5CA0 (slot0 boolean, slot1 flag number).
// Opening the menu while the chain is rebuilding the party crashed on hardware.
#define RVA_SETTOPMENU 0x7E5CA0
#define RVA_GETTOPMENU 0x7E5C20   // VA 0xBE5C20  White.getFieldTopMenuOpenFlag(int) -> bool
#define TOPMENU_OPEN_FLAG 7

// Read before writing, so the flag can be HELD without a write every frame: writing it every frame
// crashed the game with the Start button held down (the game's own menu code drives the same flag).
static int vm_top_menu_get(void) {
    if (!g_base) return -1;
    uint8_t  ctx[0x200];
    uint32_t args[2];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = TOPMENU_OPEN_FLAG;
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_GETTOPMENU))(ctx);
    return (int)(args[1] & 1);
}
static void vm_top_menu(int allow) {
    if (!g_base) return;
    uint8_t  ctx[0x200];
    uint32_t args[4];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = allow ? 1u : 0u;          // slot0 = the boolean
    args[2] = 0; args[3] = TOPMENU_OPEN_FLAG;        // slot1 = which flag
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_SETTOPMENU))(ctx);
}

// switchControl alone is NOT enough: measured, the lock is on the right character and re-taken
// every frame and the party still walks six units in five seconds. Two more levers are needed --
// the position hold (hold_still_begin, the warp override aimed at where the party already is) and
//   Field.enableFieldUserInterface(int mask, boolean on)   0xBD1A60
// which ORs/clears the mask in [0x27FF068]+0x1C. The game uses masks 1, 2 and 4; all three on is
// the ordinary field.
// The bits are SAVED and put back, not turned all on afterwards: chapter 1 has them genuinely off
// for a while, and handing them back early would show menus a run has not earned.
#define RVA_FIELDUI     0x7D1A60
#define OFF_FIELDUI_OBJ 0x23FF068         // VA 0x27FF068; +0x1C is the bit set itself
#define FIELDUI_ALL     7
static uint32_t g_uiSaved = 0xFFFFFFFFu;  // 0xFFFFFFFF = nothing saved

static void vm_field_ui(int mask, int on) {
    if (!g_base) return;
    uint8_t  ctx[0x200];
    uint32_t args[4];
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = on ? 1u : 0u;            // slot0 = the boolean
    args[2] = 0; args[3] = (uint32_t)mask;          // slot1 = the mask
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_FIELDUI))(ctx);
}
static uint32_t* field_ui_bits(void) {
    if (!g_base || IsBadReadPtr((void*)(g_base + OFF_FIELDUI_OBJ), 4)) return NULL;
    uint32_t o = *(uint32_t*)(g_base + OFF_FIELDUI_OBJ);
    if (!o || IsBadWritePtr((void*)(uintptr_t)(o + 0x1C), 4)) return NULL;
    return (uint32_t*)(uintptr_t)(o + 0x1C);
}
static void field_ui_lock(void) {
    uint32_t* b = field_ui_bits();
    g_uiSaved = b ? *b : 0xFFFFFFFFu;
    vm_field_ui(FIELDUI_ALL, 0);
    ff13logv("[FF13-SRP] battle picker: field UI bits %08X -> %08X (the event lock)\n",
             (unsigned)g_uiSaved, b ? (unsigned)*b : 0u);
}
static void field_ui_unlock(void) {
    uint32_t* b = field_ui_bits();
    if (g_uiSaved != 0xFFFFFFFFu && b) *b = g_uiSaved;
    else vm_field_ui(FIELDUI_ALL, 1);
    g_uiSaved = 0xFFFFFFFFu;
}

// switchControl is per CHARACTER, which is why this is a list and not a flag. Two ways a
// single-shot lock came undone on hardware: the leader changed under it (the lock went on before
// the party script), and the call can silently do nothing (leader_name reads a live record that is
// empty while the party is rebuilt). So: lock whoever is leading, remember the name, keep checking
// while the cover is up, and unlock EVERY name that was locked -- a character left with control
// off who later rejoins the party is a party member who cannot move.
#define CTL_NAMES 6
static char g_ctlNames[CTL_NAMES][20];
volatile int g_ctlN = 0;
volatile int g_ctlLocked = 0;

// Said once per attempt, not once per tick: ctl_hold re-applies the lock every frame.
static volatile int g_ctlSaidNoLeader = 0;

void ctl_lock(void) {
    char name[20];
    // Nothing to lock yet. Deliberately does NOT mark the lock as taken: the next tick tries again.
    if (!leader_name(name)) {
        if (!g_ctlSaidNoLeader) {
            g_ctlSaidNoLeader = 1;
            ff13logv("[FF13-SRP] battle picker: nothing to hold still yet -- no leader to read; "
                     "trying again each frame\n");
        }
        return;
    }
    g_ctlSaidNoLeader = 0;
    int known = 0;
    for (int i = 0; i < g_ctlN; i++) if (strcmp(g_ctlNames[i], name) == 0) { known = 1; break; }
    if (!known && g_ctlN < CTL_NAMES) {
        strncpy(g_ctlNames[g_ctlN], name, 19);
        g_ctlNames[g_ctlN][19] = 0;
        g_ctlN++;
    }
    vm_switch_control_name(name, 0, !known);      // said out loud the first time for each name
    // The menu flag is HELD (anybody may set it back), but by reading first -- see vm_top_menu_get.
    // field_ui_lock and hold_still_begin are NOT held: they save state to put back afterwards, so
    // running them again would save the state they themselves just wrote.
    if (vm_top_menu_get() != 0) vm_top_menu(0);
    if (!g_ctlLocked) { g_ctlLocked = 1; field_ui_lock(); hold_still_begin(); }
    else if (!known)
        // A second pick while the first lock is still on: this adds a name and nothing else, since
        // the field UI bits and the position hold were taken by the branch above.
        ff13logv("[FF13-SRP] battle picker: already holding -- \"%s\" added to the lock, but the "
                 "field UI and the position hold were taken by an earlier one\n", name);
}
// Re-taken EVERY frame while the cover is up. Twice a second was tried and left the party walkable.
void ctl_hold(void) {
    if (!g_ctlLocked) return;
    ctl_lock();
}
// Give the leader the field back whoever took it away. ctl_unlock only undoes what THIS mod locked,
// which is no use against the game's own lock: an event fight ends by handing the run to a story
// that is not running, so nobody gives control back.
void ctl_force_on(void) {
    char name[20];
    if (!leader_name(name)) return;
    ff13logv("[FF13-SRP] battle picker: giving \"%s\" the field back -- the fight's own ending "
             "took it and there was no story left to return it\n", name);
    vm_switch_control_name(name, 1, 0);
}

void ctl_unlock(void) {
    g_ctlSaidNoLeader = 0;             // a fresh lock gets to say it again
    if (!g_ctlLocked && g_ctlN == 0) return;
    for (int i = 0; i < g_ctlN; i++) vm_switch_control_name(g_ctlNames[i], 1, 1);
    g_ctlN = 0;
    if (g_ctlLocked) { g_ctlLocked = 0; vm_top_menu(1); field_ui_unlock(); hold_still_end(); }
}

// Start a fight the way a story fight starts: the same native, with an ARENA. The native reads
// nothing from the context except [ctx+0x114], popping three slots, so a forged context is enough.
void vm_start_battle(const char* arena, uint32_t flags, uint32_t btsc) {
    if (!g_base || !arena || !arena[0]) return;
    uint8_t  ctx[0x200];
    uint32_t args[6];                                     // 3 slots of {tag, value}
    memset(ctx, 0, sizeof ctx);
    args[0] = 0; args[1] = (uint32_t)(uintptr_t)arena;    // slot0 = arena / script name
    args[2] = 0; args[3] = flags;                         // slot1 = flags
    args[4] = 0; args[5] = btsc;                          // slot2 = scene number
    *(uint32_t*)(ctx + SB_ARGSTACK_OFF) = (uint32_t)(uintptr_t)args;
    ((vmNative1_t)(uintptr_t)(g_base + RVA_STARTBATTLE))(ctx);
}

// The story's event walks the party onto the arena BEFORE the battle starts -- the battle's _init
// simply takes each character's position as it finds it -- so the picker has to do the same.
#define ARENA_PARTY 3
#define ARENA_SLOTS 32          // one character owns several pool slots
// Formation: leader in the middle, lower character number on the left. A field character carries
// its charaspec at +0x4C ("p_snow"); the game's own list (0xAD3800 -> 0x282A510 and up) fixes the
// pcNNN numbering the battle prints, zero-based:
//     0 lightning  1 snow  2 hope  3 sazz  4 vanira  5 fang
// NOT the order the charaspec strings are built in -- that one is by spec, not by character.
static const char* const kCharaOrder[] = {
    "p_lightning", "p_sazz", "p_snow", "p_hope", "p_vanira", "p_fang"
};
int chara_id_of(const char* nm) {
    if (!nm) return 99;
    for (int i = 0; i < (int)(sizeof kCharaOrder / sizeof kCharaOrder[0]); i++)
        if (strcmp(nm, kCharaOrder[i]) == 0) return i;
    return 99;                                  // not one of the six; never mistaken for someone
}
// Pool slots 0 and 2 are the PARTY REFERENCE, not characters (the warp reads slot 2, then 0, as
// "where the party is"). Moving them overwrites the reading everything else is measured against.
#define ARENA_SKIP  "pool slots 0 and 2 are the party reference, not characters"
volatile uint32_t g_apIdx[3];            // the slot the character object itself carries
volatile float    g_apSrc[3][3];         // where it was standing when it was recognised
volatile uint32_t g_apObj[3];            // the character object, for its facing
volatile uintptr_t g_apRot[3];           // where its basis was found, once
volatile float    g_apPos[3][3];
volatile int      g_apCharId[3];
volatile float    g_apLeadD[3];          // squared ground distance to the party reference
char              g_apName[3][24];
volatile int      g_apN = 0;
volatile DWORD    g_apUntil = 0;         // let go once the fight has read the positions
volatile float    g_apRefX = 0, g_apRefZ = 0;
volatile int      g_apReady = 0;         // all three recognised and placed
volatile int      g_apStage = 0;         // ...but their places wait on the battle's stage
volatile int      g_apFace = 0;          // turn them at the boss on the frame they land
volatile int      g_apWp[3] = {0,1,2};   // which of the three formation places each one has
#ifdef FF13_DIAG
volatile int g_faceLogN = 0;
#endif
// The warp's own state, declared here because the event door arms the same warp.
unsigned char g_candFlags[WARP_MAXSLOT];

// Set while a picked arena fight runs; read by place_boss on the commit hook (defined below).
volatile int   g_placeBoss = 0;
// One slot is not enough: a boss with parts arrives as several actors and the battle gives none of
// them a transform, so locking only the first leaves the rest at the world origin.
volatile uint32_t g_bossSlots[BOSS_SLOTS];
volatile int   g_bossSlotN = 0;
volatile float g_bossSlotPos[BOSS_SLOTS][3];   // ...and where each of them was put
volatile float g_bossX = 0, g_bossY = 0, g_bossZ = 0;
volatile int   g_placeLogged = 0;
volatile float g_bossOx = 0, g_bossOy = 0, g_bossOz = 0;  // the scene's own enemy offset

// One place for all of them is right for a body and its parts and wrong for separate enemies; a
// scene that needs more than one place lists them in en[], in its own file's order.
volatile unsigned char g_enUsed[ARENA_ENEMIES];   // which of them have been handed out
#ifdef FF13_DIAG
// Which way the boss is pointing has never been set by us -- only where it stands -- and on
// btsc07042 it starts facing somewhere of its own and turns to the party afterwards, which the
// real fight does not do. Before writing anything into a battle actor, find out whether it even
// carries the same kind of basis a field character does. READ ONLY: the search that found the
// party's facing (find_basis: a matrix whose fourth row is where this thing stands and whose first
// two rows are unit length) is run over the boss's own object and what it finds is logged. Nothing
// is written until that says there is something worth writing to.
volatile uint32_t g_bossObj = 0;
volatile DWORD    g_bossProbeAt = 0;
#endif
volatile DWORD g_bossOpenUntil = 0;   // while the fight is being built, and no longer

// The party has to be moved onto the stage too: started this way nobody does it. The commit is
// overridden for a moment (the warp's trick) and then released, so they can move once there.
const struct ff13_arena_s* g_arena = NULL;

// Scene -> the arena its story flow uses, the flags that came with it, and where the scene puts
// its enemy (btsc#####.wdb enemy0 cells 9 and 11). Hand-checked per entry: the join from a scene
// to an arena name (mapset_loc###.wdb) is not built, so nothing here is generated.
const ff13_arena* g_arenaPending = NULL;   // waiting for the party to land

// The decks this fight is meant to have, waiting for the fight to start. See decks_apply.
const signed char (*g_decksWant)[3] = NULL;
volatile int      g_decksWantN = 0;
volatile uint32_t g_decksWantFor = 0;     // ...for which battle scene
volatile DWORD    g_decksAgainAt = 0;     // one repeat, in case the game writes once more
volatile uint32_t g_optimaWatch = 0;      // last party+deck state seen, 0 = never read yet
volatile DWORD    g_optimaWatchAt = 0;    // next sample due (this runs on every frame)
volatile int      g_optimaWatchN = 0;     // how many readings have been written out
volatile DWORD    g_arenaStartAt = 0;
volatile DWORD    g_arenaCamUntil = 0;     // the window the camera is aimed by us
volatile int      g_wsSeen = 0;            // and whether this fight's shot was seen
volatile uint32_t g_camCtl = 0;            // the battle camera control, once it is seen
volatile DWORD    g_camHoldUntil = 0;      // and how long the opening is held there

static const ff13_arena kSceneArena[] = {
    // MEASURED off the story fight's own _init lines, not derived from the scene data: the two do
    // not agree (btsc12090 authors z 9.0 and the game places 10.2669), and the authored figure
    // left the party two units too close with melee swinging through thin air.
    { 12090, "bt_lasd_ev300cb", 0x8, 0.0f, 0.0f, 10.2669f,
      { { -1.3622f, 196.031f, 2.853f },
        {  0.0f,    196.031f, 2.043f },
        {  1.54f,   196.031f, 3.447f } },
      { 0.0f, 196.031f, -12.8669f },
      // The camera, read out of the story fight's own control a second and a half in.
      { -0.997f,   0.000f, -0.076f, 0.0f,
        -0.007f,   0.996f,  0.093f, 0.0f,
         0.076f,   0.093f, -0.993f, 0.0f,
        -0.545f, 196.906f,  7.685f, 1.0f },
      {  0.399f, 198.074f, -4.719f },
      { -1.689f, 196.343f, 11.666f },
      { -0.669f, 198.214f, -2.871f },
      { -1.208f,  -0.673f,  4.935f },
      { -0.858f,   0.243f, -0.056f },
      {  1.289f,   3.072f }, 1200.0f, 1, NULL, NULL, NULL,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },       // sp: the measured places above are used
      { { NULL, 0, 0, 0 } },                           // en: one enemy
      { NULL, NULL },                                  // btlBg
      // Event door: a SCRIPT-event root (startCinema mode 0 with its own success label, evScript).
      // z029/scr100.java efCallEvLasd300 -> bf_ev_300, whose callback stages the room and then
      // starts ev_lasd_300, whose callback calls this fight.
      { "bf_ev_300", "bf_ev_300_cb", NULL, "suc_e_o_flmd_e" },
      { NULL }, { NULL, NULL }, NULL, 1, 0 },

    // btsc07010, the Shiva-bike fight (Snow alone, already riding). Picked cold it crashed: its
    // whole opening state is set up by a chain, and the chain here is the game's own RESTART path
    // (pmpm070Resta -> ev_pmpm_070 -> fight). evSetup is NULL on purpose -- the chain's own java
    // calls setup_pmpm070, which brings the party, the paradigms AND the Eidolon.
    // The geometry fields are all zero: an event door places nobody, the chain does.
    // chset/bg are named here because the game never enters this chain from nothing -- its
    // checkpoint loader (fs_Restart_pmpm_ev060) loads them first, and the mod does that half.
    { 7010, "bt_pmpm_070_cb", 0x8, 0.0f, 0.0f, 0.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      // restartAfterWin MUST stay 0 here: this chain marks its restart point AT the fight
      // (sfSetFlagForRestart("rp_pmpm_ev060")), so routing a win to the checkpoint loops it.
      // Verified on hardware. The win goes to the story instead, which is safe for a door that ran
      // the chain -- bt_pmpm_070_cb has a real thread behind it.
      { 0, 0 }, 0.0f, 0, "chset_z008_c020", "mset_pmpm_st07", NULL,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { { NULL, 0, 0, 0 } }, { NULL, NULL },
      // Entered one cutscene FURTHER BACK than the fight's own root, so that the game's own event
      // mode holds the pause menu shut (a script-event root leaves only setFieldTopMenuOpenFlag
      // doing it, which is not a complete gate) and so that efSetFieldAfter065 is not skipped.
      // The gate moves with the entry point: evFlag is the value the step before 065 leaves.
      { "ev_pmpm_065", "ev_pmpm_065_cb", "a00", NULL },
      { NULL }, { NULL, NULL }, NULL, 0, 0, { 0, 0, 0 },
      { "ev_pmpm_065", "a00" }, 6760, 0,
      NULL },

    // btsc07031, the first Ushumgal fight in Palumpolum -- MEASURED off the story fight, after its
    // event door was built and taken out again. The third party place is the second one mirrored
    // across the leader (the real fight is two), and only that one is not measured.
    { 7031, "bt_pmpm_200_cb", 0x8, 1.1416f, -0.010f, 2.4392f,
      { { 289.486f, 428.990f, -28.896f },     // left (mirrored -- see above)
        { 288.727f, 428.990f, -28.298f },     // centre (leader) = pc001
        { 287.968f, 428.990f, -27.700f } },   // right = pc003
      { 282.465f, 428.990f, -30.911f },
      // The camera, read out of the story fight's control at +1750ms, by which time it is at rest.
      { -0.878f,   0.000f,   0.478f, 0.0f,
         0.010f,   1.000f,   0.018f, 0.0f,
        -0.478f,   0.020f,  -0.878f, 0.0f,
       290.530f, 431.193f, -22.306f, 1.0f },
      { 286.683f, 431.358f, -29.367f },
      { 289.615f, 429.339f, -23.480f },
      { 286.848f, 431.272f, -28.841f },
      {  -0.981f,  -1.849f,   -1.141f },
      {  -0.331f,  -0.109f,    0.119f },
      {   0.500f,  -2.665f }, 1200.0f, 1 },

    // btsc07050 goes in on its FULL story flow, and the EVENT-battle entry is load-bearing: only
    // that route's Restart runs the real checkpoint loader. A field-encounter start gets the quick
    // battle-snapshot retry instead, which restores a staged world the story never walks.
    // Do NOT also set the party from kScenePartyScript here: the paradigms only come with
    // setup_pmpm310, which the chain runs on its own thread.
    { 7050, "bt_pmpm_310_cb", 0x8, 0.0f, 0.0f, 0.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      // The track is NAMED rather than resolved: this chain replays itself after a Restart, so the
      // end-of-fight sampler kept learning whatever the replay had reached. Naming it also stops
      // the learner running at all.
      { 0, 0 }, 0.0f, 1, NULL, NULL, "music_bossa",
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { { NULL, 0, 0, 0 } }, { NULL, NULL },
      { "pt_set_af300", "pt_set_af300_cb", NULL, "suc_fx2_on_fldd" },
      { NULL }, { NULL, NULL }, NULL, 1, 0, { 0, 0, 0 },
      { "ev_pmpm_310", "a00" }, 7300,
      // ...but not its music: the chain calls sfSetBgmVolContinue ("keep what is playing"), which
      // is wrong when the picker started this from wherever you were standing.
      1,
      NULL,          // evSetup: nothing to arm. The chain's own java calls setup_pmpm310 for us
      // A converted end must NOT reload the chain's own checkpoint: rp_pmpm_ev301 replays the
      // fight. Re-marked at the save point's restart point instead, whose loader is a complete
      // field load ending in sfFieldModeUcon.
      .evEndRp = "rp_pmpm_ev300" },

    // btsc07037 (second Ushumgal fight, Hope alone). DERIVED, not measured: the enemy offset and
    // party place are the scene file's own and are unchecked. Its chain (bf_ev_300 ->
    // efBeforEvPmpm300) is a room-stager that ends in sfPartyChange01("hope"), i.e. the party
    // change is this fight's own definition rather than a chapter set-up.
    { 7037, "bt_pmpm_300_cb", 0x8, 0.0f, 0.0f, 8.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1, NULL, NULL, NULL,
      { { 0, 0, 0 }, { 0.0f, 0.0f, -3.0f }, { 0, 0, 0 } },              // the scene names pc1 only
      { { NULL, 0, 0, 0 } }, { NULL, NULL },
      { "bf_ev_300", "bf_ev_300_cb", NULL, "suc_e_o_flmd_e" },
      { NULL }, { NULL, NULL }, NULL, 1, 0 },

    // btsc07042, MEASURED off the real fight. The enemy height is 9.05, not the 14.0 the wdb
    // authors; the third party place is the second mirrored across the leader (the real fight is
    // two). camHoldMs is 0 DELIBERATELY: this fight opens with an action camera of its own
    // ("ca_m020_ac400"), and holding the measured pose over the top of it fights the opening.
    { 7042, "bt_pmpm_400_cb", 0x8, -0.473f, 9.050f, 67.570f,
      { {  184.092f, 360.300f, -738.309f },     // left  (mirrored -- see above)
        {  180.876f, 360.300f, -736.821f },     // centre (leader) = pc000
        {  182.418f, 360.300f, -740.012f } },   // right = pc003
      {  133.480f, 369.350f, -691.960f },
      {  0.682f,  -0.000f,   0.731f, 0.0f,
         0.184f,   0.968f,  -0.172f, 0.0f,
        -0.708f,   0.252f,   0.660f, 0.0f,
       188.966f, 360.401f, -744.584f, 1.0f },
      { 158.459f, 371.267f, -716.117f },
      { 185.930f, 360.855f, -743.586f },
      { 158.223f, 365.249f, -717.492f },
      {   0.169f,   0.455f,   -2.074f },
      {  -1.717f,  -5.399f,   -0.298f },
      {   0.500f,  -0.815f }, 0.0f,
      // Ends at the checkpoint by telling the worker, NOT by posting the trigger by hand: posting
      // it here was accepted and then the restart job never ran (45s stuck in STATE_RESTART).
      1, NULL, NULL, NULL,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },       // sp: measured places above are used instead
      { { NULL, 0, 0, 0 } },                           // en: one body, and its parts ride with it
      { NULL, NULL },                                  // btlBg
      // Entered at ev_pmpm_380, NOT at the ev_pmpm_400 that calls the fight: what happens between
      // the two cutscenes (a script event that activates the boss's area group and waits for it)
      // cannot be imitated. Entering at 400 left the boss unactivated and the camera unposed.
      // No pack and no staging: the story loads neither at 380.
      { "ev_pmpm_380", "ev_pmpm_380_cb", NULL, "suc_ev_on_fldd" },
      { NULL },
      { NULL, NULL } },

    // btsc01001, the first Manasvin Warmech fight, MEASURED off the story fight. Lightning and
    // Sazh, so the third place is the second mirrored across the leader.
    { 1001, "bt_hang_000_cb", 0x8, -0.018f, 0.098f, 12.780f,
      { {  251.032f, 10.140f, 567.254f },      // left  (mirrored -- see above)
        {  248.809f, 10.140f, 569.309f },      // centre (leader) = pc000
        {  246.586f, 10.127f, 571.364f } },    // right = pc002 (Sazh)
      {  241.224f, 10.198f, 554.290f },
      { -0.703f,   0.000f,   0.711f, 0.0f,
         0.053f,   0.997f,   0.053f, 0.0f,
        -0.709f,   0.075f,  -0.701f, 0.0f,
       252.614f,  11.205f, 572.174f, 1.0f },
      { 245.396f, 11.967f, 565.033f },
      { 255.262f, 12.075f, 575.511f },
      { 244.911f, 11.981f, 565.270f },
      {   2.649f,  0.870f,   3.337f },
      {  -0.365f,  0.001f,   0.356f },
      {   0.500f, -2.351f }, 1200.0f, 1, NULL, NULL, NULL,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },       // sp: the measured places above are used
      { { NULL, 0, 0, 0 } },                           // en: one enemy
      { NULL, NULL },                                  // btlBg
      // NO event door, for this fight or btsc01016: both were tried and both CRASHED the moment
      // their event was asked for, with and without staging. These events are part of the chapter
      // starting, not a set-piece a fight hangs off, so nothing they assume holds when they are
      // asked for from a field that is already up. Neither needs one -- both entries are measured.
      { NULL, NULL, NULL, NULL }, { NULL }, { NULL, NULL }, NULL, 0, 0 },

    // btsc01016, the second Manasvin fight, DERIVED: its authored enemy placement is
    // byte-identical to btsc01001's and it is the same model, so that entry's measured offset
    // carries over. No party places and no camera -- kPartyStage places the party once the battle
    // has a stage. The two loads are the story's own staging (zone/z016/scr100.java).
    { 1016, "bt_hang_020_cb", 0x8, -0.018f, 0.098f, 12.780f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1,
      // The offline event data would hand this scene music_bossa (its cutscene's track); the fight
      // itself plays the zone's ordinary battle theme, so "" leaves the music alone.
      "chset_z016_defd", "mset_hang_0102c", "",
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },       // sp
      { { NULL, 0, 0, 0 } },                           // en
      { NULL, NULL },                                  // btlBg
      // No event door -- see btsc01001. Same chapter, same shape, same reason.
      { NULL, NULL, NULL, NULL }, { NULL }, { NULL, NULL }, NULL, 0, 0 },

    // btsc02017, Anima -- DERIVED, and the first entry to use the scene file's own placements for
    // more than one enemy and for the party (this fight stands its party three to four units back
    // where btsc12090 stands its seven to eight). Four real actors, each wanting its own place.
    // The party places here are WORLD positions rather than stage offsets, which makes this fight
    // take the measured path: the three are walked onto the arena BEFORE the call, as the story
    // event does, rather than placed once the battle has a stage.
    { 2017, "bt_hgin_320_cb", 0x8, 0.0f, -1.63f, 8.8f,
      { {  1.5f, 159.0f, -176.72f },     // the lower-numbered of the two subs
        {  0.0f, 159.0f, -177.72f },     // centre (leader)
        { -1.5f, 159.0f, -176.72f } },   // the higher-numbered
      {  0.0f, 157.37f, -189.52f },
      { -0.978f,   0.000f,   0.210f, 0.0f,
         0.009f,   0.999f,   0.042f, 0.0f,
        -0.210f,   0.043f,  -0.977f, 0.0f,
         1.800f, 160.137f, -175.659f, 1.0f },
      { -0.115f, 160.532f, -184.578f },
      {  2.323f, 159.360f, -173.907f },
      {  0.284f, 161.815f, -183.928f },
      {  0.468f,  -0.786f,    1.681f },
      {  0.167f,   1.326f,    0.200f },
      {  0.875f,  -2.941f }, 1200.0f, 1,
      "chset_z017_def5", "mset_hgin_0708b", NULL,
      // The scene's own party offsets: the source of the world places above, kept so that a fight
      // falling back to stage placement uses its own figures rather than btsc12090's.
      { {  1.5f, 0.0f, -4.0f },      // pc0
        {  0.0f, 0.0f, -3.0f },      // pc1 (centre, the leader)
        { -1.5f, 0.0f, -4.0f } },    // pc2
      { { "m203",  0.0f, -1.63f, 8.8f },
        { "m223", -4.0f,  0.0f,  3.0f },
        { "m224",  4.0f,  0.0f,  3.0f },
        { "m225",  0.0f,  0.0f,  9.0f } },
      // The mapsets bt_scene names for the FIGHT rather than the field (cell[3] the arena's own,
      // cell[4] the room around it). Asked for outright: without them the background is a
      // different set of objects from the story fight's.
      { "msbt_hgin_08", "msbt_hgin_0708" },
      // Event door: ev_hgin_320's callback starts this fight itself, so nothing here has to.
      { "ev_hgin_320", "ev_hgin_320_cb", "a00", "suc_ev_on_fldd" },
      // What the story's "before" function does: the four enemies exist as FIELD characters before
      // the cutscene, because the cutscene acts with them. Their character set is this entry's.
      { "en_mon_0021", "m203", "en_mon_0048", "m224",
        "en_mon_0049", "m225", "en_mon_0050", "m223" },
      // The story turns auto music off a scene EARLIER, so the battle step rolls nothing and the
      // chain's track carries through. Fired directly that bit is missing -- set it the same way.
      // Asking for the track instead goes silent: the chain loads it under its own group.
      { NULL, NULL }, .evQuiet = 1 },

    // btsc03037, the Shiva sisters -- an event door whose root script event is reached from another
    // FIGHT (btsc03036's callback) rather than from a trigger box. It needs TWO data packs for the
    // one loaded cutscene, which is why evPack2 exists.
    // evPos IS REQUIRED: this fight is SEAMLESS off FIELD characters, so the battle takes whoever
    // is standing there. Fired from elsewhere in the zone the group activated and the enemies were
    // built, and the fight happened around the party with the sisters 1300 units away.
    // The party itself is set from kScenePartyScript (Snow alone). The arena fields are the scene
    // file's own figures, unmeasured. Music is left to the story.
    { 3037, "bt_hgcr_195_cb", 0x8, -2.5f, 0.0f, 5.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1,
      // What fs_Restart_hgcr_ev195 loads to put this fight back up from its checkpoint.
      "chset_z002chpt", "mset_hgcr_0304e", NULL,
      { { 0, 0, 0 }, { 0.0f, 0.0f, -3.0f }, { 0, 0, 0 } },        // the scene names pc1 only
      { { "s251", -2.5f, 0.0f, 5.0f },
        { "s253",  2.0f, 0.0f, 3.0f } },
      { NULL, NULL },                                            // bt_scene names one, the field's
      { "hgcr195bf", "hgcr195bf_cb", NULL, "suc_fx2_on_fldd" },
      { NULL }, { NULL, NULL }, "b00", 1, 0,
      { 53.9916f, -74.7882f, -767.53f },              // measured: where the story fight stands
      { "ev_hgcr_195", "a00" }, 2620, 0,
      // evSetup: the party change one fight upstream is two things -- natives (kScenePartyScript)
      // and a script, which can only run on the chain's own thread. This is the script half.
      "setup_hgcr190" },

    // btsc11709, Barthandelus in Eden -- btsc12090's shape (a script-event root with one cutscene
    // under it). The chain activates the boss's area group and WAITS for it, which is the part an
    // event door exists for. evPos is the author's own figure from the script; the coordinates are
    // inert in the game's hands, so the picker does the walk the script does not.
    { 11709, "bt_gpwo030_cb", 0x8, 0.0f, 0.0f, 9.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1,
      "chset_z027_btl", "mset_gpwo_0010", NULL,
      { {  1.5f, 0.0f, -9.0f },        // this scene authors all three, and stands them far back
        {  0.0f, 0.0f, -8.0f },
        { -1.5f, 0.0f, -9.0f } },
      { { NULL, 0, 0, 0 } },
      // bt_scene cell[3]: the battle's own arena, which the field never loads.
      { "msbt_gpwo_0011", NULL },
      { "gpwo_030_bf", "gpwo_030_bf_cb", NULL, "suc_fx2_on_fldd" },
      { NULL }, { NULL, NULL }, NULL, 1, 0, { 62.31f, 31.59f, -502.68f },
      { "ev_gpwo_030", "a00" }, 13450, 0 },

    // The three Eidolon fights, all the same shape: their chain changes the party AND settles its
    // paradigms, and the paradigms are the half nothing outside the chain can do.
    // evEvFlag is what makes the chain's `if (restart flag)` branch run -- the fade-in and the
    // preparation menu, which the story does not need (it arrives from a lit cutscene) and a cold
    // pick does. Without it btsc11412 came up black.
    //     z018 Bahamut flg_evyobi_002   z021 Alexander flg_evyobi_000   z022 Hecatoncheir _003
    // evFlag only has to be BELOW the chain's own gate on scenario work 0; the chain sets it
    // forward itself. Too high and the cutscene does not play at all.
    { 10060, "bt_fark_120_cb", 0x8, 0.0f, 0.0f, 0.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1,
      NULL, NULL, NULL,                     // the chain loads what it needs
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { { NULL, 0, 0, 0 } },
      { NULL, NULL },
      // Rooted at bf_110, the step where efBeforeEvFark110 changes the party. The door is here
      // for the WAIT, not the paradigms: a door-less fight must hold the picker still while the
      // character set loads and nothing can tell it when that is done (see RVA_SYNCWAIT).
      { "bf_110", "bf_110_cb", NULL, "suc_fx2_on_fldd" },
      { NULL }, { NULL, NULL }, NULL, 1, 0, { 0, 0, 0 },
      // No evEvFlag: the flag works but the preparation menu it brings belongs to a checkpoint
      // restart. btsc11412 keeps it only because there the alternative is a black screen.
      { "ev_fark_110", "a00" }, 10140, 0,
      // Raised by this fight's restart loader; the chain opens its preparation menu whenever it
      // is up, so the pick takes it down (see rpflag_watch).
      .evRpFlag = "flg_evyobi_002" },

    { 11100, "bt_gpcg050_cb", 0x8, 0.0f, 0.0f, 0.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1,
      NULL, NULL, NULL,                     // the chain loads its own
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { { NULL, 0, 0, 0 } },
      { NULL, NULL },
      // Two cutscenes back, not one: entered at ev_gpcg_050 the chain runs correctly and the party
      // is still whoever was walking around, because the party change is not in that event.
      { "bf_gpcg_040", "bf_gpcg_040_cb", NULL, "suc_fx2_on_fldd" },
      { NULL }, { NULL, NULL }, NULL, 1, 0, { 0, 0, 0 },
      { "ev_gpcg_040", "a00" }, 11950, 0,
      // A script the game itself never calls: common.sfCallSetUpPartyGpcg050 exists and nothing
      // invokes it. Running it costs nothing; the log says whether it is even registered.
      "setup_gpcg050",
      .evRpFlag = "flg_evyobi_000" },

    { 11412, "bt_gpyu010_cb", 0x8, 0.0f, 0.0f, 0.0f,
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { 0, 0, 0 },
      { 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
      { 0, 0 }, 0.0f, 1,
      NULL, NULL, NULL,                     // likewise
      { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } },
      { { NULL, 0, 0, 0 } },
      { NULL, NULL },
      // Rooted at the head of the chain (ev_gpyu_010_fd1), not its tail: the party change is a
      // script event of its own two steps in, not a native this table could make. Vanille and Fang
      // are a pair, not a trio, and nothing outside a party change makes the game accept that.
      { "ev_gpyu_010_fd1", "ev_gpyu_010_fd2", NULL, "suc_fx2_on_fldd" },
      { NULL }, { NULL, NULL }, NULL, 1, 0, { 0, 0, 0 },
      { "ev_gpyu_010", "a00" }, 12400, 0, NULL, 0,
      // Raised on purpose: this chain keeps its fade-in inside that branch, and without it the
      // fight is played on a black screen. The menu it also opens is stopped at its own native.
      "flg_evyobi_003",
      .evRpFlag = "flg_evyobi_003" },
};

// Where the party stands, as an offset on the stage. Read off btsc12090, the one fight measured in
// full. Anchored to the ORIGIN, not to the enemy: measuring from the enemy was tried and put them
// fifty-two units forward, standing on nothing, because the stage transform carries no height of
// its own -- everything it places sits at the origin's y.
static const float kPartyStage[3][3] = {   // left, centre (leader), right -- same order as wp[]
    {  1.3622f, 0.0f, -5.453f },
    {  0.0f,    0.0f, -4.643f },
    { -1.54f,   0.0f, -6.047f },
};

// A measured entry carries absolute party places; a derived one is placed from the stage instead.
// Row 1's y is the tell: a measured one is a world height, a derived one is zero.
int arena_has_party(const struct ff13_arena_s* a) { return a && a->wp[1][1] != 0.0f; }

// Every scene file authors party offsets next to its enemies, so an entry carrying the scene's own
// figures uses them and everything else falls back to btsc12090's measurement. The difference
// matters: a fight standing its party three units back from a boss nine units out differs at seven.
const float* arena_party_stage(const struct ff13_arena_s* a, int i) {
    if (a && (a->sp[0][0] != 0.0f || a->sp[0][2] != 0.0f
              || a->sp[1][0] != 0.0f || a->sp[1][2] != 0.0f))
        return a->sp[i];
    return kPartyStage[i];
}

// Which of the scene's authored enemy places this actor gets. One place = everyone shares it,
// which is right for a body and its parts. With several, match on the model name the battle gives
// us; an unlisted name takes the next unclaimed place, i.e. the scene file's own order.
void arena_enemy_pick(const char* name, float* ox, float* oy, float* oz) {
    const struct ff13_arena_s* a = g_arena;
    *ox = g_bossOx; *oy = g_bossOy; *oz = g_bossOz;
    if (!a || !a->en[0].name) return;
    int pick = -1;
    if (name && name[0]) {
        for (int i = 0; i < ARENA_ENEMIES && a->en[i].name; i++) {
            size_t n = strlen(a->en[i].name);
            if (g_enUsed[i] || strncmp(a->en[i].name, name, n) != 0) continue;
            pick = i;
            break;
        }
    }
    if (pick < 0)
        for (int i = 0; i < ARENA_ENEMIES && a->en[i].name; i++)
            if (!g_enUsed[i]) { pick = i; break; }
    if (pick < 0) return;                 // more actors than the scene lists: they share the last
    g_enUsed[pick] = 1;
    *ox = a->en[pick].x; *oy = a->en[pick].y; *oz = a->en[pick].z;
}

// What the fight's own story flow does to the lighting, with the arguments it uses. Separate from
// the arena entry because the source is different: that comes from the scene data, this from the
// zone script. Only the light groups and the enemies are listed; the party comes from the fight.
// USE THE SCRIPT'S OWN MODE AND TIME. Instant (mode 0, time 0) was tried and the room stayed dark:
// the call names the env it moves FROM as well as TO, so with no time to move it never arrives.
// Mode 4 is AccFastFast and the time is in seconds.
#define LIGHT_BGS 2
#define LIGHT_WHO 6
#define DRAWENV_MODE 4
#define DRAWENV_TIME 1.0f
static const struct { uint16_t btsc; const char* from; const char* to;
                      const char* bg[LIGHT_BGS]; const char* who[LIGHT_WHO];
                      const char* boxOn; const char* boxOff;
                      uint16_t sweepLo, sweepHi; }
kSceneLight[] = {
    { 2017, "dwev_BG_DF008test", "dwev_BG_DF008bL",
      { "lbg_008_00001", "lbg_182_00001" },
      { "en_mon_0021", "en_mon_0048", "en_mon_0049", "en_mon_0050" },
      "scr_env_m008L", "scr_env_m008D", 0, 255 },
};

// The two groups the script names are not the whole room, so the sweep names every lbg id in the
// range and lets the room take its own. Safe because the call looks nothing up: 0xBE0EF0 ->
// 0x6FA620 allocates a 0x38 record, copies the names/mode/time into it and queues it (and queues
// nothing when the pool is full), so a name for an object not in this room costs one record.
// Spread over frames so the pool is never asked for hundreds at once.
#define LIGHT_SWEEP_PER_TICK 8
#define LIGHT_SWEEP_INST 6
volatile int  g_lightSweep = -1;      // next id to ask for, -1 = not started yet
volatile int  g_sweepDone = 0;        // ...and whether it has been through

// [battle_swap] arena_camera = 0 turns the opening-camera override off entirely.
volatile int g_arenaCamera = 1;

// TIMING: the map and the characters are asked for at DIFFERENT moments. The map's ease must be
// given its second of field time to FINISH before the battle takes the field over -- sending both
// immediately before the call lit the characters and left the room dark.
// And the room is lit only once the party has been walked onto the arena (a draw env applies to
// the objects that are there to receive it), then again in case anything streamed in meanwhile.
volatile DWORD g_lightMapAt = 0;      // when the room was last asked to brighten
volatile int   g_lightMapN = 0;       // how many times, this fight
// -> 1 if this scene has a lighting entry at all.
int scene_light_apply(uint32_t btsc, int what, const char* when) {
    for (int i = 0; i < (int)(sizeof kSceneLight / sizeof kSceneLight[0]); i++) {
        if (kSceneLight[i].btsc != btsc) continue;
        if (what & LIGHT_MAP) {
            for (int b = 0; b < LIGHT_BGS && kSceneLight[i].bg[b]; b++)
                vm_drawenv_bg(kSceneLight[i].bg[b], kSceneLight[i].from, kSceneLight[i].to,
                              DRAWENV_MODE, DRAWENV_TIME);
            vm_box(kSceneLight[i].boxOn, 1);
            vm_box(kSceneLight[i].boxOff, 0);
            g_lightMapAt = GetTickCount();
        }
        if (what & LIGHT_CHARA) {
            for (int w = 0; w < LIGHT_WHO && kSceneLight[i].who[w]; w++)
                vm_drawenv_chara(kSceneLight[i].who[w], kSceneLight[i].to,
                                 DRAWENV_MODE, DRAWENV_TIME);
            for (int p = 0; p < g_apN && p < 3; p++) {
                // The pool knows them as "p_lightning"; the script's Chara lookup wants "lightning".
                const char* nm = g_apName[p];
                if (!nm[0]) continue;
                if (nm[0] == 'p' && nm[1] == '_') nm += 2;
                vm_drawenv_chara(nm, kSceneLight[i].to, DRAWENV_MODE, DRAWENV_TIME);
            }
        }
        ff13logv("[FF13-SRP] battle picker: btsc%05u lit as its own story flow lights it "
                 "(draw env \"%s\", %s)\n", btsc, kSceneLight[i].to, when);
        return 1;
    }
    return 0;
}

// One tick's worth of the sweep. -> 1 while there is more to do.
int scene_light_sweep(uint32_t btsc) {
    for (int i = 0; i < (int)(sizeof kSceneLight / sizeof kSceneLight[0]); i++) {
        if (kSceneLight[i].btsc != btsc) continue;
        if (kSceneLight[i].sweepHi < kSceneLight[i].sweepLo) return 0;   // no sweep for this one
        if (g_lightSweep < 0) g_lightSweep = kSceneLight[i].sweepLo;
        int done = 0;
        g_inSweep = 1;
        for (int n = 0; n < LIGHT_SWEEP_PER_TICK; n++) {
            if (g_lightSweep > (int)kSceneLight[i].sweepHi) { done = 1; break; }
            char nm[24];
            // Instance numbers past two exist, so LIGHT_SWEEP_INST must stay above 2.
            for (int v = 1; v <= LIGHT_SWEEP_INST; v++) {
                wsprintfA(nm, "lbg_%03d_0000%d", g_lightSweep, v);
                vm_drawenv_bg(nm, kSceneLight[i].from, kSceneLight[i].to,
                              DRAWENV_MODE, DRAWENV_TIME);
            }
            g_lightSweep++;
        }
        g_inSweep = 0;
        if (!done) return 1;
        ff13logv("[FF13-SRP] battle picker: btsc%05u -- asked every lbg_%03d..%03d in this room for "
                 "\"%s\"\n", btsc, kSceneLight[i].sweepLo, kSceneLight[i].sweepHi,
                 kSceneLight[i].to);
        return 0;
    }
    return 0;
}

const ff13_arena* scene_arena(uint32_t btsc) {
    for (int i = 0; i < (int)(sizeof kSceneArena / sizeof kSceneArena[0]); i++)
        if (kSceneArena[i].btsc && kSceneArena[i].btsc == btsc) return &kSceneArena[i];
    return NULL;
}

// Whether picking this fight puts a cutscene on screen first. Derived from kSceneArena rather than
// written down again in the TSV: a picked fight plays a cutscene only where THIS MOD hands it one.
// A row with no door goes in through the field encounter; evNoFight gates the cutscene off.
int scene_has_cutscene(uint32_t btsc) {
    const ff13_arena* a = scene_arena(btsc);
    return a && a->ev[0] && !a->evNoFight;
}

// Put the scene's own enemy offset through the battle's own stage transform, as the game does for
// everything it places. The Y column is NOT optional: dropping oy put a flying boss on the floor.
volatile int g_stageLogged = 0;
// [BattleMain+0x3F4] is NOT cleared between fights: until the next battle builds its own it still
// holds the last one's, which once put a party on an arena half a map away. So the stage a fight is
// staged FROM is noted first, and nothing is measured off the matrix until it has changed.
volatile float g_stagePrev[3] = {0,0,0};
volatile int   g_stagePrevHave = 0;

int stage_origin(float* o) {
    char* bm = g_base ? *(char**)(g_base + RVA_BM_PTR) : NULL;
    if (!bm || IsBadReadPtr(bm + OFF_BM_STAGE, 64)) return 0;
    const float* m = (const float*)(bm + OFF_BM_STAGE);
    o[0] = m[12]; o[1] = m[13]; o[2] = m[14];
    return 1;
}

// Which arena the leftover matrix belongs to: the same fight practised twice has an identical
// stage, and waiting for it to change would wait for ever.
const char* g_stageArena = NULL;

// Has the battle built a stage this fight can be measured against yet?
int stage_is_ours(const char* arena) {
    float o[3];
    if (!stage_origin(o)) return 0;
    if (!g_stagePrevHave) return 1;                        // nothing to be confused with
    if (g_stageArena && arena && g_stageArena == arena) return 1;   // same arena: already right
    float dx = o[0] - g_stagePrev[0], dy = o[1] - g_stagePrev[1], dz = o[2] - g_stagePrev[2];
    if (dx < 0) dx = -dx; if (dy < 0) dy = -dy; if (dz < 0) dz = -dz;
    return (dx + dy + dz) > 0.01f;
}

int stage_place(float ox, float oy, float oz, float* out) {
    char* bm = g_base ? *(char**)(g_base + RVA_BM_PTR) : NULL;
    if (!bm || IsBadReadPtr(bm + OFF_BM_STAGE, 64)) return 0;
    const float* m = (const float*)(bm + OFF_BM_STAGE);
    out[0] = ox * m[0] + oy * m[4] + oz * m[8]  + m[12];
    out[1] = ox * m[1] + oy * m[5] + oz * m[9]  + m[13];
    out[2] = ox * m[2] + oy * m[6] + oz * m[10] + m[14];
    if (out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f) return 0;   // no stage yet
    if (!g_stageLogged) {
        g_stageLogged = 1;
        // The whole basis, once per fight: which way the stage faces decides whether a party placed
        // from these offsets stands in front of the boss or behind it.
        ff13logv("[FF13-SRP] battle stage: x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f) "
                 "origin(%.2f %.2f %.2f)\n", m[0], m[1], m[2], m[4], m[5], m[6],
                 m[8], m[9], m[10], m[12], m[13], m[14]);
    }
    return 1;
}

// The game's own "is this charaspec read" call, hooked to borrow its `this`:
//     mov ecx,[esp+0x3d0] ; add ecx,0x182C ; call 0x43FD00
// Hooked at the ADD, NOT at the call: the cave re-runs the bytes it replaced and `call rel32` is
// position-dependent, so relocating it sent the game elsewhere and crashed during loading. The add
// is position-independent, and ecx there is the registry the game is about to offset itself.
const uint8_t P_ISREAD[6] = {0x81,0xC1,0x2C,0x18,0x00,0x00};
static volatile uint32_t g_isReadThis = 0;
static volatile int g_isReadLogged = 0;

void __cdecl on_isread_call(uint32_t ecx) {
    if (!ecx || IsBadReadPtr((void*)(uintptr_t)(ecx + 0x182C), 4)) return;
    g_isReadThis = ecx + 0x182C;               // the same offset the next instruction applies
    if (!g_isReadLogged) {
        g_isReadLogged = 1;
        ff13logv("[FF13-SRP] charaspec-read test object captured: %08X\n", g_isReadThis);
    }
}

// ---- what the game itself asks to be loaded, and under what names ---------------------------
// 0xB55160 is the per-charaspec load request the battle setup issues for every entry it collected:
//     0xB55160(this, arg1, arg2) -> 0x7043F0(arg1, arg2) -> 0x81E830(resourceManager, arg2)
// so arg2 is the exact key the game hands the registry. Both arguments point at 16-byte inline
// names. Read-only; this is the preload list.
const uint8_t P_CSREQ[7] = {0x55,0x8B,0xEC,0x51,0x89,0x4D,0xFC};
static volatile int g_csReqLogN = 0;
static char g_csReqKey[20];          // arg2 of the last request: the registry key the game used

void __cdecl on_charaspec_request(uint32_t ecx, uint32_t retAddr,
                                         uint32_t* pA1, uint32_t* pA2) {
    (void)ecx; (void)retAddr;
    char a[17] = "", b[17] = "";
    if (pA1 && *pA1 && !IsBadReadPtr((void*)(uintptr_t)*pA1, 16))
        { memcpy(a, (const void*)(uintptr_t)*pA1, 16); a[16] = 0; }
    if (pA2 && *pA2 && !IsBadReadPtr((void*)(uintptr_t)*pA2, 16))
        { memcpy(b, (const void*)(uintptr_t)*pA2, 16); b[16] = 0; }
    if (b[0]) { strncpy(g_csReqKey, b, sizeof(g_csReqKey) - 1);
                g_csReqKey[sizeof(g_csReqKey) - 1] = 0; }
    if (g_csReqLogN < 60) {
        g_csReqLogN++;
        ff13logv("[FF13-SRP] preload request: arg1=\"%.16s\" arg2=\"%.16s\"\n",
                 a[0] ? a : "(?)", b[0] ? b : "(?)");
    }
}

// ---- capture the registry the game tests charaspec residency against ------------------------
// The actor-creation path (real entry 0x779570) does three things in a row; the third is the test:
//     [esp+0x3d0] = 0x81E830(resourceManager, actor+0xE8)      the section registry, by name
//     [esp+0x3dc] = 0x430FC0([esp+0x3d0], arg2)                "the resource exists"
//     [esp+0x3c4] = 0x43FD00([esp+0x3d0]+0x182C, arg1, 0) != 0 "the charaspec is READ" <- this one
// Hooked ONE INSTRUCTION LATER than the load, where the actor is already in ecx: the stack-arg
// helper builds only an 8-bit displacement and [esp+0x3E8] does not fit in one.
const uint8_t P_ASECT[6] = {0x81,0xC1,0xE8,0x00,0x00,0x00};
static char g_resSection[20];        // the name that registry is registered under
static volatile int g_resSectLogged = 0;

void __cdecl on_actor_section(uint32_t actor) {
    if (g_resSection[0] || !actor || IsBadReadPtr((void*)(uintptr_t)(actor + 0xE8), 16)) return;
    char n[17];
    memcpy(n, (const void*)(uintptr_t)(actor + 0xE8), 16);
    n[16] = 0;
    if (!n[0]) return;
    strncpy(g_resSection, n, sizeof(g_resSection) - 1);
    g_resSection[sizeof(g_resSection) - 1] = 0;
    if (!g_resSectLogged) {
        g_resSectLogged = 1;
        ff13logv("[FF13-SRP] resource section for actors: \"%.16s\"\n", g_resSection);
    }
}

// ---- is a charaspec actually resident yet? ---------------------------------------------------
// The game's own question, since a character-set load is asynchronous: the resource manager at
// [0x2801B70] keeps a registry at +0x30, and 0x81E830 looks a name up in it (the same call the
// battle setup uses at 0xB54334).
#define RVA_RESMGR_P 0x2401B70       // VA 0x2801B70, the resource manager pointer
#define RVA_RESFIND  0x41E830        // VA 0x81E830, __thiscall find(registry, name)
typedef void* (__thiscall *resFind_t)(void* self, const char* name);

// The name must be a FIXED 16-byte inline buffer, not a short NUL-terminated string: whatever
// follows the NUL is compared too, which made this answer "not there" for a resident model.
#define RVA_ISREAD 0x03FD00          // VA 0x43FD00  __thiscall isRead(reg+0x182C, name, 0)
typedef int (__thiscall *isRead_t)(void* self, const char* name, int zero);

// Does this look like a live C++ object of the game's own? Its first word must be a vtable inside
// the image. The difference between a wrong pointer being rejected and being dispatched through.
static int looks_like_object(const void* p) {
    if (!p || IsBadReadPtr(p, 4)) return 0;
    uint32_t vt = *(const uint32_t*)p;
    if (vt < g_base + 0x1000 || vt > g_base + 0x2000000) return 0;
    return !IsBadReadPtr((const void*)(uintptr_t)vt, 8);
}

// DO NOT build the object here (look a registry up, add 0x182C, dispatch): that crashed twice and
// a vtable check was not enough to make it safe. The `this` is captured from the game's own call at
// 0x779648 and reused; nothing is constructed.
#ifdef FF13_DIAG
static volatile int g_isReadLogN = 0;   // first dispatches of a session, bracketed in the log
#endif
static void* charaspec_resident(const char* name) {
    if (!g_base || !name || !name[0] || !g_isReadThis) return NULL;
    char key[16];
    memset(key, 0, sizeof key);
    strncpy(key, name, sizeof key - 1);
#ifdef FF13_DIAG
    // Bracket the dispatch: the captured `this` may since have been freed, in which case this can
    // spin inside a corrupted table for ever. A hang ending on the "..." half happened in here.
    int logit = (g_isReadLogN < 24);
    if (logit) {
        g_isReadLogN++;
        ff13logv("[FF13-SRP] isRead probe: this=%08X \"%.15s\" ...\n", g_isReadThis, key);
    }
    void* r = ((isRead_t)(uintptr_t)(g_base + RVA_ISREAD))((void*)(uintptr_t)g_isReadThis, key, 0)
              ? (void*)1 : NULL;
    if (logit) ff13logv("[FF13-SRP] isRead probe: ... -> %d\n", r ? 1 : 0);
    return r;
#else
    return ((isRead_t)(uintptr_t)(g_base + RVA_ISREAD))((void*)(uintptr_t)g_isReadThis, key, 0)
           ? (void*)1 : NULL;
#endif
}

// Are all of this fight's charaspecs in the registry? -1 when the scene is not in the table.
int scene_resident(uint32_t btsc, const char** missing) {
    // Five times a second: this dispatches into the game, so it must not run per frame.
    static DWORD next = 0;
    static int last = -1;
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0) return last;
    next = now + 200;
    // An arena entry naming its own character set has said which charaspecs are really needed.
    // The scene table lists what a scene MIGHT field, including models no set carries and the
    // fight never wears, so judging on it meant waiting the full cap every time.
    const struct ff13_arena_s* ar = scene_arena(btsc);
    const ff13_charaset* cs = (ar && ar->chset) ? charaset_by_name(ar->chset) : NULL;
    if (cs && cs->n) {
        for (int i = 0; i < cs->n; i++) {
            const char* nm = kModelName[kCharasetIdx[cs->first + i]];
            if (!charaspec_resident(nm)) { if (missing) *missing = nm; last = 0; return 0; }
        }
        last = 1;
        return 1;
    }
    const ff13_scene_models* sm = scene_models(btsc);
    if (!sm || !sm->n) return -1;
    for (int i = 0; i < sm->n; i++) {
        const char* nm = kModelName[kModelIdx[sm->first + i]];
        if (!charaspec_resident(nm)) { if (missing) *missing = nm; last = 0; return 0; }
    }
    last = 1;
    return 1;
}

// ---- does the loaded character set hold this fight's enemies? -------------------------------
const ff13_scene_models* scene_models(uint32_t btsc) {
    int lo = 0, hi = FF13_SCENE_MODELS_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kSceneModels[mid].btsc == btsc) return &kSceneModels[mid];
        if (kSceneModels[mid].btsc < btsc) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
int charaset_covers(const ff13_charaset* cs, const ff13_scene_models* sm) {
    for (int i = 0; i < sm->n; i++) {
        uint16_t want = kModelIdx[sm->first + i];
        int found = 0;
        for (int j = 0; j < cs->n && !found; j++) if (kCharasetIdx[cs->first + j] == want) found = 1;
        if (!found) return 0;
    }
    return 1;
}
// A scene band is not a zone -- the 11xxx band alone spans nine of them -- so listing a whole band
// offered fights from elsewhere, which gave an empty arena, a broken fight or a crash.
int zone_has_charasets(uint32_t zone) {
    for (int i = 0; i < FF13_CHARASET_N; i++) if (kCharasets[i].zone == zone) return 1;
    return 0;
}
// A zone's fights are not all in its own band: the Steppe's mission fights are btsc14xxx while its
// ordinary ones are btsc11xxx, and the picker lists one band.
int band_listed_here(uint32_t zone, uint32_t band, uint32_t btsc) {
    if (btsc >= band && btsc < band + 1000) return 1;
    for (int i = 0; i < FF13_ZONE_EXTRA_N; i++)
        if (kZoneExtraBand[i].zone == zone
            && btsc >= kZoneExtraBand[i].band && btsc < (uint32_t)kZoneExtraBand[i].band + 1000)
            return 1;
    return 0;
}

// A mission names the character sets it loads, which is a finer answer than which zone it is in:
// the Steppe's twenty all name chset_z023_*, but they split between def1/def6/def8 and def2/def3
// (the plain against the ruined city). What the FIELD has loaded says which of those you are in.
// Only applied to fights that say something; a scene with no entry is judged as before.
static int mission_set_says_elsewhere(uint32_t btsc) {
    if (!g_curCharset[0]) return 0;                 // nothing to compare against yet
    int any = 0;
    for (int i = 0; i < FF13_MISSION_SET_N; i++) {
        if (kMissionSets[i].btsc != btsc) continue;
        any = 1;
        if (!strcmp(kMissionSets[i].set, g_curCharset)) return 0;
    }
    return any;
}

int scene_belongs_here(uint32_t zone, uint32_t btsc) {
    // Kept out by hand: a scene with no arena is placed by whether the zone holds its models,
    // which is a guess, and this is where a wrong one is corrected.
    for (int i = 0; i < FF13_BTSC_NOTHERE_N; i++)
        if (kBtscNotHere[i].zone == zone && kBtscNotHere[i].btsc == btsc) return 0;
    if (mission_set_says_elsewhere(btsc)) return 0;
    const ff13_scene_models* sm = scene_models(btsc);
    // The fight's own arena beats model residency: a mission enemy is fetched by hand now (see
    // load_loose_specs), so its fight passes the model test in every zone. Not every scene names
    // an arena, so the rest fall through to the residency test below.
    if (sm && sm->zone) return sm->zone == zone;
    if (!sm || !sm->n) return 1;                    // nothing to judge on -- leave it in
    for (int i = 0; i < FF13_CHARASET_N; i++)
        if (kCharasets[i].zone == zone && charaset_covers(&kCharasets[i], sm)) return 1;
    return 0;
}
const ff13_charaset* charaset_by_name(const char* name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < FF13_CHARASET_N; i++)
        if (strcmp(kCharasets[i].name, name) == 0) return &kCharasets[i];
    return NULL;
}
// The set to load before starting `btsc` here, or NULL for "leave it alone". NULL covers three
// situations on purpose: the loaded set already holds every model; no set in this zone holds them
// all (charaset.wdb does not list every model that can be resident, so a miss is NOT evidence the
// fight will come up short); or the scene is not in the table.
const ff13_charaset* charaset_needed(uint32_t zone, uint32_t btsc) {
    const ff13_scene_models* sm = scene_models(btsc);
    if (!sm || !sm->n) return NULL;
    // Ours first: g_curCharset tracks only what the GAME asked for, so without this a second
    // attempt at the same fight would ask for the set again and wait for it again.
    const ff13_charaset* cur = charaset_by_name(g_csLoaded[0] ? g_csLoaded : g_curCharset);
    if (cur && charaset_covers(cur, sm)) return NULL;
    // Most overlap with the loaded set first, smaller set on a tie. "Smallest covering set" alone
    // was the first rule and chose a set that made the fight come up empty: the most similar set
    // asks the streaming system for the least and stays closest to what the game has decided
    // belongs where you are standing.
    const ff13_charaset* best = NULL;
    int bestOverlap = -1;
    for (int i = 0; i < FF13_CHARASET_N; i++) {
        if (kCharasets[i].zone != zone) continue;
        if (!charaset_covers(&kCharasets[i], sm)) continue;
        int overlap = 0;
        if (cur) {
            for (int a = 0; a < kCharasets[i].n; a++)
                for (int b = 0; b < cur->n; b++)
                    if (kCharasetIdx[kCharasets[i].first + a] == kCharasetIdx[cur->first + b]) {
                        overlap++; break;
                    }
        }
        if (!best || overlap > bestOverlap
            || (overlap == bestOverlap && kCharasets[i].n < best->n)) {
            best = &kCharasets[i]; bestOverlap = overlap;
        }
    }
    if (best) return best;
    // Nothing in this zone can stage it. The game itself crosses zones for this, so a set from
    // elsewhere is worth trying rather than refusing outright. Only reached for a fight the picker
    // was told to start.
    for (int i = 0; i < FF13_CHARASET_N; i++) {
        if (!charaset_covers(&kCharasets[i], sm)) continue;
        if (!best || kCharasets[i].n < best->n) best = &kCharasets[i];
    }
    if (best)
        ff13logv("[FF13-SRP] battle picker: no character set in this zone stages this fight; "
                 "trying \"%s\" from zone %u\n", best->name, best->zone);
    return best;
}

void __cdecl on_load_sound(uint32_t vmctx) {
    if (!vmctx || IsBadReadPtr((void*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF), 4)) return;
    uint32_t args = *(uint32_t*)(uintptr_t)(vmctx + SB_ARGSTACK_OFF);
    if (!args || IsBadReadPtr((void*)(uintptr_t)args, 2 * SB_SLOT)) return;
    const char* a1 = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(args + 4);
    const char* a2 = (const char*)(uintptr_t)*(uint32_t*)(uintptr_t)(args + SB_SLOT + 4);
    int a1ok = (a1 && !IsBadReadPtr((void*)a1, 1));
    int a2ok = (a2 && !IsBadReadPtr((void*)a2, 1));
    int mine = g_inSndLoad;
    if (g_lsndLogN < 40) {
        g_lsndLogN++;
        ff13logv("[FF13-SRP] loadSoundResource(\"%.31s\", \"%.31s\")%s\n",
                 a1ok ? a1 : "(unreadable)", a2ok ? a2 : "(unreadable)",
                 mine ? "   <- requested by us" : "");
    }
    // Read-only. Do not load here: taking over a slot only works while the game happens to be
    // loading that same track, and it then applies to EVERY battle in the zone.
}

// ---- "is this scene actually loaded?" ------------------------------------------------------
// Asked the way the game asks it at 0xB509E0:
//     lea ecx,obj ; call 0xB41590   (ctor: clears the object, +0x20 = 0)
//     push flag ; push id ; call 0xB41640   (getBattleScene; callee-cleans -> __thiscall)
//     obj+0x20 != 0  <=>  the scene exists in the DataBattle DB right now
// MUST be called on the game's own thread inside its encounter build, so the DB is in the state
// the real lookup will see moments later.
typedef void* (__thiscall *btscCtor_t)(void* self);
typedef void  (__thiscall *btscGet_t )(void* self, uint32_t id, uint32_t flag);
#define RVA_BTSC_CTOR 0x741590       // VA 0xB41590
static volatile unsigned char g_inProbe = 0;   // suppresses our own getBattleScene log while probing

static int btsc_is_loaded(uint32_t id) {
    if (!g_base) return 0;
    uint8_t obj[0x60];
    memset(obj, 0, sizeof obj);
    g_inProbe = 1;
    ((btscCtor_t)(uintptr_t)(g_base + RVA_BTSC_CTOR))(obj);
    ((btscGet_t )(uintptr_t)(g_base + RVA_GETBTSC  ))(obj, id, 1);
    g_inProbe = 0;
    return *(uint32_t*)(obj + 0x20) != 0;
}

// handler(ecx, [esp+0]=return address, &[esp+4]=&id, &[esp+8]=&flag) via ff13_install_argptr_hook.
// The hook passes the ADDRESS of the caller's stack argument (and popad restores registers, not the
// stack), so writing through pId substitutes the scene the game is about to fetch.
// Scoped by a short window opened when an encounter starts building, so lookups outside an
// encounter (menus, loading) are never touched.
// This rewrites the scene the game FETCHES, not the scene NUMBER it remembers: two places store the
// caller's own argument, so both keep the original number:
//     0xB53CE9   mov [encounterMgr + 0x358], arg   (encounterMgr = [0x282B190])
//     0x4D500D   mov [battleMgr    + 0x390], arg   and from it "btl/scene/btsc%05d" at 0x509BB0
// on_battle_entry below fixes that at the source; this hook is the backstop for lookups that do
// not come through it.
void __cdecl on_get_btsc(uint32_t ecx, uint32_t retAddr, uint32_t* pId, uint32_t* pFlag) {
    (void)ecx;
    if (g_inProbe) return;                     // our own probe, not the game asking
    uint32_t orig = pId ? *pId : 0u;
    int rewrote = 0;
    if (pId && g_swapBtscId && (int)(g_swapWindowUntil - GetTickCount()) > 0
        && *pId != g_swapBtscId) {
        *pId = g_swapBtscId;
        rewrote = 1;
    }
    if (g_gbsLogN < 120) {
        g_gbsLogN++;
        ff13logv("[FF13-SRP] getBattleScene id=%u%s flag=%u  <- caller 0x%08X\n",
                 orig, rewrote ? " -> REWRITTEN" : "", pFlag ? *pFlag : 0u, retAddr);
    }
}

// The one door every battle goes through: startBattle(btsc, flags) at 0x4BB5B0. Both ways in reach
// it -- the field encounter (0x8126F0, the TRG_ENCOUNT handler) and the VM native startBattle_l
// (0xBE3EDC) -- and everything downstream takes its scene number from this ONE argument:
//     0x4D57A0(btsc,..)  is-it-loaded (0xB29CC0), then
//        0x4BBE60(btsc,..)  -> 0xB53390 -> 0xB53CA0   builds the enemy/charaspec list, stores
//                                                     [encounterMgr+0x358] = btsc
//                           -> 0x4D49E0              stores [battleMgr+0x390] = btsc and loads
//                                                     the scene resource "btl/scene/btsc%05d"
// Substituting HERE makes all of them agree. Same one-shot window as the hook above, so a story
// battle starting on its own is never touched.
static volatile int g_btlEntryLogN = 0;
// What the game reported about the fight it is building. Written by the message hook, read (and
// filed) by bs_pump.
#define BTL_LOG_MS 30000
#ifdef FF13_DIAG
static volatile int g_traceN = 0;      // trace lines used by THIS fight
#endif
volatile uint32_t g_btlScene = 0;
volatile int      g_btlCreated = 0, g_btlFailed = 0, g_btlNotLoaded = 0;
volatile DWORD    g_btlStatAt = 0, g_btlLogUntil = 0;
volatile DWORD    g_bgmArmedAt = 0;    // when the picker asked for a track
volatile DWORD    g_btlLastMsgAt = 0;   // when the game last said something about this fight
volatile int      g_btlShort = 0;       // THIS fight fielded fewer than it built
// Set when THIS fight had an enemy taken out of it by instant-win. What such a fight fielded is
// not evidence about the zone, so the picker must not learn from it -- the record persists.
volatile int      g_iwHitThisFight = 0;
char g_btlBad[24];                  // the first battle chara that failed to activate
// Stamped when a picked fight ends; cleared by any battle that is not ours. Everything the GAME
// OVER screen does differently hangs off this.
volatile DWORD g_pickedEndAt = 0;

void __cdecl on_battle_entry(uint32_t ecx, uint32_t retAddr, uint32_t* pId, uint32_t* pFlag) {
    (void)ecx;
    if (!pId) return;
    uint32_t bflags = (pFlag && !IsBadReadPtr(pFlag, 4)) ? *pFlag : 0xFFFFFFFFu;
    uint32_t orig = *pId;
    int rewrote = 0;
    if (g_swapBtscId && (int)(g_swapWindowUntil - GetTickCount()) > 0 && orig != g_swapBtscId) {
        *pId = g_swapBtscId;
        rewrote = 1;
    } else if (g_swapBtscId && orig != g_swapBtscId && g_swapWindowUntil
               && (DWORD)(GetTickCount() - g_swapWindowUntil) < 60000) {
        // The window closed before the battle it was opened for started, so the fight builds with
        // the original scene -- which on the picker's path is -1, i.e. no fight at all. Said out
        // loud, because a fight that quietly does not happen is the worst outcome here.
        ff13logv("[FF13-SRP] battle picker: the swap window closed %lums before this battle started "
                 "-- btsc%05u was NOT substituted and nothing will build\n",
                 (unsigned long)(GetTickCount() - g_swapWindowUntil), g_swapBtscId);
        flash_say(ui_text(T_PICK_LOST_WINDOW));
    }
    // Every battle, picked or walked into, opens the window in which the game's own messages say
    // how many enemies it built and which ones failed.
    g_btlScene   = *pId;
    g_btlCreated = 0;
    g_btlFailed  = 0;
    g_btlNotLoaded = 0;
    g_btlBad[0]  = 0;
    g_iwHitThisFight = 0;          // per fight, like everything else the message hook counts
    g_btlStatAt  = GetTickCount();
    g_btlLogUntil = g_btlStatAt + BTL_LOG_MS;
    // A battle that is not ours ends any claim on the next game over: otherwise an ordinary fight
    // walked into straight afterwards would get the practice shortcuts.
    if (!g_bsStarted) g_pickedEndAt = 0;
#ifdef FF13_DIAG
    g_traceN = 0;                 // the trace budget is per fight, not per session
    arena_probe_formation();      // where the three of them stand as this fight begins
#endif
    if (g_btlEntryLogN < 60) {
        g_btlEntryLogN++;
        ff13logv("[FF13-SRP] startBattle btsc%05u flags=0x%X%s  <- caller 0x%08X\n",
                 orig, bflags, rewrote ? " -> SUBSTITUTED at the source" : "", retAddr);
    }
}

// ---- the game telling us what went wrong, in its own words ----------------------------------
// FF13 ships with its development warning printer intact: 0x8A5B20, 1066 call sites, each passing
// the source file and line it was written on.
// Signature (cdecl, from the call sites and from the callee reading [ebp+0x14] as the category and
// [ebp+0x1C] as its va_list): log(int, const char* file, int line, int category, const char* fmt, ...)
// At the entry, before anything is pushed:
//     [esp+4] arg0  [esp+8] file  [esp+0xC] line  [esp+0x10] category  [esp+0x14] fmt  [esp+0x18] va
// so taking the ADDRESS of `file` hands us the whole argument block as one array.
// Two things are deliberate: the messages are formatted HERE by a bounds-checking formatter, since
// this runs inside 1066 call sites we do not control and a %s into freed memory must print "(bad)"
// rather than take the game down; and the text is cp932, converted to UTF-8 on the way out.
const uint8_t P_GLOG[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x18};   // push ebp;mov ebp,esp;sub esp,0x18
static volatile int g_glogN = 0;
#define GLOG_MAX 400                       // a fight's worth; a runaway site cannot fill the disk

// %s / %d / %i / %u / %x / %X / %c / %f / %g / %%. Anything else is copied through and one
// argument is skipped, which keeps the rest of the line aligned instead of garbling it.
static void glog_format(char* out, size_t outN, const char* fmt, const uint32_t* va) {
    size_t o = 0;
    if (outN == 0) return;
    if (IsBadStringPtrA(fmt, 512)) { strncpy(out, "(bad format)", outN - 1); out[outN-1] = 0; return; }
    for (const char* p = fmt; *p && o + 1 < outN; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        p++;
        if (*p == '%') { out[o++] = '%'; continue; }
        while (*p && !strchr("diuxXcsfgeEGp", *p)) p++;   // skip flags/width/precision
        if (!*p) break;
        char tmp[64];
        tmp[0] = 0;
        if (*p == 's') {
            const char* s = (const char*)(uintptr_t)(*va++);
            if (!s || IsBadStringPtrA(s, 256)) strncpy(tmp, "(bad)", sizeof tmp - 1);
            else { strncpy(tmp, s, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0; }
        } else if (*p == 'c') {
            tmp[0] = (char)(*va++); tmp[1] = 0;
        } else if (*p == 'f' || *p == 'g' || *p == 'e' || *p == 'E' || *p == 'G') {
            double d;                                     // a float arg is promoted to 8 bytes
            memcpy(&d, va, sizeof d); va += 2;
            snprintf(tmp, sizeof tmp, "%g", d);
        } else if (*p == 'u') {
            snprintf(tmp, sizeof tmp, "%u", (unsigned)*va++);
        } else if (*p == 'x' || *p == 'X' || *p == 'p') {
            snprintf(tmp, sizeof tmp, "0x%X", (unsigned)*va++);
        } else {
            snprintf(tmp, sizeof tmp, "%d", (int)*va++);
        }
        size_t n = strlen(tmp);
        if (o + n >= outN) n = outN - 1 - o;
        memcpy(out + o, tmp, n); o += n;
    }
    out[o] = 0;
}

// cp932 in, UTF-8 out. On any failure the original is kept: a mangled line still beats no line.
static void glog_to_utf8(const char* in, char* out, int outN) {
    wchar_t wide[600];
    int w = MultiByteToWideChar(932, 0, in, -1, wide, (int)(sizeof wide / sizeof wide[0]));
    if (w <= 0 || WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outN, NULL, NULL) <= 0) {
        strncpy(out, in, (size_t)outN - 1);
        out[outN - 1] = 0;
    }
}

// The lines that say how a fight was populated. Matched on FILE:LINE, NOT on the text: the messages
// are cp932 Japanese while the source location is ASCII and stable.
//   BattleChara.cpp:736   one per enemy the fight intends to field
//   BattleMain.cpp:3234   "activate failed" -- that enemy's model is not resident
// created minus failed is what actually turns up, story fights included.
#define GLOG_F_CREATED  736
#define GLOG_F_FAILED  3234
// A third line, which can turn up ALONE: the activate-failed line is not always printed (btsc09055
// produced five of these and no 3234), so watching only for 3234 counted that fight as complete.
#define GLOG_F_NOTLOADED 916

void __cdecl on_game_log(uint32_t ecx, uint32_t fmtVal, uint32_t* pArgs, uint32_t* unused) {
    (void)ecx; (void)unused;
    if (!pArgs || IsBadReadPtr(pArgs, 5 * 4)) return;
    const char* file = (const char*)(uintptr_t)pArgs[0];
    int line = (int)pArgs[1];
    const char* base = "?";
    if (file && !IsBadStringPtrA(file, 260)) {
        base = file;
        for (const char* q = file; *q; q++) if (*q == '\\' || *q == '/') base = q + 1;
    }
    // Counting happens whatever the log level says -- the picker's tally is built from it. Only
    // the WRITING is a verbosity question.
    DWORD now = GetTickCount();
    int inBattle = g_btlScene && (int)(g_btlLogUntil - now) > 0;
    if (inBattle) {
        if (line == GLOG_F_CREATED && strcmp(base, "BattleChara.cpp") == 0)
            { g_btlCreated++; g_btlLastMsgAt = now; }
        else if (line == GLOG_F_NOTLOADED && strcmp(base, "SceneActorActionPlayer.cpp") == 0) {
            g_btlNotLoaded++;
            g_btlLastMsgAt = now;
            const char* who = (const char*)(uintptr_t)pArgs[4];
            if (!g_btlBad[0] && who && !IsBadStringPtrA(who, sizeof g_btlBad)) {
                strncpy(g_btlBad, who, sizeof(g_btlBad) - 1);
                g_btlBad[sizeof(g_btlBad) - 1] = 0;
            }
        }
        else if (line == GLOG_F_FAILED && strcmp(base, "BattleMain.cpp") == 0) {
            g_btlFailed++;
            g_btlLastMsgAt = now;
            const char* who = (const char*)(uintptr_t)pArgs[4];   // the charaspec that failed
            if (!g_btlBad[0] && who && !IsBadStringPtrA(who, sizeof g_btlBad)) {
                strncpy(g_btlBad, who, sizeof(g_btlBad) - 1);
                g_btlBad[sizeof(g_btlBad) - 1] = 0;
            }
        }
    }
    // Written only at [log] level = verbose and only during a battle, since the printer fires from
    // all over the engine. "During a battle" means the whole battle, not the counter's window: that
    // window shuts 1.5s after the last build message, which is right for the tally and wrong for
    // anything the fight says later (a mid-fight summon's account arrives long after it closed).
    if (g_logLevel < 2) return;
    if (!inBattle && !overlay_in_battle()) return;
    if (g_glogN >= GLOG_MAX) return;
    g_glogN++;
    char msg[600], utf8[1200];
    glog_format(msg, sizeof msg, (const char*)(uintptr_t)fmtVal, pArgs + 4);
    glog_to_utf8(msg, utf8, (int)sizeof utf8);
    ff13logv("[FF13-SRP] GAME: %s  (%.40s:%d)\n", utf8, base, line);
    if (g_glogN == GLOG_MAX)
        ff13logv("[FF13-SRP] GAME: (%d messages -- capped, later ones dropped)\n", GLOG_MAX);
}

#ifdef FF13_DIAG
// The game's OTHER printer, 0x8A5A60, is a plain trace printf(fmt, ...) with 442 call sites,
// compiled down to `xor eax,eax; ret` in the retail build. Hooking the stub gets the lines back;
// the battle camera narrates itself through it (0x4D6680 / 0x4D7140 / 0x4D7300), which is the one
// thing the object fields cannot say: WHY the camera decided what it decided.
const uint8_t P_TRACE[5] = {0x55,0x8B,0xEC,0x33,0xC0};   // push ebp;mov ebp,esp;xor eax,eax
#define TRACE_MAX 500

void __cdecl on_trace(uint32_t ecx, uint32_t fmtVal, uint32_t* pArgs, uint32_t* unused) {
    (void)ecx; (void)unused;
    const char* fmt = (const char*)(uintptr_t)fmtVal;
    if (!fmt || IsBadStringPtrA(fmt, 200)) return;
    // Only the first seconds of a fight, the window the party is moved onto the stage in, so an
    // ordinary encounter and an arena-started one can be read side by side.
    if ((int)(GetTickCount() - (g_btlStatAt + 4000)) >= 0) return;
    if (g_traceN >= TRACE_MAX || !pArgs || IsBadReadPtr(pArgs, 16)) return;
    g_traceN++;
    char msg[400];
    glog_format(msg, sizeof msg, fmt, pArgs);
    for (char* p = msg; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    ff13logv("[FF13-SRP] TRACE: %s\n", msg);
}
#endif // FF13_DIAG

// ---- which camera sequence a fight gets (READ-ONLY) ------------------------------------------
// bt_scene.wdb cell 6 is a fight's battle-intro camera (op_btboss_*), and 31 of the 46 boss and
// event fights leave it empty. 0x595620 is where the name is written into the camera object
// ([this+0x260], a 16-byte inline string); arg1 points at the name being set.
const uint8_t P_CAMSEQ[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x7C,0x02,0x00,0x00};
static volatile int g_camLogN = 0;

void __cdecl on_cam_seq(uint32_t pName) {
    if (g_camLogN >= 40 || !pName || IsBadReadPtr((void*)(uintptr_t)pName, 16)) return;
    char n[17];
    memcpy(n, (const void*)(uintptr_t)pName, 16);
    n[16] = 0;
    g_camLogN++;
    ff13logv("[FF13-SRP] battle camera sequence: \"%.16s\"  (btsc%05u, state %d)\n",
             n[0] ? n : "(empty)", g_btlScene, bm_state());
}

// ---- and letting the fights that carry none have one ------------------------------------------
// Do NOT try to drive the sequence from outside: setSequence/requestCamArts were all tried and all
// failed (see the notes). The gate is one bit of the scene's own record -- state 4 asks 0xB421A0,
// which ends in `sete` and returns the NEGATION of bit 8, so that bit means SUPPRESS the intro.
// Clearing it for the length of the fight puts every step back in the game's hands.
// Not done everywhere: most scenes that name no camera are ordinary encounters and look right
// without one, so only the scenes listed below, each of which has a donor sequence.
const uint8_t P_CAMREQ[9] = {0x55,0x8B,0xEC,0x81,0xEC,0xB4,0x00,0x00,0x00};
// The request struct as the game builds it at 0x4FE5FE: 16-byte sequence name at +0, priority at
// +0x20. Only the name is touched, on the caller's own stack.
#define OFF_BM_ARTSCAM 0x368      // [BattleMain+0x368] -> the btl-artscam object
#define RVA_ARTSCAM_VT 0xCAB900   // VA 0x10AB900: its vtable, so the pointer can be proven
#define OFF_CAM_SEQ    0x270      // artscam +0x10 (sequence player) +0x260 = its 16-byte name
#define OFF_CAM_FLAGS  0x338      // artscam's own flag word; bit 12 gates its per-frame update

volatile int g_camSetOk = 0;              // the request hook is in place

// Scene -> the sequence to hand it, with the fight the name is taken from. EMPTY, and it stays
// that way unless a fight is reported that the GAME frames badly: the story flow was captured and
// btsc12090 gets no intro camera there either. Example rows, if one is ever needed:
//   { 11709, "op_btboss_sup2" },  // Barthandelus on Gran Pulse  <- btsc09055, the same boss
//   { 12090, "op_btboss_sup2" },  // Barthandelus, the last one  <- btsc09055, the same boss
static const struct { uint16_t btsc; const char* seq; } kCamSupply[] = {
    { 0, NULL },
};

// Where the battle state machine is (0 = not in a battle). Its 40 states are the jump table at
// 0x502C8C, driven from 0x4FDC60; state 4 waits on the camera sequence.
int bm_state(void) {
    if (!g_base) return 0;
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_STATE + 4)) return 0;
    return *(int*)(bm + OFF_BM_STATE);
}

// The artscam object, or NULL. Proven by its vtable rather than assumed: it is built per battle.
static void* artscam(void) {
    if (!g_base) return NULL;
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_ARTSCAM + 4)) return NULL;
    void* cam = *(void**)(bm + OFF_BM_ARTSCAM);
    if (!cam || IsBadReadPtr(cam, OFF_CAM_SEQ + 16)) return NULL;
    if (*(uint32_t*)cam != (uint32_t)(g_base + RVA_ARTSCAM_VT)) return NULL;
    return cam;
}

// The accessor state 4 asks (0xB421A0) reads one bit out of the scene's own record:
//     R   = [BattleMain+0x398 +0x20]            (the scene wrapper the battle init filled in)
//     row = [R+4] ? [R+4] : [R]                 (two record shapes, hence two offsets)
//     answer = (row[+0x44 or +0x40] >> 8) & 1
// The record is the LOADED DATABASE and is shared with every other fight in the band, so the bit
// must be put back when the fight ends. With it cleared the game asks for its generic
// "default_op%02d", which on_cam_request swaps for the table's name.
#define OFF_BM_SCENEID 0x390      // [BattleMain+0x390] = the scene number this fight was built from
#define OFF_BM_SCENE   0x398      // [BattleMain+0x398] = the scene wrapper itself (not a pointer)
#define OFF_SCENE_REC  0x20       // wrapper +0x20 -> the record pair

volatile DWORD g_camWatch = 0;
volatile int   g_camDone  = 1;
static char      g_camWant[16];           // the sequence to swap into the game's request

// Put the record back the way it was. Called when the fight ends, and before arming another.
#define CAM_NOINTRO 0x100                 // bit 8 of the record flag word: "no intro camera"
static uint32_t* g_camBit = NULL;         // the record word whose suppress bit was cleared

void cam_disarm(void) {
    if (g_camBit && !IsBadWritePtr(g_camBit, 4)) *g_camBit |= (uint32_t)CAM_NOINTRO;
    g_camBit = NULL;
    g_camWant[0] = 0;
}

// CLEAR the scene's bit 8, do not set it: the bit means SUPPRESS the intro, so setting it (the
// first attempt) was exactly backwards.
//     btsc12092  bit 8 = 0 -> accessor returns 1 -> intro staged, held, played
//     btsc12090  bit 8 = 1 -> accessor returns 0 -> the whole block is jumped over
// With it cleared the game reads the scene's own camera name (0xB42100), falls back to scanning
// default_op00..62, builds the request, holds the battle while the cuts play and resets after.
// The bit goes back when the fight ends, and again before the next is armed in case that was
// skipped by a crash or a cancelled fight.
static int cam_arm(uint32_t btsc, const char* want) {
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_SCENE + OFF_SCENE_REC + 4)) return 0;
    if (*(uint32_t*)(bm + OFF_BM_SCENEID) != btsc) return 0;      // not this fight's scene yet
    char* rec = *(char**)(bm + OFF_BM_SCENE + OFF_SCENE_REC);
    if (!rec || IsBadReadPtr(rec, 8)) return 0;
    char* row = *(char**)(rec + 4);
    int off = 0x44;
    if (!row) { row = *(char**)rec; off = 0x40; }
    if (!row || IsBadWritePtr(row + off, 4)) return 0;
    strncpy(g_camWant, want, sizeof g_camWant - 1);
    g_camWant[sizeof g_camWant - 1] = 0;
    uint32_t* f = (uint32_t*)(row + off);
    if (!(*f & CAM_NOINTRO)) {                    // already allowed -- nothing of ours to undo
        ff13logv("[FF13-SRP] battle picker: btsc%05u already allows a battle-intro camera\n", btsc);
        return 1;
    }
    *f &= ~(uint32_t)CAM_NOINTRO;
    g_camBit = f;
    ff13logv("[FF13-SRP] battle picker: btsc%05u -- intro camera un-suppressed, asking for \"%s\"\n",
             btsc, g_camWant);
    return 1;
}

// Every camera request the battle makes, with its caller. While a fight in the table is running,
// the generic default_opNN is swapped for the one the table names.
static volatile int g_camReqN = 0;

void __cdecl on_cam_request(uint32_t ecx, uint32_t retAddr, uint32_t* pReqSlot,
                                   uint32_t* unused) {
    (void)ecx; (void)unused;
    if (!pReqSlot || IsBadReadPtr(pReqSlot, 4)) return;
    char* nm = (char*)(uintptr_t)*pReqSlot;
    if (!nm || IsBadReadPtr(nm, 16)) return;
    char shown[17];
    memcpy(shown, nm, 16); shown[16] = 0;
    int swapped = 0;
    if (g_camWant[0] && strncmp(nm, "default_op", 10) == 0 && !IsBadWritePtr(nm, 16)) {
        memset(nm, 0, 16);
        strncpy(nm, g_camWant, 15);
        swapped = 1;
    }
    if (g_camReqN < 40) {
        g_camReqN++;
        ff13logv("[FF13-SRP] camera request \"%.16s\"%s  <- caller 0x%08X\n", shown,
                 swapped ? " -> swapped" : "", (unsigned)retAddr);
    }
}

#ifdef FF13_DIAG
// Armed by bs_pump a few seconds into a picked fight; read by pos_snapshot on the commit hook.
volatile DWORD g_posSnap = 0;      // log every actor while this tick has not passed
volatile DWORD g_camSnapAt = 0;    // when to take the snapshot
volatile int   g_posN = 0;
#endif

#ifdef FF13_DIAG
// Read from the outside, the first gate answers the opposite of the outcome, so the wrapper read
// there is not the one the game reads. This hooks the accessor and takes the pointer it is handed.
const uint8_t P_HASCAM[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x10};
static volatile int g_hasCamN = 0;

void __cdecl on_has_cam(uint32_t ecx) {
    if (g_hasCamN >= 12 || !ecx || IsBadReadPtr((void*)(uintptr_t)(ecx + 0x20), 4)) return;
    char* bm = g_base ? *(char**)(g_base + RVA_BM_PTR) : NULL;
    char* rec = *(char**)(uintptr_t)(ecx + 0x20);
    unsigned a = 0, b = 0, v40 = 0, v44 = 0;
    if (rec && !IsBadReadPtr(rec, 8)) {
        a = *(uint32_t*)rec; b = *(uint32_t*)(rec + 4);
        char* row = b ? (char*)(uintptr_t)b : (char*)(uintptr_t)a;
        if (row && !IsBadReadPtr(row + 0x40, 8)) {
            v40 = *(uint32_t*)(row + 0x40); v44 = *(uint32_t*)(row + 0x44);
        }
    }
    g_hasCamN++;
    ff13logv("[FF13-SRP] hasIntroCamera: wrapper=%08X (BM+0x398=%08X) rec=%08X [R]=%08X [R+4]=%08X "
             "+0x40=%08X +0x44=%08X -> %u\n", (unsigned)ecx,
             (unsigned)(uintptr_t)(bm ? bm + OFF_BM_SCENE : NULL), (unsigned)(uintptr_t)rec, a, b,
             v40, v44, ((b ? v44 : v40) >> 8) & 1);
}

// The two gates in front of the intro-camera block (state 4, 0x4FE3FA and 0x4FE407), read exactly
// as the game reads them, once per fight:
//   0xB421A0  R = [wrapper+0x20]; row = [R+4] ? [R+4] : [R]; (row[+0x44 or +0x40] >> 8) & 1
//   0x4FE407  bit 0x4A of [BattleMain+0x38] -- set during battle init at 0x4D4CF0; SET means skip
volatile int g_gatesDone = 0;

static void cam_gates(uint32_t btsc) {
    if (g_gatesDone || bm_state() != 4) return;
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_SCENE + OFF_SCENE_REC + 4)) return;
    g_gatesDone = 1;
    char* rec = *(char**)(bm + OFF_BM_SCENE + OFF_SCENE_REC);
    char* row = NULL; int off = 0; unsigned v44 = 0, v40 = 0, intro = 0;
    if (rec && !IsBadReadPtr(rec, 8)) {
        row = *(char**)(rec + 4); off = 0x44;
        if (!row) { row = *(char**)rec; off = 0x40; }
        if (row && !IsBadReadPtr(row + 0x40, 8)) {
            v40 = *(uint32_t*)(row + 0x40);
            v44 = *(uint32_t*)(row + 0x44);
            intro = ((off == 0x44 ? v44 : v40) >> 8) & 1;
        }
    }
    unsigned f38 = 0, f40 = 0, f44 = 0;
    if (!IsBadReadPtr(bm + 0x38, 12)) {
        f38 = *(uint32_t*)(bm + 0x38); f40 = *(uint32_t*)(bm + 0x3C); f44 = *(uint32_t*)(bm + 0x40);
    }
    ff13logv("[FF13-SRP] gates btsc%05u: introCam=%u (row=%p read +0x%02X, +0x40=%08X +0x44=%08X), "
             "bit74=%u  BM+0x38=%08X %08X %08X\n",
             btsc, intro, (void*)row, off, v40, v44, (f44 >> 10) & 1, f38, f40, f44);
}
#else
#define cam_gates(b) ((void)0)
#endif

// Run every frame of a picked fight from bs_pump (the game thread) until it has an answer.
void cam_pump(uint32_t btsc, DWORD now) {
    cam_gates(btsc);
    if (g_camDone) return;
    const char* want = NULL;
    for (int i = 0; i < (int)(sizeof kCamSupply / sizeof kCamSupply[0]); i++)
        if (kCamSupply[i].btsc == btsc) { want = kCamSupply[i].seq; break; }
    if (!want) {
        // Not a fault by itself: ordinary encounters have no intro camera either.
        if ((int)(now - g_camWatch) >= 0) {
            g_camDone = 1;
            ff13logv("[FF13-SRP] battle picker: btsc%05u names no battle-intro camera\n", btsc);
        }
        return;
    }
    if (!g_camSetOk) { g_camDone = 1; return; }
    if (cam_arm(btsc, want)) g_camDone = 1;
    else if ((int)(now - g_camWatch) >= 0) {
        g_camDone = 1;
        ff13logv("[FF13-SRP] battle picker: btsc%05u -- the scene record never turned up\n", btsc);
    }
}

// Follows the battle state machine ([BattleMain+0x44], the word the overlay reads) through a picked
// fight, printing the camera name at every step. Picker fights only, and capped.
#ifdef FF13_DIAG
// What state 4 waits on is NOT the name: 0x5966E0 asks for bit 0 of [seq+0x2F0] and [seq+0x2C0] >= 0
// (the current cut). 0x595620 sets both only if it reaches its tail -- it can bail out earlier, and
// the replace table it consults can answer "<ignore>". So sample those two alongside the name.
#define OFF_SEQ_CUT   0x2C0       // from the sequence player (artscam+0x10)
#define OFF_SEQ_FLAGS 0x2F0
static volatile int   g_bmSt = 0;
static volatile int   g_bmLines = 0;
static volatile DWORD g_bmT0 = 0;
static char           g_bmLast[64];

void bm_timeline(DWORD now) {
    int st = bm_state();
    if (!st) { g_bmSt = 0; g_bmLast[0] = 0; return; }
    if (!g_bmSt) { g_bmLines = 0; g_bmT0 = now; }
    char line[64];
    void* cam = artscam();
    if (cam) {
        const char* seq = (const char*)cam + 0x10;
        const char* nm  = (const char*)cam + OFF_CAM_SEQ;
        snprintf(line, sizeof line, "%d|%.16s|%d|%08X|%08X", st, nm[0] ? nm : "-",
                 *(int*)(seq + OFF_SEQ_CUT), *(uint32_t*)(seq + OFF_SEQ_FLAGS),
                 *(uint32_t*)((const char*)cam + OFF_CAM_FLAGS));
    } else {
        snprintf(line, sizeof line, "%d|(no camera object)", st);
    }
    g_bmSt = st;
    if (!strcmp(line, g_bmLast) || g_bmLines >= 60) return;
    strncpy(g_bmLast, line, sizeof g_bmLast - 1);
    g_bmLast[sizeof g_bmLast - 1] = 0;
    g_bmLines++;
    ff13logv("[FF13-SRP] +%ums state=%s\n", (unsigned)(now - g_bmT0), line);
}
#else
#endif

// doEncountCheck entry: capture the field manager so the picker can post a pending touch to it.
// The one-off log answers whether the global at 0x27FF124 is the same manager -- if so the picker
// needs no real encounter to find it, and with the object array at mgr+0x10 (564 entries of 0x90)
// and the in-use bitmap at mgr+0x13D50 (both from 0x5B3840) it needs none to find a touch id either.
static volatile int g_mgrCmpLogged = 0;

void __cdecl on_encount_check(uint32_t ecx) {
    if (ecx && !IsBadReadPtr((void*)(uintptr_t)(ecx + OFF_TOUCHED_ID), 4)) g_fieldMgr = ecx;
    if (!g_mgrCmpLogged && g_base) {
        g_mgrCmpLogged = 1;
        uintptr_t p = g_base + RVA_FIELDMGR_PTR;
        uint32_t cand = IsBadReadPtr((void*)p, 4) ? 0 : *(uint32_t*)p;
        ff13logv("[FF13-SRP] field manager: doEncountCheck ecx=%08X, [0x27FF124]=%08X -> %s\n",
                 ecx, cand, (cand == ecx) ? "SAME (can be read statically)" : "different");
    }
}

volatile uint32_t g_encIdSeq = 0;     // bumped per encounter build; see touch_stall_probe
volatile DWORD    g_touchAt = 0;      // ...and when the picker last asked for one
volatile uint32_t g_touchSeq = 0;

void __cdecl on_encount_id(uint32_t id) {
    g_lastTouchedId = id;                    // the battle picker replays this id to start a fight
    g_encIdSeq++;
    if (g_encIdLogN < 60) {
        g_encIdLogN++;
        // Reset the scene-lookup budget: this call starts an encounter build, and without it the
        // cap is spent on startup/loading lookups long before the first fight.
        g_gbsLogN = 0;
        ff13logv("[FF13-SRP] --- encounter setup for touched object id=%u (0x%X) ---\n", id, id);
    }
}

// ---- battle BGM: the music does NOT follow the battle scene ---------------------------------
// The music is chosen on a separate path (playBgm / getDefaultNameOfBattleBGM drive BgmManager,
// which knows nothing about btsc).
// Hook site: the play entry 0xACED90 (ecx = manager, arg2 = the resource NAME as a 16-byte inline
// buffer, arg3 = a float), which validates the name with isBgmResource (0xAE3750) and then drives
// playback through a vtable call. NOT its neighbour BgmManager::onPlay_ (0xACE7F0), which is the
// after-the-fact notification: hooking that logged "SWAPPED" while the music did not change.
// Hooked with argptr so we get the ADDRESS of the name argument and can point it at our own 16-byte
// buffer rather than rewrite a string the caller may still use.
const uint8_t P_BGM[9] = {0x55,0x8B,0xEC,0x81,0xEC,0xDC,0x00,0x00,0x00};

// ---- the OTHER play path: BgmManager::lazyPlay ----------------------------------------------
// 0xACED90 is the IMMEDIATE play, and a name whose resource is not in memory produces silence.
//     BgmManager::lazyPlay(const char name[16], float fade)   0xACF080, __thiscall, ret 8
// hands the name to the sound resource system under the group "general" (0xAFBF10) and plays it
// once it arrives, so it works for ANY music_* track in any area with no preload and no slot to
// steal. It returns 0 without doing anything when the name is not a BGM resource or is already
// playing, so it doubles as a name check. The manager is a lazy singleton behind 0xACE680.
// Starting ours does NOT stop the game's own battle theme (hardware played both at once): the lazy
// request goes through a different object ([mgr+0x70]), so the ordinary track must be stopped BY
// NAME through BgmManager::stop (0xACF220, __thiscall, ret 8).
#define RVA_BGM_INSTANCE 0x6CE680    // VA 0xACE680  BgmManager::instance()  (__cdecl, no args)
// lazyPlay RVA is RVA_BGMLAZY (srp_internal.h) -- __thiscall lazyPlay(name[16], float fade), ret 8
#define RVA_BGM_STOP     0x6CF220    // VA 0xACF220  __thiscall stop(name[16], float fade)
#define RVA_BGM_CURRENT  0x6CEBC0    // VA 0xACEBC0  __thiscall getPlayingName(out[16]) -> name
typedef void*       (__cdecl   *bgmInst_t)(void);
typedef char        (__thiscall *bgmLazy_t)(void* self, const char* name, float fade);
typedef char        (__thiscall *bgmStop_t)(void* self, const char* name, float fade);
typedef const char* (__thiscall *bgmCur_t )(void* self, char* out16);

// The request is handed on as a POINTER, so the name must outlive the call. Game thread only.
static char g_bgmLazyBuf[BGM_NAME_MAX + 1];
static char g_bgmStopBuf[BGM_NAME_MAX + 1];

// What the manager reports as the track it is playing. Game thread only.
int bgm_current(char* out /* BGM_NAME_MAX+1 */) {
    out[0] = 0;
    if (!g_base) return 0;
    void* mgr = ((bgmInst_t)(uintptr_t)(g_base + RVA_BGM_INSTANCE))();
    if (!mgr || IsBadReadPtr(mgr, 4)) return 0;
    char buf[BGM_NAME_MAX + 1];
    memset(buf, 0, sizeof buf);
    const char* now = ((bgmCur_t)(uintptr_t)(g_base + RVA_BGM_CURRENT))(mgr, buf);
    if (!now || IsBadReadPtr((void*)now, BGM_NAME_MAX)) return 0;
    memcpy(out, now, BGM_NAME_MAX);
    out[BGM_NAME_MAX] = 0;
    return 1;
}

int bgm_lazy_play(const char* name) {
    if (!g_base || !name || !name[0]) return 0;
    void* mgr = ((bgmInst_t)(uintptr_t)(g_base + RVA_BGM_INSTANCE))();
    if (!mgr || IsBadReadPtr(mgr, 4)) return 0;
    memset(g_bgmLazyBuf, 0, sizeof g_bgmLazyBuf);
    strncpy(g_bgmLazyBuf, name, BGM_NAME_MAX);
    // Asking for the track that is already playing makes lazyPlay log an assert and refuse.
    char cur[BGM_NAME_MAX + 1];
    if (bgm_current(cur) && strncmp(cur, g_bgmLazyBuf, BGM_NAME_MAX) == 0) return 1;
    return ((bgmLazy_t)(uintptr_t)(g_base + RVA_BGMLAZY))(mgr, g_bgmLazyBuf, 0.0f) ? 1 : 0;
}

// No return value: 0xACF220 ends in a plain `ret 8` with eax left over from its last call. It stops
// BY NAME (an empty name means "whatever is playing").
void bgm_stop(const char* name, float fade) {
    if (!g_base || !name || !name[0]) return;
    void* mgr = ((bgmInst_t)(uintptr_t)(g_base + RVA_BGM_INSTANCE))();
    if (!mgr || IsBadReadPtr(mgr, 4)) return;
    memset(g_bgmStopBuf, 0, sizeof g_bgmStopBuf);
    strncpy(g_bgmStopBuf, name, BGM_NAME_MAX);
    ((bgmStop_t)(uintptr_t)(g_base + RVA_BGM_STOP))(mgr, g_bgmStopBuf, fade);
}

// ---- the zone's own default track names ------------------------------------------------------
// Zone.getDefaultNameOfBattleBGM / getDefaultNameOfFieldBGM (0xBEB3C0 / 0xBEB470) both call
// 0x80E140(this = [0x2811190], out field[16], out battle[16]), which is nothing but two inline
// 16-byte copies out of the object: field at this+0x6040, battle at this+0x6050. Read directly
// rather than called -- same answer, no calling convention to get wrong, safe on any frame.
#define RVA_ZONEBGM_MGR 0x2411190    // VA 0x2811190 -> the zone object
#define OFF_ZONEBGM_FIELD  0x6040
#define OFF_ZONEBGM_BATTLE 0x6050
char g_bgmFieldNow[BGM_NAME_MAX + 1] = "";   // what is actually playing out on the field
char g_bgmMine[BGM_NAME_MAX + 1] = "";      // the track WE started, until the fight ends

void bgm_zone_refresh(void) {
    if (!g_base) return;
    uint32_t* pp = (uint32_t*)(uintptr_t)(g_base + RVA_ZONEBGM_MGR);
    if (IsBadReadPtr(pp, 4) || !*pp) return;
    uintptr_t zone = (uintptr_t)*pp;
    if (IsBadReadPtr((void*)(zone + OFF_ZONEBGM_FIELD), 0x20)) return;
    char f[BGM_NAME_MAX + 1], b[BGM_NAME_MAX + 1];
    memcpy(f, (const void*)(zone + OFF_ZONEBGM_FIELD),  BGM_NAME_MAX);
    memcpy(b, (const void*)(zone + OFF_ZONEBGM_BATTLE), BGM_NAME_MAX);
    f[BGM_NAME_MAX] = 0; b[BGM_NAME_MAX] = 0;
    if (!f[0] && !b[0]) return;                     // no zone loaded yet
    if (strncmp(f, g_zoneField, BGM_NAME_MAX) == 0 && strncmp(b, g_zoneBattle, BGM_NAME_MAX) == 0)
        return;
    strncpy(g_zoneField,  f, BGM_NAME_MAX); g_zoneField[BGM_NAME_MAX]  = 0;
    strncpy(g_zoneBattle, b, BGM_NAME_MAX); g_zoneBattle[BGM_NAME_MAX] = 0;
    // A different pair means a different area, so the learned tracks belong to the old one.
    g_bgmScripted[0] = 0;
    g_bgmFieldNow[0] = 0;
    g_bgmMapN = 0;
    ff13logv("[FF13-SRP] zone BGM defaults: field=\"%.16s\" battle=\"%.16s\"\n",
             g_zoneField[0] ? g_zoneField : "(none)", g_zoneBattle[0] ? g_zoneBattle : "(none)");
}

// What a picked fight should ask for; whatever this returns is started through lazyPlay, so the
// track need not be loaded. `tag` (may be NULL) gets a short form of the same answer for the
// picker line, so a wrong choice is obvious before the fight starts.
static const char* bgm_pick_track3(uint32_t btsc, const char** origin, const char** tag,
                                   int useLearned) {
    const char* dummy = NULL;
    if (!tag) tag = &dummy;
    const char* learned = useLearned ? bgm_lookup(btsc) : NULL;
    // A measurement first; everything below is derived. Only fights the game itself started are
    // recorded (see on_encount_scene), so this cannot be an echo of our own substitution.
    if (learned) { *origin = " (heard in this fight here)"; *tag = "heard"; return learned; }
    // Then this fight's exact track from the event data, once its arena name has been seen.
    const char* t = bgm_event_track(btsc);
    if (t) { *origin = " (this fight's own track, from the event data)"; *tag = "exact"; return t; }
    // Then the same answer worked out offline by build_bgm_table.py.
    const ff13_bgm_scene* row = bgm_scene_row(btsc);
    if (row) {
        // "verified" was heard from the game itself; "scene" is right but belongs to the cutscene
        // that leads in (which the picker skips); "matched" is derived from the event data.
        switch (row->origin) {
        case FF13_BGM_VERIFIED:
            *origin = " (heard from the game itself)";            *tag = "verified"; break;
        case FF13_BGM_INHERIT:
            *origin = " (carried in from this fight's cutscene)"; *tag = "scene";    break;
        default:
            *origin = " (this fight's track, matched offline)";   *tag = "matched";  break;
        }
        return row->track;
    }
    // An unmatched event fight is usually ordinary enemies in a scripted fight, and those play the
    // AREA'S ORDINARY BATTLE THEME. Falling through made every one sound like a boss. The
    // hand-written list's kind counts on both sides here: btsc02011 is script-started (so the
    // story list calls it a boss) yet the hand list says EVENT, and its own start plays the
    // ordinary theme; btsc01019 is kind 0 in the scene data and field-touched, so only the hand
    // list ever calls it a boss.
    const ff13_btsc_info* bi = btsc_info(btsc);
    int handKind = 0;
    btsc_hand(btsc, 0, NULL, NULL, NULL, NULL, &handKind);
    if (((bi && (bi->kind & FF13_BTSC_EVENT)) || handKind == FF13_NAME_KIND_EVENT)
        && !(bi && (bi->kind & FF13_BTSC_BOSS)) && handKind != FF13_NAME_KIND_BOSS
        && g_zoneBattle[0]) {
        *origin = " (event fight, unmatched -> this area's ordinary battle theme)";
        *tag = "event"; return g_zoneBattle;
    }
    // Missions play BGM_MISSION -- in the Titan trials that is also the FIELD music, so it reads
    // as "no change", and that is correct: of the trial missions only 11830 has a track of its own
    // (a VERIFIED row above; user-confirmed both ways).
    if (bi && (bi->kind & FF13_BTSC_MISSION)) {
        *origin = " (a mission -> the mission battle theme)";
        *tag = "mission"; return BGM_MISSION;
    }
    // Everything below this line assumes a boss, so anything that is not one takes the area's
    // ordinary battle theme here rather than opening on a boss theme.
    if (!(bi && (bi->kind & FF13_BTSC_BOSS)) && !is_boss_scene(btsc)
        && handKind != FF13_NAME_KIND_BOSS && g_zoneBattle[0]) {
        *origin = " (not a boss -> this area's ordinary battle theme)";
        *tag = "area-battle"; return g_zoneBattle;
    }
    // Then the area's boss track from the event data, which needs nothing to have been observed.
    t = bgm_zone_track(field_zone_id());
    if (t) { *origin = " (this area's boss track, from the event data)"; *tag = "area"; return t; }
    if (g_bgmScripted[0]) { *origin = " (boss track heard in a story fight here)";
                            *tag = "heard"; return g_bgmScripted; }
    *origin = " (the generic boss theme)"; *tag = "generic";
    return BGM_DEFAULT_BOSS;
}
const char* bgm_pick_track(uint32_t btsc, const char** origin) {
    return bgm_pick_track3(btsc, origin, NULL, 1);
}
// Most fight tracks start through lazyPlay, which never reaches the immediate-play entry below --
// without this listener the learner only ever saw the rare direct plays. Observation only.
const uint8_t P_BGMLAZY[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x84,0x00,0x00,0x00};
void __cdecl on_bgm_lazyplay(uint32_t ecx, uint32_t retAddr, uint32_t* pName, uint32_t* pFade) {
    (void)ecx; (void)retAddr; (void)pFade;
    const char* name = (pName && *pName) ? (const char*)(uintptr_t)*pName : NULL;
    if (!name || IsBadReadPtr((void*)name, BGM_NAME_MAX)) return;
    static volatile LONG lazyLogN = 0;
    if (lazyLogN < 200) { InterlockedIncrement(&lazyLogN);
                          ff13logv("[FF13-SRP] BGM lazyPlay \"%.16s\"\n", name); }
    if (!g_bgmLearnFor) return;
    // Our own plays cannot mislead this: the pin is armed only for unswapped encounters, and the
    // mod itself only ever plays for picked (swapped) ones.
    if (bgm_is_outcome_track(name)) { bgm_learn_close(); return; }   // the fight is over
    if (bgm_is_battle_track(name)) bgm_learn_note(name);
}

void __cdecl on_bgm_play(uint32_t ecx, uint32_t retAddr, uint32_t* pName, uint32_t* unused) {
    (void)ecx; (void)unused;
    const char* name = (pName && *pName) ? (const char*)(uintptr_t)*pName : NULL;
    int nameOk = (name && !IsBadReadPtr((void*)name, BGM_NAME_MAX));
    // While the learn pin is armed, every battle track is a candidate; the last one standing when
    // the fight has been running for a few seconds is committed (bgm_learn_tick).
    if (g_bgmLearnFor && nameOk && bgm_is_outcome_track(name)) bgm_learn_close();
    if (g_bgmLearnFor && nameOk && bgm_is_battle_track(name)) bgm_learn_note(name);
    // Stamp every track, so startBattle_l can look back at what was already playing.
    if (nameOk && bgm_is_battle_track(name)) {
        strncpy(g_bgmLastPlayed, name, BGM_NAME_MAX);
        g_bgmLastPlayed[BGM_NAME_MAX] = 0;
        g_bgmLastPlayedAt = GetTickCount();
    }
    // ...and the other order, for a scripted fight that starts its music afterwards: within a few
    // seconds only, and never the theme already playing out on the field.
    if (g_bgmLearnScripted && nameOk && bgm_is_boss_track(name)
        && (int)(g_bgmLearnUntil - GetTickCount()) > 0
        && !(g_bgmFieldNow[0] && strncmp(name, g_bgmFieldNow, BGM_NAME_MAX) == 0)) {
        strncpy(g_bgmScripted, name, BGM_NAME_MAX);
        g_bgmScripted[BGM_NAME_MAX] = 0;
        g_bgmLearnScripted = 0;
        ff13logv("[FF13-SRP] learned scripted-battle track: \"%.16s\" (usable by picked fights)\n",
                 g_bgmScripted);
    }
    // A track starting while we stand on the field is this area's field music whatever the zone
    // record calls its default -- scripts override it -- and remembering the real one is what
    // stops it being learned as "the boss theme of this area".
    if (nameOk && field_zone_state() == 3 && !g_bgmWant[0] && !g_bsStarted) {
        strncpy(g_bgmFieldNow, name, BGM_NAME_MAX);
        g_bgmFieldNow[BGM_NAME_MAX] = 0;
    }
    // The game starting its own battle theme is the cue to ask for ours from a later tick. The name
    // is deliberately NOT rewritten in that flow: naming a track the area has not loaded leaves
    // the previous one running and makes the manager treat our later request as a duplicate. ONE
    // exception -- the wanted track is playing RIGHT NOW and this call is about to replace it:
    // rewriting to the already-playing name keeps it, where stop+lazyPlay went silent.
    if (g_bgmWant[0] && nameOk && bgm_is_battle_track(name)
        && (g_bsStarted || (overlay_in_battle() && (int)(g_btlStatAt - g_bgmArmedAt) >= 0))
        && (int)(g_bgmWantUntil - GetTickCount()) > 0
        && strncmp(name, g_bgmWant, BGM_NAME_MAX) != 0) {
        char cur[BGM_NAME_MAX + 1];
        if (bgm_current(cur) && strncmp(cur, g_bgmWant, BGM_NAME_MAX) == 0) {
            static char keep[16];
            memset(keep, 0, sizeof keep);
            memcpy(keep, g_bgmWant, strlen(g_bgmWant));
            *pName = (uint32_t)(uintptr_t)keep;
            strncpy(g_bgmMine, g_bgmWant, BGM_NAME_MAX);   // fight-over cleanup owns it from here
            g_bgmMine[BGM_NAME_MAX] = 0;
            ff13logv("[FF13-SRP] battle picker: \"%.16s\" is already playing -- \"%.16s\" "
                     "rewritten to keep it\n", g_bgmWant, name);
            g_bgmWant[0] = 0; g_bgmDue = 0; g_bgmHoldUntil = 0; g_bgmSkipHold = 0;
            return;
        }
    }
    int due = 0;
    // Guarded on a picked fight being under way, not on the battle state: on hardware the order is
    // getBattleScene, this call with the area's ordinary theme, startBattle, the scene object, the
    // camera, and only then the state -- so waiting for the state never latched. Not on g_btlScene
    // either, which the result-filing clears about two seconds in.
    if (g_bgmWant[0] && nameOk && bgm_is_battle_track(name)
        && (g_bsStarted
            || (overlay_in_battle() && (int)(g_btlStatAt - g_bgmArmedAt) >= 0))
        && (int)(g_bgmWantUntil - GetTickCount()) > 0
        && strncmp(name, g_bgmWant, BGM_NAME_MAX) != 0) {
        strncpy(g_bgmTheirs, name, BGM_NAME_MAX);
        g_bgmTheirs[BGM_NAME_MAX] = 0;
        g_bgmDue = 1;
        due = 1;
    }
    if (g_bgmLogN < 60) {
        g_bgmLogN++;
        ff13logv("[FF13-SRP] BGM play \"%.16s\"%s  <- caller 0x%08X\n",
                 nameOk ? name : "(unreadable)",
                 due ? "  (picked fight: asking for its own track next frame)" : "", retAddr);
    }
}

// Cave builder for the site above. Unlike the shared ff13_hook helpers this one feeds the handler's
// RETURN VALUE back into EAX by overwriting the EAX image pushad left on the stack.
void* install_encscene_cave(uintptr_t hookAddr, void* handler) {
    if (memcmp((const void*)hookAddr, P_ENC, sizeof(P_ENC)) != 0) return NULL;
    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return NULL;
    uint8_t code[] = {
        /*+0x00*/ 0x60,                            // pushad            ; saved EAX at [esp+0x1C]
        /*+0x01*/ 0x9C,                            // pushfd            ; saved EAX at [esp+0x20]
        /*+0x02*/ 0x50,                            // push eax          ; the resolved scene id
        /*+0x03*/ 0xE8, 0,0,0,0,                   // call handler      (rel32 @+4)
        /*+0x08*/ 0x83, 0xC4, 0x04,                // add esp, 4        ; saved EAX at [esp+0x20]
        /*+0x0B*/ 0x89, 0x44, 0x24, 0x20,          // mov [esp+0x20], eax  ; -> popad yields our id
        /*+0x0F*/ 0x9D,                            // popfd
        /*+0x10*/ 0x61,                            // popad
        /*+0x11*/ 0x89, 0x81, 0x54, 0x03, 0x00, 0x00, // mov [ecx+0x354],eax   (relocated original)
        /*+0x17*/ 0xE9, 0,0,0,0                    // jmp hookAddr+6    (rel32 @+0x18)
    };
    int32_t relCall = (int32_t)((uintptr_t)handler - ((uintptr_t)cave + 0x08));
    int32_t relBack = (int32_t)((hookAddr + 6)     - ((uintptr_t)cave + 0x1C));
    memcpy(&code[0x04], &relCall, 4);
    memcpy(&code[0x18], &relBack, 4);
    memcpy(cave, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(code));
    DWORD old;
    if (!VirtualProtect((void*)hookAddr, 6, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(cave, 0, MEM_RELEASE); return NULL;
    }
    uint8_t* h = (uint8_t*)hookAddr;
    h[0] = 0xE9;
    int32_t relHook = (int32_t)((uintptr_t)cave - (hookAddr + 5));
    memcpy(h + 1, &relHook, 4);
    h[5] = 0x90;                                   // pad the 1 leftover byte
    VirtualProtect((void*)hookAddr, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)hookAddr, 6);
    return cave;
}

