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

#ifndef SRP_INTERNAL_H
#define SRP_INTERNAL_H

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include "ff13_hook.h"
// Per-scene names, sort key and tag, from a generated header. Not committed (the strings are the
// game's own text); without it every fight is drawn unnamed and in the built-in order.
#if defined(__has_include)
#  if __has_include("btsc_picker.h")
#    include "btsc_picker.h"
#    define FF13_HAVE_BTSC_NAMES 1
#  endif
#endif
// Sort key for a fight the picker table does not list: it sorts after every listed row, grouped by kind.
#define FF13_BTSC_ORDER_LAST 0xFFFFu
// Fallbacks so the picker code compiles and reads the same without btsc_picker.h.
#ifndef FF13_NAME_KIND_BOSS
#  define FF13_NAME_KIND_BOSS    1
#  define FF13_NAME_KIND_EVENT   2
#  define FF13_NAME_KIND_MISSION 3
#endif
#ifndef FF13_BTSC_NO_TEXT
#  define FF13_BTSC_NO_TEXT 0xFFFFFFFFu
#endif
// A binding is a virtual-key code with its modifier bits packed above it, so a binding still
// travels as the single UINT everything already passes around.
#define HK_CTRL   0x0100u
#define HK_SHIFT  0x0200u
#define HK_ALT    0x0400u
#define HK_MODS   0x0700u
#define hk_key(x)  ((UINT)((x) & 0xFFu))
#define hk_mods(x) ((UINT)((x) & HK_MODS))
// Hotkey poll period; the loop and hk_edge live in dllmain.c. Declared here because srp_save.c
// stores its timeouts as tick counts = milliseconds / this period.
#define HK_POLL_MS 80
int hk_edge(UINT b, int* was);
int hk_active(void);        // may a hotkey fire at all (the global_hotkeys test)
UINT hk_mods_now(void);     // the modifier bits held right now
#include "ff13_keys.h"

// Every deadline here is "(int)(now - until) < 0" with 0 meaning none, which reads as STILL RUNNING
// once Windows has been up 24.9 days (GetTickCount past 2^31): a party hold that had ended pinned
// everyone in place for the whole fight. So the mod's clock counts from this DLL's first call.
DWORD srp_ticks(void);
#define GetTickCount() srp_ticks()
#include "ff13_ini.h"

// ---- generated tables read by more than one TU ----
#include "ovl_text.h"
#include "bgm_table.h"
// Which enemy models a scene needs and which charaset holds them (build_charaset_table.py).
// An enemy whose model is not resident is built and then fails to activate.
#include "charaset_table.h"
// What each battle scene fields, extracted from the game's btsc*.wdb (build_btsc_table.py).
#include "btsc_table.h"

// ---- shared constants: RVAs, field offsets and sizes ----
#define KILL_OFF      0
#define KILL_ONEHIT   1
#define KILL_INSTANT  2               // instant-win AND one-hit-kill
#define MOD_DATA_DIR "ff13-mods"
// ---- combat-actor field offsets (from `this`) ----
#define OFF_HP        0x290   // current HP   (int32) -- the field the cave writes
#define OFF_MAXHP     0x6C8   // max HP       (int32, direct dword; no indirection)
#define OFF_GUARD324  0x324   // enemy guard  (int32) -- the cave's enemy gate (party=0 / enemy!=0)
#define OFF_DEFEATED  0x259   // byte; bit 0x02 = defeated
#define OFF_TEAM      0x24E   // byte of the actor flags; bit 0x04 is NOT a side test (measured set on
                              // party actors too). Use OFF_GUARD324 for enemy-or-not.
