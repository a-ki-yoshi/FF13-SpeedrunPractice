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
// ============================ DIAGNOSTICS (-DFF13_DIAG) ======================

// applyHPDelta is very high-frequency: the seen-set holds it to one line per distinct actor.
#define SEEN_MAX 512
static volatile uint32_t g_seen[SEEN_MAX];
volatile int g_seenCount = 0;

static int seen_add_if_new(uint32_t ptr) {
    int cnt = g_seenCount;
    for (int i = 0; i < cnt && i < SEEN_MAX; i++)
        if (g_seen[i] == ptr) return 0;
    if (cnt < SEEN_MAX) { g_seen[cnt] = ptr; g_seenCount = cnt + 1; }
    return 1;
}

// Installed only when g_logLevel>=2, via ff13_install_ecx_hook at the applyHPDelta entry
// (ecx = this = combat actor). Runs on the game thread: strictly read-only, every read guarded.
// guard324 is logged raw so the party=0 / enemy!=0 split can be spot-checked.
void __cdecl on_apply_hp_delta(uint32_t thisActor) {
    if (g_logLevel < 2) return;                    // verbose diagnostic only
    if (!thisActor) return;
    void* a = (void*)(uintptr_t)thisActor;
    if (IsBadReadPtr(a, ACTOR_SPAN)) return;       // unreadable -> skip silently
    if (!seen_add_if_new(thisActor)) return;       // already logged this window

    int      hp    = *(int*)     ((char*)a + OFF_HP);
    int      maxhp = *(int*)     ((char*)a + OFF_MAXHP);
    uint32_t g324  = *(uint32_t*)((char*)a + OFF_GUARD324);
    int      def   = (*(uint8_t*)((char*)a + OFF_DEFEATED) & 0x02) ? 1 : 0;
    ff13logv("actor=0x%08X hp=%d/%d guard324=0x%X defeated=%d\n", thisActor, hp, maxhp, g324, def);
}
#endif // FF13_DIAG

// ---- the instant-win raw code cave (installed at 0x46E85B) ----
// At the injection point eax = this actor, ecx = newHP. The cave clobbers ONLY ecx, and only in
// the armed+enemy case where ecx is exactly the 0 we mean to write; pushfd/popfd save EFLAGS and
// keep esp balanced. No other register, and no memory other than [eax+0x290], is touched.
void* g_onehitCave = NULL;

void* install_onehit_cave(uintptr_t hookAddr, const volatile void* armedFlag,
                                    uintptr_t returnAddr) {
    static const uint8_t PROLOGUE[6] = {0x89,0x88,0x90,0x02,0x00,0x00}; // mov [eax+0x290],ecx
    if (memcmp((const void*)hookAddr, PROLOGUE, 6) != 0) return NULL;   // wrong bytes -> abort

    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) return NULL;

    // 33 bytes; the two `je do_write` rel8 targets are hardcoded against do_write at cave +21.
    uint8_t code[] = {
        /* +0  */ 0x9C,                               // pushfd
        /* +1  */ 0x80, 0x3D, 0,0,0,0, 0x00,          // cmp byte ptr [g_armed], 0   (abs @[3..6])
        /* +8  */ 0x74, 0x0B,                         // je  do_write (+21): 21-(10) = 0x0B
        /* +10 */ 0x83, 0xB8, 0x24,0x03,0x00,0x00, 0x00, // cmp dword ptr [eax+0x324], 0
        /* +17 */ 0x74, 0x02,                         // je  do_write (+21): 21-(19) = 0x02
        /* +19 */ 0x33, 0xC9,                         // xor ecx, ecx
        /* +21 */ 0x9D,                               // do_write: popfd
        /* +22 */ 0x89, 0x88, 0x90,0x02,0x00,0x00,    // mov [eax+0x290], ecx  (original instr)
        /* +28 */ 0xE9, 0,0,0,0                        // jmp returnAddr        (rel32 @[29..32])
    };
    uint32_t armedAbs = (uint32_t)(uintptr_t)armedFlag;
    memcpy(&code[3], &armedAbs, 4);
    int32_t relBack = (int32_t)(returnAddr - ((uintptr_t)cave + 33));
    memcpy(&code[29], &relBack, 4);
    memcpy(cave, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(code));

    DWORD old;
    if (!VirtualProtect((void*)hookAddr, 6, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return NULL;
    }
    uint8_t* h = (uint8_t*)hookAddr;
    h[0] = 0xE9;
    int32_t relHook = (int32_t)((uintptr_t)cave - (hookAddr + 5));
    memcpy(h + 1, &relHook, 4);
    h[5] = 0x90;                                   // NOP padding for the 6th prologue byte
    VirtualProtect((void*)hookAddr, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)hookAddr, 6);
    return cave;
}