#define OFF_LIFECYCLE 0x270   // dword of eight nibbles; bits 12-15 = lifecycle (1 alive / 3 dead)
#define OFF_ACTOR_NAME 0x1A40 // charaspec name, 16 bytes NUL-padded (read off the placement join)
// The actor's numeric statuses: five layers of 64 ints embedded here, read as
// *(int*)(actor + 0x2C4 + layer*0x100 + idx*4) (0x4B5150). Chara::getStatus (0x472430) reads
// layer 4 and applies the buffs on top of what it finds, so this slot is the UNBUFFED value and a
// buff still scales what is written here. Indexes are the script Party API's ST_* constants.
#define OFF_ACTOR_STATS  0x2C4
#define ACTOR_STAT_LAYER 4
#define ST_ATK_PHY       5    // Chara::getAtkPhy (0x472310) is getStatus(5): damage AND the UI
// Read span bounds-checked before touching an actor.
#define ACTOR_SPAN    (OFF_MAXHP + 4)
// ---- BattleMain state: read only by the overlay, to hide field-only lines during a battle ----
#define RVA_BM_PTR   0x23FD208   // VA 0x27FD208: pointer to the BattleMain singleton (BM)
// VA 0x283EAC0: the game's IDirect3DDevice9*, CreateDevice's out pointer (call at 0xEB2B63).
// The game asks for PUREDEVICE (flags 0x52/0x56), which plain d3d9.dll serves from its own vtable.
#define RVA_D3DDEV_PTR 0x243EAC0
#define OFF_BM_STATE 0x44        // [BM+0x44] = battle state machine (0 = idle; != 0 = in battle)
#define RVA_FRAMETICK 0x2AA480   // VA 0x6AA480: app-clock tick; ecx=singleton, [esp+4]=int* frame delta (1/300000 s)
#define RVA_SETTER 0x1B21C0    // VA 0x5B21C0 (setAiTerritoryState backend: cdecl setter(id, state))
#define RVA_WH_HOOK 0x7E029D
#define RVA_WH_CONT 0x7E02A3
#define RVA_WH_SKIP 0x7E02E0
#define SUPPRESS_PREFIX "gr_mon_"
#define RVA_STARTBATTLE 0x7E3DF0     // VA 0xBE3DF0 startBattle_l entry
#define RVA_TARGETHP_PTR 0x24337B4
#define OFF_TARGETHP     0xBEC
#define BGM_NAME_MAX  16
#define BGM_HOLD_MAX_MS     8000
#define OFF_TOUCHED_ID 0x14648                    // [fieldMgr+0x14648] = pending touched object id
#define RVA_ENCSCENE 0x75032D        // VA 0xB5032D `mov [ecx+0x354],eax`
#define RVA_GETBTSC 0x741640         // VA 0xB41640 getBattleScene(id, flag), ecx = DataBattle DB
#define RVA_ENCID   0x7500A0         // VA 0xB500A0 (id) -> 0xB50270 on [0x282B190]
#define RVA_LOADSND 0x7DE1C0         // VA 0xBDE1C0 Resource.loadSoundResource(String, String)
#define RVA_LOADCHARASET 0x7DE930    // VA 0xBDE930 Resource.loadCharaByCharaSet(String)
#define CSET_NAME_MAX 39
#define RVA_LOADCHARA 0x7DED30       // VA 0xBDED30 Resource.loadChara(String)
#define RVA_ISLOADEDCHARA 0x7DEF80   // VA 0xBDEF80 Resource.isLoadedChara(String)
#define RVA_LOADBG 0x7DE6C0          // VA 0xBDE6C0 Resource.load_privBgByMapSet(String)
#define RVA_DRAWENV_BG    0x7E0EF0   // VA 0xBE0EF0  Scene.overwriteDrawEnvManually_l
#define RVA_DRAWENV_CHARA 0x7C9450   // VA 0xBC9450  Chara.setDrawEnvManually_l
#define RVA_BOX_ON  0x7DFF30         // VA 0xBDFF30  Scene.validateObject
#define RVA_BOX_OFF 0x7DFEC0         // VA 0xBDFEC0  Scene.invalidateObject
#define RVA_SCREENENV 0x7C4610       // VA 0xBC4610  Camera.enableAutoScreenEnv
#define RVA_LAYOUT_ON  0x7DFAC0      // VA 0xBDFAC0  Scene.showLayoutInstance (= sfShowBgInstance)
#define RVA_LAYOUT_OFF 0x7DFB40      // VA 0xBDFB40  Scene.hideLayoutInstance (= sfHideBgInstance)
#define RVA_OPTIMA_REGIST 0x7DCAF0   // VA 0xBDCAF0
#define RVA_OPTIMA_SET    0x7DCBC0   // VA 0xBDCBC0
#define RVA_OPTIMA_CLEAR  0x7DCC00   // VA 0xBDCC00
#define RVA_OPTIMA_CLRALL 0x7DCC50   // VA 0xBDCC50
#define RVA_ADDROLE       0x7DB910   // VA 0xBDB910
#define RVA_STARTCINEMA 0x7E33D0     // VA 0xBE33D0
#define EVENT_STAGE_MS 800
#define EVENT_GIVEUP_CAP_MS 20000
#define EVENT_SIDEEFFECT_MS 1200
#define ARENA_REACH 30.0f
#define ARENA_SAME  2.5f        // slots this close together ON THE GROUND are one character
#define ARENA_YBAND 5.0f
#define CHARA_NAME_OFF 0x4C
#define WARP_MAXSLOT 0x4000
#define OFF_BM_STAGE 0x3F4        // [BattleMain+0x3F4] = the 4x4 the battle places everything with
#define BOSS_SLOTS 12
#define ARENA_ENEMIES 6
#define BOSS_BUILD_MS 12000
#define ARENA_GATHER_MS 400
#define ARENA_SETTLE_MS 1500
#define ARENA_DERIVED_MS 6000
#define OPTIMA_LOG_MAX 400   // a story run to a fight can pass several party changes
#define LIGHT_MAP   1
#define LIGHT_CHARA 2
#define LIGHT_EASE_MS 1300
#define LIGHT_ROOM_AGAIN_MS 650
#define RVA_ISREAD_CALL 0x379642     // VA 0x779642  add ecx,0x182C
#define RVA_CSREQ 0x755160           // VA 0xB55160
#define RVA_ACTORSECT 0x3795EA       // VA 0x7795EA  add ecx,0xE8   (ecx = the actor)
#define RVA_STARTBTL 0x0BB5B0          // VA 0x4BB5B0 startBattle(btsc, flags)
#define RVA_GAMELOG 0x4A5B20              // VA 0x8A5B20
#define RVA_ASSERT  0xA8B2E0              // VA 0xE8B2E0 the game's assert (dies at +0x96)
#define RVA_TRACE 0x4A5A60                 // VA 0x8A5A60
#define RVA_CAMSEQ 0x195620          // VA 0x595620
#define RVA_CAMREQ      0x0D6C20  // VA 0x4D6C20  __thiscall requestCamArts(BattleMain, req*, int)
#define RVA_HASCAM 0x7421A0                // VA 0xB421A0  __thiscall hasIntroCamera(wrapper)
#define RVA_FIELDMGR_PTR 0x23FF124     // VA 0x27FF124, candidate global
#define RVA_BGMPLAY   0x6CED90       // VA 0xACED90 BgmManager play entry (ecx, arg2 = name)
#define RVA_BGMLAZY   0x6CF080       // VA 0xACF080 BgmManager::lazyPlay (ecx, arg1 = name)
#define RVA_BANKPLAY  0x6F7A40       // VA 0xAF7A40 the sound service's "give me a stream"
#define RVA_ENCCHECK   0x1CEE00       // VA 0x5CEE00 doEncountCheck (ecx = field manager)
#define BS_MENU_MAX 400   // the 11xxx band alone holds ~395 scenes
#define BS_NAME_COL 34
#define BS_ROWS 10
#define RVA_SPECREPL 0x71140         // VA 0x471140 -- the charaspec swap (thiscall)
#define RVA_ENCF4      0x7505B6        // VA 0xB505B6
#define RVA_RESOLVETGT 0x158E80      // VA 0x558E80
#define RVA_ABILLABEL 0x943B0        // VA 0x4943B0
#define RVA_AIAFTER   0x7C3300       // VA 0xBC3300 Battle.apiAiPushEnemyAfterExecArgs
// Refuse a warp home longer than this. The streaming budget measures ~390 units; a trip that
// overruns it strands the party on unstreamed ground, in a load the game never comes out of.
// 350 leaves margin on a figure that was measured rather than documented.
#define HOME_MAX_DIST 350.0f
#define RVA_SENDTRIG  0x417400            // VA 0x817400  __thiscall(ecx=ZSM, int trigger), ret 4
#define RVA_GOMENU         0x4ED130    // VA 0x8ED130 the game-over menu update
#define RVA_GOSE           0x4ED24D    // VA 0x8ED24D: the push/mov/call that makes the sound
#define GO_SE_LEN          13          // length of that sequence; all of it is blanked for silence
#define RVA_WORKER_HOW 0x414DDC      // VA 0x814DDC  mov eax, [0x27FF068] -- just after the store
#define RVA_WORKER_IN  0x414B90      // VA 0x814B90  push ebp; mov ebp,esp; sub esp,0x808
#define RVA_WORKER_ARM 0x414F6B      // VA 0x814F6B  push 0x10CEC80 -- both values are settled here
#define RVA_PLACEJOIN 0x66C07        // VA 0x466C07  mov eax, 2 -- the join, before the trace
#define RVA_ITEMTAKE  0x76A940      // VA 0xB6A940  __cdecl removeItem(const char id[16], int n)
#define RVA_ITEMCOUNT 0x7268E0      // VA 0xB268E0  __cdecl getItemCount(const char id[16]) -> int
#define RVA_ITEMGIVE  0x7268C0      // VA 0xB268C0  __cdecl addItem(const char id[16], int n, int f)
#define RVA_TPGET     0x7272E0      // VA 0xB272E0  __cdecl getTp(void) -> int
#define RVA_BTRES_CLEAR 0x0FB880    // VA 0x4FB880  __cdecl void(void): empties db_bt_result under its lock
#define RVA_BTRESDB_PTR 0x242B060   // VA 0x282B060 -> db_bt_result (the battle result store; the overlay reads sheet 0 through it too)
#define OFF_BTRES_LEVEL 0x2C        // sheets in use, 0..3; the 4 sheets are 0x208 each from +0x30
#define RVA_TPSET     0x7272F0      // VA 0xB272F0  __cdecl setTp(int)
#define RVA_LOCK_ENTER  0x11B50     // VA 0x00411B50  __thiscall(lock, int tag)  ret 4
#define RVA_LOCK_LEAVE  0x11BD0     // VA 0x00411BD0  __thiscall(lock)
#define RVA_LOADSHOW 0x4E2080      // VA 0x8E2080
#define RVA_LOADHIDE 0x4E2150      // VA 0x8E2150
#define RVA_ENCREFUSE 0x1CEEA4     // VA 0x5CEEA4
#define RVA_ACTIVATE      0x0DA820   // VA 0x004DA820  activateBattleChara entry (the probe)
#define RVA_ACTIVATE_GATE 0x0DA8A3   // VA 0x004DA8A3  cmp [ebp-0xb8],0 -- the residency verdict
#define RVA_GETWORLDPOS 0x38EEE0       // VA 0x78EEE0 GetWorldPos entry (ecx = SceneObject)
#define RVA_COMMIT      0x39C5E0       // VA 0x79C5E0 commit-transform (ecx=obj, arg0=Vector4* src)
#define RVA_CAM_RECENTER 0x236A20     // VA 0x636A20  __thiscall recenter(this)
#define RVA_WIDESHOT 0x1A5807     // VA 0x5A5807
#define RVA_GETCAM 0x2C0880       // VA 0x6C0880  void __cdecl getCurrentCamera(float out[16])
#define OVL_FLASH_OFF  0xFFC4CFCF   // status flash   : something switched off

// ---- shared types ----
typedef struct { const char* name; float x, y, z; } ff13_enemy;
typedef struct ff13_arena_s { uint16_t btsc; const char* arena; uint32_t flags;
                 float ex, ey, ez;      // the enemy, as an offset on the stage
                 float wp[3][3];        // the party, as three world positions
                 float bw[3];           // and the enemy's, for pointing them at it beforehand
                 float cm[16];          // the camera the story fight settles into, as a 4x4
                 float ct[3];           // what that camera looks at
                 float c0[3], ct0[3];   // the pair it opens on
                 float cd[3], cd2[3];   // the two that drag it in from wherever it was
                 float ca[2];           // and the angles it rests at once it gets there
                 float camHoldMs;       // how long to keep it there (0 = set it and let go)
                 int   restartAfterWin; // 1 = end a win at the checkpoint, 0 = let the story have it
                 const char* chset;     // character set to load first (NULL = the table's choice)
                 const char* bg;        // mapset holding the arena terrain (NULL = already there)
                 const char* bgm;       // NULL = resolve as usual; "" = keep the fight's own track
                                        // (the offline match is wrong for this scene)
                 float sp[3][3];        // where the SCENE stands the party (0 = kPartyStage)
                 ff13_enemy en[ARENA_ENEMIES];     // its enemies, when it fields more than one
                 const char* btlBg[2];             // the BATTLE's own mapsets (bt_scene cells 3/4)
                 const char* ev[4];                // event, callback, data pack, success label
                                                   // (NULL name = no event door; arena door instead).
                                                   // Also the source of the picker's "CS" mark: a
                                                   // door is the only way a picked fight plays a
                                                   // cutscene. See scene_has_cutscene.
                 const char* evChara[8];           // field characters its "before" function creates,
                                                   // as name/model pairs
                 const char* evArea[2];            // ...and the area groups it activates
                 const char* evPack2;              // a second data pack, where the story loads two
                 int evScript;                     // 1 = the entry point is a SCRIPT event rather
                                                   // than a cutscene (startCinema mode 0)
                 int evMarker;                     // party marker group to stand them on first
                                                   // (0 = none; the groups the game uses are 1-up)
                 float evPos[3];                   // ...or where to walk them, when the story
                                                   // relies on the player having walked there
                 const char* evLoad[2];            // cutscene + pack to load first, when the root
                                                   // is not itself the cutscene
                 int evFlag;                       // scenario flag to put back before firing, so a
                                                   // chain that has run can run again (0 = leave it)
                 int evMusic;                      // 1 = ask for this fight's own track anyway (an
                                                   // event door otherwise leaves music to the story)
                 const char* evSetup;              // script run once the chain is under way, for the
                                                   // one thing no native does: paradigms. Spent on
                                                   // the chain's next native call -- see g_setupWant
                 int evNoFight;                    // 1 = run the chain for its side effects, not the
                                                   // fight: evFlag is set rather than rewound (which
                                                   // shuts the trailing cutscene's gate) and the
                                                   // picker starts the fight itself afterwards
                 const char* evEvFlag;             // named event flag to raise before firing; the
                                                   // chains that check one mean "arrived at cold
                                                   // rather than walked into"
                 int evQuiet;                      // 1 = set the story's auto-music-off bit (Party
                                                   // work 1 bit 0) before firing, as the story's
                                                   // OWN staging does -- the battle step then rolls
                                                   // no theme and the chain's track carries through
                 int evGate;                       // scenario-flag value that SHUTS the chain's
                                                   // replay after a converted end (0 = none), or
                                                   // the reload re-fights instead of giving the
                                                   // field back
                 const char* evEndRp;              // restart point RE-MARKED at fight-over so a
                                                   // converted end reloads OUTSIDE a chain that
                                                   // marked its checkpoint inside itself (NULL =
                                                   // leave the checkpoint alone)
                 const char* evRpFlag;             // the event flag this fight's RESTART LOADER
                                                   // raises; taken down at the pick and after the
                                                   // way back, or the chain opens its preparation
                                                   // menu on every pick
                 int noHomeWarp; }                 // 1 = leave the party where the fight's own end
                                                   // puts them (the spot is still recorded). LAST
                                                   // on purpose: rows initialise positionally, so
                                                   // a field inserted above silently shifts them
        ff13_arena;

// Field objects the story's own hand removes right before a fight, for picks that start without
// that hand (no event door). Every row is undone once the field is home.
#define FDROP_AREA 0
#define FDROP_HIDE 1
#define FDROP_AREA_EX 2   // dropped via deactivateAreaEx, as the story handles this group
typedef struct { uint16_t btsc; uint8_t kind; const char* name; } ff13_fielddrop;
const ff13_fielddrop* field_drops(uint32_t btsc, int* n);

// ---- diagnostic probes (release builds get the no-ops) ----
#ifdef FF13_DIAG
void turn_watch_tick(void);
void boss_basis_probe(void);
void encinfo_probe(const char* when);
void actor_watch_tick(void);
#else
#define turn_watch_tick() ((void)0)
#define boss_basis_probe() ((void)0)
#define actor_watch_tick() ((void)0)
#endif

// ---- shell, config, logging and the field query (srp_core.c) ----
const char* gs_steps_str(void);
const char* gs_steps_arrow(const char* arrow);
extern UINT g_helpVk;
extern UINT g_armVk;
extern UINT g_supVk;
extern UINT g_gsVk;
extern UINT g_warpVk;
extern UINT g_warpCancelVk;
extern UINT g_boVk;
extern UINT g_saveVk;
extern UINT g_camUnstickVk;
extern UINT g_ssVk;
extern UINT g_menuOkVk;
extern UINT g_listPrevVk;
extern UINT g_listNextVk;
extern volatile int g_hkGlobal;
extern volatile int g_killMode;
extern volatile unsigned char g_battleOverlay;
extern int  g_logLevel;
void crash_report_install(void);
void addr_where(uintptr_t a, char* out, size_t n);
void crash_report_rearm(void);
#define GS_STEPS_MAX 8
extern double g_gsSteps[GS_STEPS_MAX];
extern int    g_gsStepN;
extern volatile int g_gsStep;
extern volatile unsigned char g_gsEnabled;
extern double g_gsFactor;
void gs_apply(int step);
// Worker thread. Replaces the step list; the factor that was on stays on if it is still listed.
void gs_steps_set(const double* v, int n);
extern volatile unsigned char g_armed;
extern int  g_dataDirOk;
int data_file(char* out, size_t n, const char* name);
void ff13log_reset(void);
void ff13loga(const char* fmt, ...);
void ff13log(const char* fmt, ...);
void ff13logv(const char* fmt, ...);
extern uintptr_t g_base;
int ui_english(void);
extern void* g_fontJp;
const char* ui_text(ovl_msg m);
// 1 = draw ASCII only: exactly when ui_text falls back to English.
int ui_ascii_only(void);
// A value row's value between the left/right arrows, and the ellipsis on a row that opens the text
// editor; both fall back to ASCII under ui_ascii_only.
void ui_arrows(char* out, size_t n, const char* value);
const char* ui_ellipsis(void);
int cfg_ieq(const char* a, const char* b);
int cfg_vk_bad(const UINT* vk);
void cfg_vk_clear_bad(UINT* vk);          // the binding was set anew: it no longer reads as bad
void cfg_bad_clear(const char* name);     // ...likewise for a setting named in cfg_bad_list()
const char* cfg_bad_list(void);
void config_load(void);
// Worker thread only. Rewrites the one line [section] key in the ini beside the exe (added when
// missing), leaving every other byte as it was. 1 = written; 0 = logged and flashed.
int  config_write(const char* section, const char* key, const char* value);
// The [hotkeys] bindings in the ini's table order: the name, and the variable it sets.
int hk_binding_count(void);
const char* hk_binding_name(int i);
UINT* hk_binding_vk(int i);
void state_save(void);
void state_load(void);
uint32_t field_zone_id(void);
int field_zone_state(void);
int wait_for_bytes(uintptr_t addr, const uint8_t* expect, int len, int timeoutMs);