// ---- GAME-SPEED MODE: scale the per-frame delta so the game advances faster ----
// Hooked at the app-clock tick 0x6AA480, whose first stack arg is `int* frameDeltaMs`; multiplying
// it before the tick body runs makes every system that reads the frame dt advance further. Keep the
// factor modest (high ones can desync audio/streaming), and note that while it is ON the in-game
// timers are warped, so any IGT/real-time readout is only accurate with it OFF.
// ---- ...and the ATB gauges with it -----------------------------------------------------------
// The frame delta does NOT reach the ATB gauges (measured on hardware), so game-speed also drives
// the game's own ATB-only lever: battle command 35 `setAtbSpeed`, whose only consumers are the two
// BattleChara ATB accessors -- animation, camera and enemy motion never read it.
//   [0x027FD208]  the battle object (the same one the GAME timer is read from)
//   [bs + 0x4F4]  float, the multiplier; 1.0 is normal
//   0x004F5220(this, float, int)  __thiscall setter; the int arm also runs 0x49D1A0 on [bs+8]
#define RVA_BS_ATB_PTR   0x23FD208    // VA 0x27FD208
#define OFF_BS_ATBSPEED  0x4F4
const uint8_t P_SETATB[8] = {0x55,0x8B,0xEC,0x83,0xEC,0x14,0x89,0x4D};
volatile int   g_atbOk = 0;            // the setter is where it should be
static volatile float g_atbSaid = -1.0f;      // last value the log named

// Every frame while a fight is up. The value is READ back rather than remembered, so it follows the
// game resetting it between fights, and a gauge the game itself has stalled is left alone.
static void atb_speed_pump(void) {
    if (!g_atbOk || !g_base) return;
    uintptr_t p = g_base + RVA_BS_ATB_PTR;
    if (IsBadReadPtr((void*)p, 4)) return;
    uint32_t bs = *(uint32_t*)p;
    if (!bs || IsBadWritePtr((void*)(uintptr_t)(bs + OFF_BS_ATBSPEED), 4)) return;
    float cur = *(float*)(uintptr_t)(bs + OFF_BS_ATBSPEED);
    if (cur == 0.0f) return;                          // the game is holding the gauges itself
    float want = (g_gsEnabled && g_gsFactor > 0.0) ? (float)g_gsFactor : 1.0f;
    if (cur == want) return;
    ((void (__thiscall *)(void*, float, int))(uintptr_t)(g_base + RVA_SETATBSPEED))
        ((void*)(uintptr_t)bs, want, 0);
    if (want != g_atbSaid) {
        g_atbSaid = want;
        ff13log("[FF13-SRP] game-speed: ATB gauge speed %.2fx (was %.2fx)\n",
                 (double)want, (double)cur);
    }
}

const uint8_t P_FRAMETICK[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x84,0x00,0x00,0x00}; // push ebp;mov ebp,esp;sub esp,0x84
_Static_assert(sizeof(P_FRAMETICK) == 9, "P_FRAMETICK size");