// ---- one-hit cave, high speed and enemy suppression (srp_speed.c) ----
extern volatile int g_seenCount;
void __cdecl on_apply_hp_delta(uint32_t thisActor);
extern void* g_onehitCave;
void* install_onehit_cave(uintptr_t hookAddr, const volatile void* armedFlag, uintptr_t returnAddr);
extern const uint8_t P_FRAMETICK[9];
void __cdecl on_frame_tick(uint32_t pDelta);
extern const uint8_t P_SETTER[6];
void __cdecl on_terr_setter(uint32_t id);
extern volatile unsigned char g_suppressEnabled;
int __cdecl should_skip_activate(void* ebpVal);
extern void* g_whCave;
void* install_withhide_cave(uintptr_t hookAddr, void* handler, uintptr_t contAddr, uintptr_t skipAddr);
void __cdecl on_probe_actArea (uint32_t ctx);
void __cdecl on_probe_withHide(uint32_t ctx);
void __cdecl on_probe_ignoreFA(uint32_t ctx);
void __cdecl on_probe_wait (uint32_t ctx);
void __cdecl on_probe_poll (uint32_t ctx);
void __cdecl on_probe_deact (uint32_t ctx);

// ---- battle swap, scene tables, arena/event doors and BGM (srp_battle.c) ----
extern const uint8_t P_SB[9];
extern char g_swapNameBuf[32];
extern volatile uint32_t g_swapBtscId;
extern volatile unsigned char g_swapEnabled;
extern volatile DWORD g_swapWindowUntil;
extern char g_zoneField[BGM_NAME_MAX + 1];
extern char g_bgmWant[BGM_NAME_MAX + 1];
extern volatile DWORD g_bgmWantUntil;
extern volatile unsigned char g_bgmDue;
extern char g_bgmTheirs[BGM_NAME_MAX + 1];
extern volatile int   g_bgmHoldMs;
extern volatile int   g_ovlTopPx;
extern volatile DWORD g_bgmHoldUntil;
extern volatile unsigned char g_bgmSkipHold;
extern char g_bgmHushed[BGM_NAME_MAX + 1];
extern volatile DWORD g_bgmWatchUntil;
extern char g_bgmWatchLast[BGM_NAME_MAX + 1];
int bgm_is_battle_track(const char* n);
int bgm_is_outcome_track(const char* n);
extern volatile uint32_t g_lastBtscId;
extern volatile uint32_t g_lastTouchedId;
extern volatile uint32_t g_fieldMgr;
#define RVA_UNLOADSND 0x7DE370       // VA 0xBDE370 Resource.unloadSoundResource(String, String)
void vm_unload_sound(const char* name, const char* group);
int  sound_slots_free(void);
void sound_make_room(const char* want);
void sound_give_room_back(void);
void bank_log_reset(void);
void bank_log_pump(void);
void bgm_heal_standdown(void);
void __cdecl on_bank_play(uint32_t ecx, uint32_t name);
void __cdecl on_game_assert(uint32_t ecx, uint32_t line, uint32_t* a, uint32_t* unused);
void __cdecl on_start_battle(uint32_t vmctx);
extern const uint8_t P_ENC[6];
extern void* g_encCave;
uint32_t __cdecl on_encount_scene(uint32_t origId);
extern const uint8_t P_GBS[9];
extern const uint8_t P_ENCID[7];
extern const uint8_t P_SBTL[10];
extern const uint8_t P_LSND[6];
extern const uint8_t P_LCS[6];
extern char g_curCharset[CSET_NAME_MAX + 1];
extern char g_csLoaded[CSET_NAME_MAX + 1];
extern const char* volatile g_setupWant;
extern volatile DWORD g_setupUntil;
extern volatile DWORD g_gameTid;
void __cdecl on_load_charaset(uint32_t vmctx);
void vm_load_charaset(const char* name);
extern const uint8_t P_LCH[6];
extern volatile int g_lchOk;
extern const uint8_t P_ILC[6];
extern volatile int g_ilcOk;
extern volatile int g_looseLoaded;
int load_loose_specs(uint32_t btsc, const ff13_charaset* cs);
void loose_specs_report(void);
extern const uint8_t P_LBG[6];
extern char g_curBg[40];
extern volatile int g_bgAsked;
void __cdecl on_load_bg(uint32_t vmctx);
void vm_load_bg(const char* mapset);
void vm_load_sound(const char* name, const char* group);
void vm_field_ucon(void);
void vm_fade_in(void);
int menuskip_install(void);          // Window.showMainMenu made to return without opening
extern volatile unsigned char g_menuSkip;
void vm_restart_save(const char* rp);
extern volatile int   g_gateReplayVal;
extern volatile DWORD g_gateReplayUntil;
extern volatile int   g_uconDue;
void __cdecl on_drawenv_bg(uint32_t vmctx);
void __cdecl on_drawenv_chara(uint32_t vmctx);
void __cdecl on_box_on (uint32_t c);
void __cdecl on_box_off(uint32_t c);
void __cdecl on_layout_on (uint32_t c);
void __cdecl on_layout_off(uint32_t c);
void __cdecl on_screen_env(uint32_t vmctx);
int vm_get_work(int idx);
int vm_capacity_level(void);        // Party.getCapacityLevel(): the Crystarium stage, -1 = unknown
void vm_set_work(int idx, int val);
void vm_set_event_flag(const char* name, int on);
int vm_get_event_flag(const char* name);
void optima_log(const char* when);
void decks_apply(const signed char (*decks)[3], int n, int sel);
uint32_t party_state_key(void);
int party_battle_ids(int out[3]);
int decks_read_live(signed char out[][3], int maxn);
int deck_sel_live(void);
void vm_set_optima_deck(int deck);
int  vm_top_menu_get(void);
const char* zone_state_name(int st);
int scene_win_at_checkpoint(uint32_t btsc);
const signed char (*scene_decks(uint32_t btsc, int* n))[3];
// A stat the story hands a fight that a pick cannot bring with it; NULL for most fights. It goes to
// the fight's ONLY party member, so a fight with more than one is refused rather than guessed at.
// The row must NOT fire on top of the story's own step: `setup` is the initSetupParty stage left in
// globalWork[210] and `capLv` is the Crystarium stage that same step sets -- either one standing is
// enough, the first catching it inside a session, the second across a reload.
typedef struct {
    uint16_t btsc; unsigned char stat; short add; int setup; int capLv;
} ff13_scene_stat;
#define SETUP_PROGRESS_WORK 210     // initSetupParty's own "applied up to here" (BattleMemberDebug)
const ff13_scene_stat* scene_stat(uint32_t btsc);
int battle_resolved(void);          // nonzero once the fight has resolved (before the state ends)
extern const uint8_t P_OPTREG[6];
extern const uint8_t P_OPTSET[6];
extern const uint8_t P_OPTCLR[6];
extern const uint8_t P_OPTALL[3];
extern const uint8_t P_ADDROLE[6];
void __cdecl on_optima_regist(uint32_t vmctx);
void __cdecl on_optima_clear(uint32_t vmctx);
void __cdecl on_optima_clrall(uint32_t vmctx);
void __cdecl on_add_role(uint32_t vmctx);
void __cdecl on_optima_set(uint32_t vmctx);
extern const char* const* g_partyWho;
extern int g_partyWhoN;
extern int g_partyDoor;
const char* const* scene_party_who(uint32_t btsc, int* n);
void party_script_run(const char* when);
extern const struct ff13_arena_s* g_evPending;
extern volatile DWORD g_evAt;
extern volatile DWORD g_evGiveUpCap;
extern volatile DWORD g_evNoFightAt;
extern volatile int   g_evSkipDoor;
extern volatile uint32_t g_evTouch;
void vm_activate_area(const char* group);
void vm_deactivate_area(const char* group);
void vm_deactivate_area_ex(const char* group);
void vm_layout_show(const char* inst, int show);
void vm_party_marker(int group);
void vm_create_field_chara(const char* name, const char* model);
void vm_start_event(const char* name, const char* cb, const char* pack, const char* suc, const char* pack2, int script, const char* loadName, const char* loadPack);
void vm_dispose_event(const char* name, const char* pack, const char* pack2);
extern volatile int g_evCutSeen;
void __cdecl on_start_cinema(uint32_t vmctx);
void vm_sync_wait(void);
int leader_name(char out[20]);
int riding_chocobo(void);
extern volatile int g_ctlLocked;
void ctl_lock(void);
void ctl_hold(void);
// srp_proxy.c -- input silenced at the DirectInput proxy while a pick is being staged
#define DI_GATE_MAX_MS 30000     // every arm expires on its own; holders re-arm before half-life
void di_gate_arm(void);
int  di_gate_left_ms(void);
void di_gate_lift(void);
// Hand the game one keystroke THROUGH a live gate: the gate hands it a zeroed keyboard every read,
// so a byte set in that buffer is a key the game believes is down. DirectInput indexes the keyboard
// by scan code, not VK.
#define DIK_LEFT   0xCB
#define DIK_RIGHT  0xCD
void di_fake_dik(int dik, DWORD ms);
int  ctl_force_on(void);
void ctl_unlock(void);
void vm_start_battle(const char* arena, uint32_t flags, uint32_t btsc);
int chara_id_of(const char* nm);
extern volatile uint32_t g_apIdx[3];
extern volatile float    g_apSrc[3][3];
extern volatile uint32_t g_apObj[3];
extern volatile uintptr_t g_apRot[3];
extern volatile float    g_apPos[3][3];
extern volatile int      g_apCharId[3];
extern volatile float    g_apLeadD[3];
extern char              g_apName[3][24];
extern volatile int      g_apN;
extern volatile DWORD    g_apUntil;
extern volatile float    g_apRefX, g_apRefZ;
extern volatile int      g_apReady;
extern volatile int      g_apStage;
extern volatile int      g_apFace;
extern volatile int      g_apWp[3];
extern volatile int g_faceLogN;
extern unsigned char g_candFlags[WARP_MAXSLOT];
extern volatile int   g_placeBoss;
extern volatile uint32_t g_bossSlots[BOSS_SLOTS];
extern volatile int   g_bossSlotN;
extern volatile float g_bossSlotPos[BOSS_SLOTS][3];
extern volatile float g_bossX, g_bossY, g_bossZ;
extern volatile int   g_placeLogged;
extern volatile float g_bossOx, g_bossOy, g_bossOz;
extern volatile unsigned char g_enUsed[ARENA_ENEMIES];
extern volatile uint32_t g_bossObj;
extern volatile DWORD    g_bossProbeAt;
extern volatile DWORD g_bossOpenUntil;
extern const struct ff13_arena_s* g_arena;
extern const ff13_arena* g_arenaPending;
extern const signed char (*g_decksWant)[3];
extern volatile int      g_decksWantN;
extern volatile uint32_t g_decksWantFor;
extern volatile DWORD    g_decksAgainAt;
extern volatile uint32_t g_optimaWatch;
extern volatile DWORD    g_optimaWatchAt;
extern volatile int      g_optimaWatchN;
extern volatile DWORD    g_arenaStartAt;
extern volatile DWORD    g_arenaCamUntil;
extern volatile int      g_wsSeen;
extern volatile uint32_t g_camCtl;
extern volatile DWORD    g_camHoldUntil;
int arena_has_party(const struct ff13_arena_s* a);
const float* arena_party_stage(const struct ff13_arena_s* a, int i);
void arena_enemy_pick(const char* name, float* ox, float* oy, float* oz);
extern volatile int  g_lightSweep;
extern volatile int  g_sweepDone;
extern volatile int g_arenaCamera;
extern volatile DWORD g_lightMapAt;
extern volatile int   g_lightMapN;
int scene_light_apply(uint32_t btsc, int what, const char* when);
int scene_light_sweep(uint32_t btsc);
const ff13_arena* scene_arena(uint32_t btsc);
int scene_has_cutscene(uint32_t btsc);
extern volatile int g_stageLogged;
extern volatile float g_stagePrev[3];
extern volatile int   g_stagePrevHave;
int stage_origin(float* o);
extern const char* g_stageArena;
int stage_is_ours(const char* arena);
int stage_place(float ox, float oy, float oz, float* out);
extern const uint8_t P_ISREAD[6];
void __cdecl on_isread_call(uint32_t ecx);
extern const uint8_t P_CSREQ[7];
void __cdecl on_charaspec_request(uint32_t ecx, uint32_t retAddr, uint32_t* pA1, uint32_t* pA2);
extern const uint8_t P_ASECT[6];
void __cdecl on_actor_section(uint32_t actor);
int scene_resident(uint32_t btsc, const char** missing);
const ff13_scene_models* scene_models(uint32_t btsc);
int charaset_covers(const ff13_charaset* cs, const ff13_scene_models* sm);
int zone_has_charasets(uint32_t zone);
int band_listed_here(uint32_t zone, uint32_t band, uint32_t btsc);
int scene_belongs_here(uint32_t zone, uint32_t btsc);
const ff13_charaset* charaset_by_name(const char* name);
const ff13_charaset* charaset_needed(uint32_t zone, uint32_t btsc);
void __cdecl on_load_sound(uint32_t vmctx);
void __cdecl on_get_btsc(uint32_t ecx, uint32_t retAddr, uint32_t* pId, uint32_t* pFlag);
extern volatile uint32_t g_btlScene;
extern volatile int      g_btlCreated, g_btlFailed, g_btlNotLoaded;
extern volatile DWORD    g_btlStatAt, g_btlLogUntil;
extern volatile DWORD    g_bgmArmedAt;
extern volatile DWORD    g_btlLastMsgAt;
extern volatile int      g_btlShort;
extern volatile int      g_iwHitThisFight;
extern char g_btlBad[24];
extern volatile DWORD g_pickedEndAt;
void __cdecl on_battle_entry(uint32_t ecx, uint32_t retAddr, uint32_t* pId, uint32_t* pFlag);
extern const uint8_t P_GLOG[6];
void __cdecl on_game_log(uint32_t ecx, uint32_t fmtVal, uint32_t* pArgs, uint32_t* unused);
extern const uint8_t P_TRACE[5];
void __cdecl on_trace(uint32_t ecx, uint32_t fmtVal, uint32_t* pArgs, uint32_t* unused);
extern const uint8_t P_CAMSEQ[9];
void __cdecl on_cam_seq(uint32_t pName);
extern const uint8_t P_CAMREQ[9];
extern volatile int g_camSetOk;
int bm_state(void);
extern volatile DWORD g_camWatch;
extern volatile int   g_camDone;
void cam_disarm(void);
void __cdecl on_cam_request(uint32_t ecx, uint32_t retAddr, uint32_t* pReqSlot, uint32_t* unused);
extern volatile DWORD g_posSnap;
extern volatile DWORD g_camSnapAt;
extern volatile int   g_posN;
extern const uint8_t P_HASCAM[6];
void __cdecl on_has_cam(uint32_t ecx);
extern volatile int g_gatesDone;
void cam_pump(uint32_t btsc, DWORD now);
#ifdef FF13_DIAG
void bm_timeline(DWORD now);
#else
#define bm_timeline(now) ((void)0)
#endif
#ifdef FF13_DIAG
void diag_vm_census(const char* when);
void diag_ptr_seen(uint32_t p);
#endif
void __cdecl on_encount_check(uint32_t ecx);
extern volatile uint32_t g_encIdSeq;
extern volatile DWORD    g_touchAt;
extern volatile uint32_t g_touchSeq;
void __cdecl on_encount_id(uint32_t id);
extern const uint8_t P_BGM[9];
extern const uint8_t P_BGMLAZY[9];
extern const uint8_t P_BANKPLAY[9];
extern const uint8_t P_ASSERT[9];
int bgm_current(char* out /* BGM_NAME_MAX+1 */);
int bgm_lazy_play(const char* name);
void bgm_stop(const char* name, float fade);
extern char g_bgmFieldNow[BGM_NAME_MAX + 1];
extern char g_bgmMine[BGM_NAME_MAX + 1];
void bgm_zone_refresh(void);
const char* bgm_pick_track(uint32_t btsc, const char** origin);
void __cdecl on_bgm_play(uint32_t ecx, uint32_t retAddr, uint32_t* pName, uint32_t* unused);
void __cdecl on_bgm_lazyplay(uint32_t ecx, uint32_t retAddr, uint32_t* pName, uint32_t* pFade);
void* install_encscene_cave(uintptr_t hookAddr, void* handler);

// ---- battle picker, kill mode and the game-over path (srp_picker.c) ----
extern volatile uint32_t g_bsStarted;
extern volatile int g_bsTouchDue;         // a touch hand-off is staged or in flight
void blackout_hold_on(void);
int blackout_on(void);
int is_boss_scene(uint32_t btsc);
void boss_learn(uint32_t btsc, const char* arena);
const char* bgm_event_track(uint32_t btsc);
const char* bgm_zone_track(uint32_t zone);
const ff13_btsc_info* btsc_info(uint32_t btsc);
void btsc_hand(uint32_t btsc, int mode, const unsigned short** jp, const char** en, unsigned* order, const unsigned short** tag, int* kind);
void zb_save(void);
void zb_load(void);
void zb_learn(uint32_t zone, uint32_t band, uint32_t touch);
extern volatile int      g_bsOpen;
extern volatile int      g_bsReq;
extern volatile int      g_bsRememberDue;
extern volatile int      g_bsCount;
extern volatile int      g_bsSel;
extern uint32_t          g_bsList[BS_MENU_MAX];
extern uint8_t           g_bsVar[BS_MENU_MAX];
extern UINT g_bsVk;
extern UINT g_bsUpVk;
extern UINT g_bsDownVk;
extern UINT g_bsLeftVk;
extern UINT g_bsRightVk;
void zb_remember_sel(uint32_t zone, uint32_t btsc, int mode);
// The battle actor table: the index into it IS the actor id, and the entries are charas.
#define RVA_BTL_ACTORS   0x23FD20C   // VA 0x27FD20C -- the battle actor manager (0x4ED600)
#define ACTOR_SLOTS      0x13        // the bound 0x4ED600 itself checks the id against
// Chara::getChainObject (0x481460) is `return this + 0x20A8`.
#define OFF_CHARA_CHAIN  0x20A8
#define OFF_CHAIN_STATE  0x00        // 0 = nothing running, 1/2 = a chain, 3 = broken
#define OFF_CHAIN_LEFT   0x18        // milliseconds left, counted down on the battle clock
#define CHAIN_BROKEN     3
// Raw ATB, a plain signed int. Chara::getAtbStages (0x46E900) is (int)(raw/100.0 + 0.1), and that
// stage count -- not the raw value -- is what 0x548EA0 compares an ability's cost against.
#define OFF_CHARA_ATB    0x2A0
uint32_t* actor_table(void);
extern const uint8_t P_SPECREPL[9];
extern const uint8_t P_ENCF4[7];
void preds_verify(void);
void __cdecl on_enc_field4(uint32_t ecx);
extern const uint8_t P_RESOLVETGT[9];
void __cdecl on_cond_target(uint32_t ecx, uint32_t kind, uint32_t* pActorId, uint32_t* pOut);
extern const uint8_t P_ABILLABEL[9];
void __cdecl on_ability_label(uint32_t ecx, uint32_t label);
extern const uint8_t P_AIAFTER[6];
// The condition dispatcher's join. Hooked so the enemy's own roll for the optional extra attack
// can be overruled -- see on_ai_cond.
#define RVA_AICOND    0x158031       // VA 0x558031
extern const uint8_t P_AICOND[7];
void __cdecl on_ai_cond(uint32_t esp);
void extra_force_reset(void);
void extra_force_done(void);
#define RVA_AISTATE   0x149D40       // VA 0x549D40 -- the AI state setter
extern const uint8_t P_AISTATE[6];
void __cdecl on_ai_state(uint32_t ecx, uint32_t state, uint32_t* pRet, uint32_t* pUp);
// The store inside apiAiSetEnemyTarget's worker where the drawn target handle reaches the AI
// block.
#define RVA_AITARGET  0x14BE0F       // VA 0x54BE0F
extern const uint8_t P_AITARGET[11];
#define RVA_AIQUEUE   0x1498A0       // VA 0x5498A0 -- the AI's decision queues its attack
extern const uint8_t P_AIQUEUE[9];
void __cdecl on_ai_queue(uint32_t esp);
#define RVA_AIRUN     0x148EA0       // VA 0x548EA0 -- carries out the AI's decision
extern const uint8_t P_AIRUN[9];
void __cdecl on_ai_run(uint32_t ecx);
#define RVA_AIREAIM   0x149370       // VA 0x549370 -- ...its last re-read of the target before the attack runs
extern const uint8_t P_AIREAIM[9];
void __cdecl on_ai_reaim(uint32_t esp);
#ifdef FF13_DIAG
void aim_watch_hit(uint32_t actor);
#define RVA_SETTGT 0x7CB60           // VA 0x47CB60 -- chara->setTarget(handle, 0)
extern const uint8_t P_SETTGT[6];
void __cdecl on_set_target(uint32_t ecx, uint32_t handle, uint32_t* pRet, uint32_t* pUnused);
#endif
void __cdecl on_ai_target(uint32_t esp);
// The status-effect roll, one fork before it draws.
#define RVA_STATROLL  0xA5597        // VA 0x4A5597
#define RVA_STATLAND  0xA55D1        // VA 0x4A55D1: the store the roll's compare falls through to,
                                     // and the only "it landed" the dice can reach