// Installed via ff13_install_stackarg_hook with stackOff=4, so pDelta is [esp+4] = frameDeltaMs.
// Runs on the game thread once per frame, before the tick body.
void __cdecl on_frame_tick(uint32_t pDelta) {
    // The picker's game-thread work rides on this tick: it is the one that fires every frame
    // unconditionally (the GetWorldPos hook only runs while something is moving).
    bs_pump();
    battle_cam_watch();
    atb_speed_pump();          // ...and the gauges, which the frame delta does not reach
    if (!g_gsEnabled || !pDelta) return;
    if (IsBadWritePtr((void*)(uintptr_t)pDelta, 4)) return;
    int v = *(int*)(uintptr_t)pDelta;
    if (v > 0) *(int*)(uintptr_t)pDelta = (int)((double)v * g_gsFactor);
}

#ifdef FF13_DIAG
// ---- direct observation: hook the AI-territory state setter and log its args on each call ----
// setAiTerritoryStateByTerritoryName's C++ backend is setter(int id, int state) @ 0x5B21C0 (cdecl).
// The shared stackarg hook passes only ONE [esp+stackOff] value, so only id (stackOff=4) is taken.
const uint8_t P_SETTER[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x18}; // push ebp;mov ebp,esp;sub esp,0x18
_Static_assert(sizeof(P_SETTER) == 6, "P_SETTER size");

static volatile int g_setterLogN = 0;               // cap total lines so a hot call site can't flood
void __cdecl on_terr_setter(uint32_t id) {
    if (g_setterLogN >= 400) return;                // safety cap
    g_setterLogN++;
    ff13logv("[FF13-SRP] AiTerritoryState setter: id=%d (#%d)\n", (int)id, g_setterLogN);
}
#endif // FF13_DIAG

// ---- group-activation suppressor: enemy-group suppression (THE feature) ----
// Only `gr_mon_` may match: `gr_fa_` is treasure/gimmicks/doors and `gr_npc_` is NPCs, and the
// savepoint restore path re-pops all three, so a bare "gr_" would take the chest and the
// quest-giver with the enemies. Not ini-settable: there is one useful answer.
volatile unsigned char g_suppressEnabled = 0; // default OFF; toggle with g_supVk before loading
static volatile int g_actLogN = 0;                   // cap group-name logging

// __cdecl handler: ebpVal = the game function's ebp. The group name char* is at [ebp-4]. Returns 1
// to suppress (divert to the skip path), 0 to activate normally. STRICTLY reads; guarded.
int __cdecl should_skip_activate(void* ebpVal) {
    if (IsBadReadPtr((char*)ebpVal - 4, 4)) return 0;
    const char* g = *(const char* const*)((char*)ebpVal - 4);
    if (!g || IsBadReadPtr((void*)g, 16)) return 0;
    int match = (strncmp(g, SUPPRESS_PREFIX, sizeof(SUPPRESS_PREFIX) - 1) == 0);
    if (g_actLogN < 300) {
        g_actLogN++;
        ff13logv("[FF13-SRP] activateArea group=\"%.31s\"%s\n", g,
                 match ? (g_suppressEnabled ? " -> SUPPRESSED" : " -> (match; suppress OFF)") : "");
    }
    return (match && g_suppressEnabled) ? 1 : 0;
}