extern const uint8_t P_STATROLL[7];
void __cdecl on_status_roll(uint32_t esp);
extern const uint8_t P_STATLAND[7];
void __cdecl on_status_land(uint32_t esp);
// The hit-reaction resist roll, the same shape one layer out.
#define RVA_LAUNCH    0xA7E79        // VA 0x4A7E79
#define RVA_LAUNCHRES 0xA7ECD        // VA 0x4A7ECD: the store that downgrades the reaction, and the
                                     // only "it did not go up" the dice can reach
extern const uint8_t P_LAUNCH[8];
void __cdecl on_launch_roll(uint32_t esp);
extern const uint8_t P_LAUNCHRES[11];
void __cdecl on_launch_resist(uint32_t esp);
void __cdecl on_ai_afterexec(uint32_t ctx);
void __cdecl on_spec_replace(uint32_t ecx, uint32_t name);
extern volatile int      g_homeHave;
extern volatile int      g_homeDue;
extern float g_homeX, g_homeY, g_homeZ;
#define HOME_CELLS_MAX 16
extern int g_homeCells[HOME_CELLS_MAX][2];
extern int g_homeCellsN;
int  save_capture_resident_cells(int out[][2], int maxn);
void save_mapset_census(const char* tag);
int  save_current_mapset(char out[17]);
void save_arm_reconcile(const int cells[][2], int n);
void save_arm_reconcile_sticky(const int cells[][2], int n, DWORD ms);
extern const uint8_t P_SENDTRIG[9];
extern volatile int   g_trigOk;
extern volatile int   g_workerHowOk;
extern volatile int g_goOk;
extern const uint8_t P_GOMENU[9];
extern const uint8_t P_GOSE[GO_SE_LEN];
extern volatile int g_goSeOk;
void __cdecl on_gameover_menu(uint32_t ctrl);
extern const uint8_t P_WORKHOW[5];
void __cdecl on_worker_how(uint32_t esp);
extern const uint8_t P_WORKIN[9];
extern const uint8_t P_WORKARM[5];
void __cdecl on_worker_in(uint32_t esp);
void __cdecl on_worker_arm(uint32_t esp);
extern const uint8_t P_PLACEJOIN[5];
extern volatile int g_bossGotMatrix;
void __cdecl on_place_join(uint32_t esp);
extern const uint8_t P_ITEMTAKE [6];
extern const uint8_t P_ITEMCOUNT[7];
extern const uint8_t P_ITEMGIVE [7];
extern const uint8_t P_TPGET[8];
extern const uint8_t P_BTRES_CLEAR[14];
extern const uint8_t P_TPSET[12];
extern const uint8_t P_LOCKENTER[9];
extern const uint8_t P_LOCKLEAVE[7];
extern volatile int   g_itemOk;
extern volatile int   g_tpOk;
extern volatile int   g_btresOk;
void __cdecl on_item_take(uint32_t esp);
extern volatile int g_libraOk;
extern const uint8_t P_LOADSHOW[6];
extern const uint8_t P_LOADHIDE[10];
void __cdecl on_load_show(uint32_t mode);
void __cdecl on_load_hide(uint32_t ecx);
extern const uint8_t P_ENCREFUSE[5];
void __cdecl on_encount_refused(uint32_t ecx);
extern const uint8_t P_ACTIVATE[9];
extern const uint8_t P_ACTGATE[7];
extern volatile int g_iwArmed;
extern volatile int g_iwInstProbe, g_iwInstGate;
void __cdecl on_activate_probe(uint32_t esp);
void __cdecl on_activate_gate(uint32_t esp);
const char* kill_mode_name(int m);
const char* kill_mode_name_en(int m);
void kill_mode_apply(int mode, int announce);
void bs_pump(void);

// ---- warp cluster and the battle camera (srp_warp.c) ----
extern const uint8_t P_TP6[6];
void __cdecl on_getworldpos(uint32_t ecx);
extern volatile uint32_t g_warpActive;
extern volatile float g_ovrX, g_ovrY, g_ovrZ;
extern volatile float g_refX, g_refY, g_refZ;
extern volatile uint32_t g_leaderIdx;
extern volatile uint32_t g_warpMoved, g_warpSkip;
void __cdecl on_commit(uint32_t ecx, float* src);
int field_party_pos(float* x, float* y, float* z);
void arena_clear_flags(void);
extern const uint8_t P_CAMREC[6];
extern volatile int g_camRecOk;
void camera_follow_carry(float dx, float dy, float dz);
void camera_recentre_due(void);
void camera_follow_hold(void);
void field_camera_recenter(void);
extern const uint8_t P_WIDESHOT[8];
void __cdecl on_wide_shot(uint32_t esp);
extern const uint8_t P_GETCAM[6];
extern volatile int   g_getCamOk;
void battle_cam_watch(void);
void cam_control_dump(const char* when);
void arena_probe_formation(void);
void arena_recentre_reset(void);
void arena_hold(void);
void arena_collect(uint32_t ecx, uint32_t slot, const float* pos);
int warp_flag_cluster(float cx, float cy, float cz);
void hold_still_begin(void);
void hold_still_end(void);
extern volatile int   g_warpPrompt;
extern char           g_warpPromptMsg[288];
extern volatile DWORD g_warpPromptUntil;
void warp_load(void);
void warp_record(void);
void home_warp_back(void);
void warp_ask(void);
void warp_confirm(void);
void warp_cancel(void);

// ---- save anywhere (srp_save.c) ----
// These three live here but are written by config_load() in srp_core.c.
extern wchar_t g_listMark[9];
extern char g_saveDir[MAX_PATH];
extern volatile int g_keepNote;
void save_sidecars_load(void);
void save_install_hooks(void);
void save_poll_tick(DWORD now);
// save-list sort menu: srp_save.c owns the state and the work, dllmain.c polls the keys and
// holds the DirectInput gate while it is open, srp_overlay.c draws it.
#define SS_MODES 8
// The order the list is built in: state.dat "order". The comparator the game's qsort is handed
// reads it, so a change applies the next time a list opens and nothing on disk moves.
#define SS_ORDER_MODES 4         // 0 date (the game's own), 1 story asc, 2 story desc, 3 by number
extern volatile int g_sortOrder;
// Menu order, and g_ssSel indexes it.
#define SS_FILTER 0
#define SS_CAT_PICK 1
#define SS_CAT_NEW 2
#define SS_CAT_NEW_ALL 3
#define SS_CAT_RENAME 4
#define SS_CAT_DEL 5
#define SS_ORDER 6               // a value row: the arrows pick, confirm does nothing
#define SS_RENUM 7
extern volatile int g_ssOpen;
extern volatile int g_ssSel;
extern volatile int g_filterChapter;     // 0 = every chapter
void ss_menu_open(void);
void save_order_cycle(int dir);
// 1 = the retired real-date sidecar still holds rows, so a sort really had been applied under the
// build that re-stamped files. Read before save_sidecars_load migrates and renames it.
int  dates_sidecar_has_rows(void);
int  save_renumber_apply(void);   // 0 = it only asked; the menu stays up for the second confirm
int  renum_armed_n(void);         // saves the asked-for run would move; 0 = not asking
void filter_apply(int chapter);
void filter_menu_refresh(void);
int  filter_cycle(int dir);
int  filter_count_of(int chapter);
void filter_restore(void);
// 1 = Steam is syncing this game's saves, 0 = it is not, -1 = Steam would not say (callers
// treat -1 as 0).
int steam_cloud_on(void);
#define LABEL_PASTE_NO_ROW    -1
#define LABEL_PASTE_NO_TEXT   -2
#define LABEL_PASTE_BAD       -3
#define LABEL_PASTE_NO_SAVE   -4        // the row is the save screen's "make a new save" one
#define LABEL_PASTE_NO_WINDOW -5
#define LABEL_TEXT_MAX        24
extern volatile int g_labelEdit;
extern volatile int g_labelEditSlot;
const wchar_t* label_edit_text(void);
// What is wrong with the editor's text right now (0 = nothing). INPUT comes from typing and the
// next keystroke clears it; the other two come from a refused commit and stand until the name
// changes -- the editor stays open on them.
#define EDIT_BAD_INPUT  1
#define EDIT_BAD_EXISTS 2
#define EDIT_BAD_NAME   3
int  label_edit_bad(void);
int  label_edit_ready(void);      // is the save list showing a row right now?
int  label_edit_open(void);
void label_edit_finish(int commit);
int  label_edit_tick(char* what, size_t n);
// What the on-screen text editor is being used for; the label editor has to stay mode 0, so a
// caller that only knows about labels keeps working.
#define EDIT_LABEL      0
#define EDIT_CAT_NEW    1
#define EDIT_CAT_RENAME 2
#define EDIT_CAT_SEND   3         // naming the category a save is being sent to
#define EDIT_CAT_NEW_ALL 4        // ...a new category seeded with EVERY save, not just the newest
int  edit_mode(void);
// ---- save categories: the whole save directory, parked in and out of
// <save dir>\categories\<name>\.
#define CATNAME_MAX 24            // characters; the name is used as a directory name
#define CAT_MAX 16
void cat_list_refresh(void);
int  cat_cycle(int dir);
int  cat_count(void);
const char* cat_name_cur(void);   // the category in use, UTF-8
const char* cat_name_sel(void);   // ...the delete target likewise, or NULL when none is picked
const char* cat_name_at(int i);   // parked category i likewise, or NULL
const char* cat_pick(void);       // that target as the filesystem spells it
int  cat_switch(const char* target);
// The switch row's own pick, "none" one of its stops. Picking moves nothing; cat_sw_go switches
// to it (Enter, leaving the row, closing the menu) and clears it either way. 1 = switched.
int  cat_sw_cycle(int dir);
const char* cat_sw_name_sel(void);
int  cat_sw_go(void);
// The delete row's hold: the first one arms the confirm, the next one deletes. It stands itself
// down (save_poll_tick), so callers only ever press it.
int  cat_del_hold(void);
int  cat_del_armed(void);         // 1 = that row is waiting for the second hold
int  cat_edit_open(int mode);     // EDIT_CAT_NEW / EDIT_CAT_NEW_ALL / EDIT_CAT_RENAME
void cat_edit_tick(void);