// ---- THE despawn suppressor: hooks activateAreaWithHide_l (0xBE0230) at 0xBE029D, where it does
// `cmp [ebp-4],0 ; je 0xBE02E0` -- a NULL group char* makes it skip the spawn. A match diverts to
// that same built-in skip path: contAddr 0xBE02A3 (the not-NULL continue), skipAddr 0xBE02E0.
void* g_whCave = NULL;
void* install_withhide_cave(uintptr_t hookAddr, void* handler,
                                   uintptr_t contAddr, uintptr_t skipAddr) {
    static const uint8_t EXPECT[6] = {0x83,0x7D,0xFC,0x00, 0x74,0x3D}; // cmp [ebp-4],0 ; je 0xBE02E0
    if (memcmp((const void*)hookAddr, EXPECT, 6) != 0) return NULL;
    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return NULL;
    uint8_t code[] = {
        /*+0x00*/ 0x55,                         // push ebp
        /*+0x01*/ 0xE8, 0,0,0,0,                // call handler          (rel32 @+2)
        /*+0x06*/ 0x83, 0xC4, 0x04,             // add esp, 4
        /*+0x09*/ 0x84, 0xC0,                   // test al, al
        /*+0x0B*/ 0x75, 0x0B,                   // jne do_skip (+0x18)
        /*+0x0D*/ 0x83, 0x7D, 0xFC, 0x00,       // cmp [ebp-4], 0        (relocated)
        /*+0x11*/ 0x74, 0x05,                   // je  do_skip (+0x18)   (relocated)
        /*+0x13*/ 0xE9, 0,0,0,0,                // jmp contAddr          (rel32 @+0x14)
        /*+0x18*/ 0xE9, 0,0,0,0                 // do_skip: jmp skipAddr (rel32 @+0x19)
    };
    int32_t relCall = (int32_t)((uintptr_t)handler - ((uintptr_t)cave + 0x06));
    int32_t relCont = (int32_t)(contAddr           - ((uintptr_t)cave + 0x18));
    int32_t relSkip = (int32_t)(skipAddr           - ((uintptr_t)cave + 0x1D));
    memcpy(&code[0x02], &relCall, 4);
    memcpy(&code[0x14], &relCont, 4);
    memcpy(&code[0x19], &relSkip, 4);
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
    h[5] = 0x90;                                 // pad the 1 leftover byte
    VirtualProtect((void*)hookAddr, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)hookAddr, 6);
    return cave;
}

#ifdef FF13_DIAG
// ---- activate-variant ENTRY probes (diagnostic): find WHICH native the restore script calls ----
// Scene::activate*Area* natives share the VM-native ABI: ctx = [esp+4] at entry, the operand stack
// pointer is [ctx+0x114], each operand is 8 bytes {tag@+0, value@+4} and the value is the group
// name char*. READ-ONLY. Installed via ff13_install_stackarg_hook(stackOff=4 -> ctx).
static void probe_activate(uint32_t ctx, const char* label) {
    if (g_actLogN >= 300) return;
    if (IsBadReadPtr((void*)(uintptr_t)(ctx + 0x114), 4)) return;
    uint32_t osp = *(uint32_t*)(uintptr_t)(ctx + 0x114);
    char line[192]; int n = 0;
    n += snprintf(line + n, sizeof(line) - n, "[FF13-SRP] %s ctx=0x%08X:", label, ctx);
    for (int off = 4; off <= 0x14 && n < (int)sizeof(line) - 40; off += 8) {
        const char* s = "?";
        if (!IsBadReadPtr((void*)(uintptr_t)(osp + off), 4)) {
            uint32_t v = *(uint32_t*)(uintptr_t)(osp + off);
            if (v && !IsBadReadPtr((void*)(uintptr_t)v, 16)) s = (const char*)(uintptr_t)v;
        }
        n += snprintf(line + n, sizeof(line) - n, " [+%x]=\"%.24s\"", off, s);
    }
    g_actLogN++;
    ff13logv("%s\n", line);
}
void __cdecl on_probe_actArea (uint32_t ctx) { probe_activate(ctx, "actArea_l");     }
void __cdecl on_probe_withHide(uint32_t ctx) { probe_activate(ctx, "actWithHide_l");  }
void __cdecl on_probe_ignoreFA(uint32_t ctx) { probe_activate(ctx, "actIgnoreFA_l");  }
void __cdecl on_probe_wait    (uint32_t ctx) { probe_activate(ctx, "waitAct");        }
void __cdecl on_probe_poll    (uint32_t ctx) { probe_activate(ctx, "pollAct");        }
void __cdecl on_probe_deact   (uint32_t ctx) { probe_activate(ctx, "deactivate");     }
#endif // FF13_DIAG