// ---- the save list's chooser ------------------------------------------------------------------
// Row order, and g_lselSel indexes it.
#define LSEL_NOTE 0
#define LSEL_COPY 1
#define LSEL_MOVE 2
#define LSEL_MARK 3
#define LSEL_DEL  4
#define LSEL_ROWS 5
extern volatile int g_lselOpen;
extern volatile int g_lselSel;
extern volatile int g_lselSlot;
int  range_anchor(void);        // -1 = no range mark, else the slot it starts from
int  range_shown_count(void);   // members as of the last cursor move, for the row text
int  range_del_ready(void);     // 0 = drawn but cannot run: no mark, or one that will not resolve
int  range_del_armed(void);     // 1 = it is waiting for the second hold
int  range_del_count(void);     // ...over this many saves, counted when it armed
int  lsel_open(void);
void lsel_close(void);
void lsel_follow_now(void);       // sync the menu to the row the game shows; call before acting
void menu_screen_tick(void);      // closes our menus when the game changes screen under them
int  lsel_go(void);
void menu_gate_release(void);     // dllmain.c: lift the gate once no gated key is still down
void save_keys_step(void);        // advance the save loop's edge states without acting on them

// ---- send the save under the list's cursor to another category --------------------------------
extern volatile int g_sendDest;   // 0..cat_count()-1 = a parked category, cat_count() = a new one
int  send_cycle(int dir);
const char* send_dest_name(void); // where to, UTF-8, or NULL for the new-category stop

// ---- overlay services (srp_overlay.c) ----
// Text colors, kept as one block so the whole scheme is one edit; opaque ARGB.
#define OVL_GAMESPEED  0xFFD2FA69   // GAME SPEED     : lime-green
#define OVL_ONEHIT     0xFFFF3B30   // ONE-HIT KILL   : red-orange (same as INSTANT WIN)
#define OVL_SUPPRESS   0xFFC772EB   // ENEMY SUPPRESS : violet
#define OVL_BATTLE     0xFFFFFFFF   // battle overlay : white (the figures themselves)
#define OVL_BATTLE_HEAD 0xFFC4CFCF  // ...and grey for the TIME / TARGET group headings
#define OVL_SWAP       0xFFF5A623   // BATTLE SWAP    : amber
#define OVL_INSTANT    0xFFFF3B30   // INSTANT WIN    : red-orange
#define OVL_WARP_PROMPT  0xFF86FDD2   // warp prompt: mint-green
#define OVL_FLASH   0xFFC4CFCF   // status flash   : grey (same value as OVL_FLASH_OFF; separate
                                    // channels so they CAN differ, not because they must)
#define OVL_PICK_SEL   0xFFFFF566   // picker: the row you are on
#define OVL_MENU       0xFF9AD3FE   // save-data menus and the settings screen: the rows you can pick
#define OVL_TEXT       0xFFFFFFFF   // ...and their lines that are not rows: key help, explanations (the picker's too)
#define OVL_MENU_SEL   0xFF9FFDF9   // ...and the row the cursor is on (the editor's line, too)
#define OVL_MENU_OFF   0xFF6489A5   // ...and a row that is drawn but cannot run right now
#define OVL_MENU_BAND  0x3366B8FF   // the quad behind the selected row
#define OVL_LINE_H     24           // one overlay row
#define OVL_TOP_DEF    34           // [overlay] top_px: the first row's top, clear of corner FPS counters
#define OVL_TOP_MAX    400
#define OVL_TOP_STEP   8
void flash_say_c(const char* s, DWORD col);
void flash_say(const char* s);
void vk_label(UINT b, char* out, size_t n);
void tidy_ok_phrase(char* out, size_t n);
int overlay_in_battle(void);
int read_via_ptr(unsigned ptrRva, int off);
void overlay_init(void);
// Render thread only, inside a draw pass: the pass's device and the ASCII font.
int  ovl_char_w(void);                    // ASCII cell width in pixels; 0 = no font yet
int  ovl_text_w(const char* utf8);        // drawn width in pixels, measured as ovl_say draws
// Pads v with spaces to the widest of cands, so what is drawn after it (a menu row's right arrow)
// stays put as v changes.
void ovl_fit(char* v, size_t n, const char* const* cands, int cn);
int  ovl_space(char* out, size_t n, int px);  // blanks about px wide; returns the px left over
void ovl_say(int x, int y, const char* utf8, DWORD argb);
int  ovl_say_wrap(int x, int contX, int y, const char* utf8, DWORD argb, int lineH);
int  ovl_wrap_count(int x, int contX, const char* utf8);   // the rows ovl_say_wrap would take
// The rows utf8 takes with `tail` put after its last row, or on a row of its own at contX when it
// does not fit there; *tailX / *tailRow (0-based) say where to draw the tail.
int  ovl_wrap_tail(int x, int contX, const char* utf8, const char* tail, int* tailX, int* tailRow);
void ovl_backdrop(int x0, int y0, int x1, int y1, DWORD argb);
// The band behind a selected row of `rows` rows at y: drawn after the backdrop, before any text.
void ovl_menu_band(int y, int rows);

// ---- the hotkeys-and-settings screen (srp_setup.c) ----
// Everything but the two draw calls runs on the hotkey worker; the caller lifts the input gate
// (menu_gate_release) after a close, as with the tidy menus.
void setup_open(void);                    // arms the input gate
void setup_close(void);
int  setup_is_open(void);
void setup_move(int dy);                  // up/down, wrapping, over the heading rows
void setup_cycle(int dir);                // left/right: the selected row's value or sub-cursor
void setup_ok(void);                      // confirm: start a key capture, or toggle
int  setup_capturing(void);               // a key row is waiting for its new key
void setup_capture_tick(void);            // every tick while capturing
// 1 while the key that ended the last capture is still down: the caller's gesture state still
// tracks the OLD bindings, so a key just made a menu key would otherwise fire as one.
int  setup_settling(void);
int  setup_visible(void);                 // render thread: draw it this pass? (the lapse guard)
int  setup_draw(int x, int y);            // render thread: draws the block, returns the y below

#endif // SRP_INTERNAL_H
