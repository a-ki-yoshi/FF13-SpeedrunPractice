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
// applyHPDelta entry prologue: 55 8B EC 83 EC 20 = push ebp / mov ebp,esp / sub esp,0x20.
// Length MUST be 6: the `83 EC 20` straddles the 5-byte jmp boundary.
static const uint8_t P_APPLYHP[] = {0x55,0x8B,0xEC,0x83,0xEC,0x20};
#define RVA_APPLYHP 0x6E6B0   // VA 0x46E6B0 - imagebase 0x400000 (diagnostic read-hook entry)
#endif // FF13_DIAG

// Instant-win injection site. The 6-byte AOB `89 88 90 02 00 00` is NOT unique in .text, hence the
// 14-byte unique signature waited on here, with the injection at +6.
static const uint8_t P_OHK_SIG[] = {0x8B,0x45,0xE8,0x8B,0x4D,0xFC,   // mov eax,[ebp-18]; mov ecx,[ebp-4]
                                   0x89,0x88,0x90,0x02,0x00,0x00,    // mov [eax+0x290],ecx  <-- inject +6
                                   0x6A,0x00};                       // push 0
#define RVA_OHK_SIG    0x6E855   // VA 0x46E855 - imagebase 0x400000 (unique 14-byte signature)
#define RVA_OHK_INJECT 0x6E85B   // VA 0x46E85B = signature + 6 (the mov [eax+0x290],ecx)

// The Steam build is SteamStub-DRM wrapped: at DllMain the .text is still encrypted, so EVERY site
// below is waited on (wait_for_bytes) until the stub has decrypted it and the expected signature
// appears. Patching before that writes into ciphertext.

// Does the keyboard belong to this game right now? Asked of the foreground window's PROCESS, not of
// a window handle: the game creates more than one window over its life, so a handle captured at
// startup would stop matching. Nothing is cached -- the answer changes with alt-tab.
static int hk_active(void) {
    if (!g_hkFocusOnly) return 1;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;                 // briefly true across a task switch or a lock screen
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// One press = one fire. The down/up state is remembered EVEN WHILE THE GAME IS NOT IN FRONT and
// only the firing is gated, so a key held down in another window does not fire on alt-tab back.
// Edge-detected on the MSB "currently down" bit: GetAsyncKeyState's LSB "recently pressed" bit is
// documented as unreliable.
// The modifier match is EXACT and deliberate: a modified binding must not fire the plain key, and
// the plain key must not fire while a modifier is down -- anything looser makes combos impossible.
// Left and right are the same thing (VK_CONTROL / VK_SHIFT / VK_MENU are the either-side codes).
static UINT hk_mods_now(void) {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= HK_CTRL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= HK_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= HK_ALT;
    return m;
}
// Down = the key is down AND exactly the wanted modifiers are. A binding that IS a modifier would
// otherwise never be down, so it excuses itself from its own test.
static int hk_down(UINT b) {
    UINT k = hk_key(b);
    if (!(GetAsyncKeyState((int)k) & 0x8000)) return 0;
    if (k == VK_CONTROL || k == VK_SHIFT || k == VK_MENU) return 1;
    return hk_mods_now() == hk_mods(b);
}
int hk_edge(UINT b, int* was) {
    int now = hk_down(b);
    int fire = now && !*was && hk_active();   // hk_active is only asked on an edge
    *was = now;
    return fire;
}

// Every timing here is a multiple of the poll tick (HK_POLL_MS, in srp_internal.h -- srp_save.c
// divides its tick counts by it): the tick is the resolution, so the repeat can never be faster
// than the poll and a key held for less than one tick can be missed entirely.
#define HK_REPEAT_DELAY_MS 400
#define HK_REPEAT_RATE_MS   80
// Hold the warp key this long and it records instead of warping. A hold IS the deliberation, so
// recording asks for no confirmation.
#define WARP_HOLD_MS         640

// One key, two gestures: returns 1 when the key was released without reaching the hold (a tap) and
// 2 the moment the hold is reached, while the key is still down. The tap can only land on RELEASE
// -- until the key comes back up there is no telling it from a hold.
typedef struct { int was; DWORD downAt; int held; } hk_tap;
static int hk_taphold(UINT b, hk_tap* s, DWORD now, DWORD holdMs) {
    int down = hk_down(b);
    int r = 0;
    if (down && !s->was)      { s->downAt = now; s->held = 0; }          // the press
    else if (down && !s->held && (int)(now - s->downAt) >= (int)holdMs) { s->held = 1; r = 2; }
    else if (!down && s->was && !s->held) { r = 1; }                     // released, still a tap
    s->was = down;
    return hk_active() ? r : 0;
}

// Held down = keeps firing (hk_edge stays the default: everything else here is a toggle or a
// one-shot). The clock keeps running while the game is in the background, so alt-tabbing back to a
// key that is still held resumes the repeat rather than bursting through what it "owed".
typedef struct { int was; DWORD due; } hk_rep;
static int hk_held(UINT b, hk_rep* s, DWORD now) {
    int down = hk_down(b);
    int fire = 0;
    if (down && !s->was) {                                  // the press itself
        s->due = now + HK_REPEAT_DELAY_MS;
        fire = 1;
    } else if (down && (int)(now - s->due) >= 0) {          // and every tick of the repeat after it
        s->due = now + HK_REPEAT_RATE_MS;
        fire = 1;
    }
    s->was = down;
    return fire && hk_active();
}
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    ff13log_reset();   // fresh log per launch
    ff13loga("[FF13-SRP] worker start. imagebase=%p (one-hit-kill DISARMED by default)\n",
             (void*)g_base);
    ff13loga("[FF13-SRP] files this mod writes: %s\n",
             g_dataDirOk == 1 ? MOD_DATA_DIR "\\"
                              : "beside the exe (" MOD_DATA_DIR "\\ could not be created)");
    config_load();
    state_load();   // restore the toggles from the last session
    save_sidecars_load();   // ...and what save-anywhere left in its sidecars
    crash_report_install();


    int waited = wait_for_bytes(g_base + RVA_OHK_SIG, P_OHK_SIG, sizeof(P_OHK_SIG), 120000);
    if (waited < 0) {
        ff13loga("[FF13-SRP] timed out waiting for SteamStub to decrypt the injection site -> no cave\n");
        ff13loga("[FF13-SRP] ...and with the worker gone, save-anywhere is not installed either\n");
        return 0;
    }
    ff13loga("[FF13-SRP] injection site decrypted after ~%dms; installing instant-win cave\n", waited);

    // install_onehit_cave does its own memcmp of the 6-byte prologue at the injection point.
    g_onehitCave = install_onehit_cave(g_base + RVA_OHK_INJECT, &g_armed,
                                           g_base + RVA_OHK_INJECT + 6);
    (g_onehitCave ? ff13logv : ff13loga)("[FF13-SRP] instant-win cave @0x%08X: %s (cave=%p)\n",
             (unsigned)(g_base + RVA_OHK_INJECT), g_onehitCave ? "OK" : "FAIL",
             g_onehitCave);
    if (!g_onehitCave) {
        ff13loga("[FF13-SRP] cave install failed (bytes did not match) -> nothing patched. Aborting.\n");
        ff13loga("[FF13-SRP] ...and with the worker gone, save-anywhere is not installed either\n");
        return 0;
    }

#ifdef FF13_DIAG
    // The AI-territory state setter: logs the id the game passes on a group defeat.
    int waited4 = wait_for_bytes(g_base + RVA_SETTER, P_SETTER, (int)sizeof(P_SETTER), 120000);
    if (waited4 < 0) {
        ff13loga("[FF13-SRP] timed out waiting for AiTerritoryState setter to decrypt -> no setter hook\n");
    } else {
        void* scave = ff13_install_stackarg_hook(g_base + RVA_SETTER, P_SETTER,
                                                 (int)sizeof(P_SETTER), (void*)on_terr_setter, 4);
        (scave ? ff13logv : ff13loga)("[FF13-SRP] AiTerritoryState setter hook @0x%08X: %s (decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_SETTER), scave ? "OK" : "FAIL", waited4);
    }

    // Entry probes on the activate-variant natives: which one the restore script calls, and in
    // which operand slot. All prologues are 55 8B EC 83 EC XX.
    struct { unsigned rva; uint8_t pro[6]; void* fn; const char* nm; } acts[] = {
        {0x7DE810, {0x55,0x8B,0xEC,0x83,0xEC,0x4C}, (void*)on_probe_actArea,  "actArea_l"},
        {0x7E0230, {0x55,0x8B,0xEC,0x83,0xEC,0x44}, (void*)on_probe_withHide, "actWithHide_l"},
        {0x7E0320, {0x55,0x8B,0xEC,0x83,0xEC,0x44}, (void*)on_probe_ignoreFA, "actIgnoreFA_l"},
        {0x7E04E0, {0x55,0x8B,0xEC,0x83,0xEC,0x38}, (void*)on_probe_wait,     "waitAct"},
        {0x7DFFA0, {0x55,0x8B,0xEC,0x83,0xEC,0x74}, (void*)on_probe_poll,     "pollAct"},
        {0x7E0800, {0x55,0x8B,0xEC,0x83,0xEC,0x38}, (void*)on_probe_deact,    "deactivate"},
    };
    for (int i = 0; i < 6; i++) {
        int w = wait_for_bytes(g_base + acts[i].rva, acts[i].pro, 6, 60000);
        if (w < 0) { ff13logv("[FF13-SRP] activate probe %s: decrypt timeout\n", acts[i].nm); continue; }
        void* c = ff13_install_stackarg_hook(g_base + acts[i].rva, acts[i].pro, 6, acts[i].fn, 4);
        (c ? ff13logv : ff13loga)("[FF13-SRP] activate probe %s @0x%08X: %s\n",
                 acts[i].nm, (unsigned)(g_base + acts[i].rva), c ? "OK" : "FAIL");
    }
#endif // FF13_DIAG

    // Install THE suppressor on activateAreaWithHide_l (the native that pops the persisting group).
    static const uint8_t P_WH[6] = {0x83,0x7D,0xFC,0x00,0x74,0x3D}; // cmp [ebp-4],0 ; je 0xBE02E0
    int waited6 = wait_for_bytes(g_base + RVA_WH_HOOK, P_WH, (int)sizeof(P_WH), 120000);
    if (waited6 < 0) {
        ff13loga("[FF13-SRP] timed out waiting for activateAreaWithHide_l to decrypt -> no suppressor\n");
    } else {
        g_whCave = install_withhide_cave(g_base + RVA_WH_HOOK, (void*)should_skip_activate,
                                         g_base + RVA_WH_CONT, g_base + RVA_WH_SKIP);
        (g_whCave ? ff13logv : ff13loga)("[FF13-SRP] withHide suppressor @0x%08X: %s (every " SUPPRESS_PREFIX "* group, %s; "
                 "decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_WH_HOOK), g_whCave ? "OK" : "FAIL",
                 g_suppressEnabled ? "ENABLED" : "disabled (press the suppress hotkey)", waited6);
    }

    // Game-speed, on the app-clock tick entry: the stackarg hook passes [esp+4] (= frameDeltaMs
    // pointer) to on_frame_tick. Non-fatal on failure.
    int waited7 = wait_for_bytes(g_base + RVA_FRAMETICK, P_FRAMETICK, (int)sizeof(P_FRAMETICK), 120000);
    if (waited7 < 0) {
        ff13loga("[FF13-SRP] timed out waiting for frame-tick to decrypt -> no game-speed\n");
    } else {
        void* hc = ff13_install_stackarg_hook(g_base + RVA_FRAMETICK, P_FRAMETICK,
                                              (int)sizeof(P_FRAMETICK), (void*)on_frame_tick, 4);
        (hc ? ff13logv : ff13loga)("[FF13-SRP] game-speed hook @0x%08X: %s (factor=%.2f, %s; decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_FRAMETICK), hc ? "OK" : "FAIL", g_gsFactor,
                 g_gsEnabled ? "ENABLED" : "disabled (press the game-speed hotkey)", waited7);
    }

    // The instant-win pair: a read-only probe on activateBattleChara's entry and the gate on its
    // residency verdict. Both are non-fatal and both are idle unless asked for -- the probe unless
    // `log = verbose`, the gate unless the kill mode is INSTANT WIN. In the release build on
    // purpose: that is what gets tested on hardware.
    int waitedEp = wait_for_bytes(g_base + RVA_ACTIVATE, P_ACTIVATE, (int)sizeof(P_ACTIVATE), 15000);
    if (waitedEp < 0) {
        ff13logv("[FF13-SRP] instant-win: 0x%08X did not show activateBattleChara's prologue "
                 "within 15s -> no probe\n", (unsigned)(g_base + RVA_ACTIVATE));
    } else {
        void* ep = ff13_install_esp_hook(g_base + RVA_ACTIVATE, P_ACTIVATE,
                                         (int)sizeof(P_ACTIVATE), (void*)on_activate_probe);
        g_iwInstProbe = ep ? 1 : 0;
        (ep ? ff13logv : ff13loga)("[FF13-SRP] instant-win: activate probe @0x%08X: %s (decrypted after ~%dms; "
                 "logs only while log=verbose)\n",
                 (unsigned)(g_base + RVA_ACTIVATE), ep ? "OK" : "FAIL", waitedEp);
    }
    int waitedEg = wait_for_bytes(g_base + RVA_ACTIVATE_GATE, P_ACTGATE, (int)sizeof(P_ACTGATE), 15000);
    if (waitedEg < 0) {
        ff13logv("[FF13-SRP] instant-win: 0x%08X did not show the residency cmp "
                 "(83 BD 48 FF FF FF 00) within 15s -> NO instant-win gate; the toggle will do "
                 "nothing\n", (unsigned)(g_base + RVA_ACTIVATE_GATE));
    } else {
        void* eg = ff13_install_esp_hook(g_base + RVA_ACTIVATE_GATE, P_ACTGATE,
                                         (int)sizeof(P_ACTGATE), (void*)on_activate_gate);
        g_iwInstGate = eg ? 1 : 0;
        (eg ? ff13logv : ff13loga)("[FF13-SRP] instant-win: gate @0x%08X: %s (decrypted after ~%dms; OFF until "
                 "the hotkey is pressed)\n",
                 (unsigned)(g_base + RVA_ACTIVATE_GATE), eg ? "OK" : "FAIL", waitedEg);
    }

    // Warp: capture the field leader at GetWorldPos (0x78EEE0) and override the committed position
    // at the per-object commit (0x79C5E0). Non-fatal: on failure only warp is unavailable.
    int waitedTc = wait_for_bytes(g_base + RVA_GETWORLDPOS, P_TP6, 6, 120000);
    if (waitedTc < 0) {
        ff13loga("[FF13-SRP] timed out waiting for GetWorldPos to decrypt -> no warp capture\n");
    } else {
        void* tc = ff13_install_ecx_hook(g_base + RVA_GETWORLDPOS, P_TP6, 6, (void*)on_getworldpos);
        (tc ? ff13logv : ff13loga)("[FF13-SRP] warp capture hook @0x%08X: %s (decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_GETWORLDPOS), tc ? "OK" : "FAIL", waitedTc);
    }
    int waitedTo = wait_for_bytes(g_base + RVA_COMMIT, P_TP6, 6, 120000);
    if (waitedTo < 0) {
        ff13loga("[FF13-SRP] timed out waiting for commit-transform to decrypt -> no warp warp\n");
    } else {
        void* to = ff13_install_ecx_stackarg_hook(g_base + RVA_COMMIT, P_TP6, 6, (void*)on_commit, 4);
        (to ? ff13logv : ff13loga)("[FF13-SRP] warp override hook @0x%08X: %s (decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_COMMIT), to ? "OK" : "FAIL", waitedTo);
    }
    warp_load();   // restore a previously recorded point (persists across game restarts)
    zb_load();   // zone -> btsc band, so the picker works from a cold start in known zones

    // Battle swap at the startBattle_l entry (0xBE3DF0). READ-ONLY unless armed: disarmed it only
    // logs the btsc name of each encounter.
    // Short timeouts from here on, on purpose: `.text` is decrypted in one go at startup (every
    // hook reports ~0ms, the earliest ~300ms), so a long wait never means "still encrypted", it
    // means the site is wrong -- and waiting 2 minutes for that would hold up the overlay and the
    // hotkey loop, which run after this point.
    int waitedSb = wait_for_bytes(g_base + RVA_STARTBATTLE, P_SB, (int)sizeof(P_SB), 15000);
    if (waitedSb < 0) {
        ff13loga("[FF13-SRP] timed out waiting for startBattle_l to decrypt -> no battle swap\n");
    } else {
        void* sb = ff13_install_stackarg_hook(g_base + RVA_STARTBATTLE, P_SB, (int)sizeof(P_SB),
                                              (void*)on_start_battle, 4);
        (sb ? ff13logv : ff13loga)("[FF13-SRP] battle-swap hook (script path) @0x%08X: %s (decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_STARTBATTLE), sb ? "OK" : "FAIL", waitedSb);
    }

    // Where a cutscene begins, so the picker's cover can stay up until one does (see
    // on_start_cinema). Read-only. Without it the cover comes down on the fight instead, which only
    // means the field shows through for a moment.
    {
        static const uint8_t P_SC[6] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0};   // and esp, -16
        int waitedSc = wait_for_bytes(g_base + RVA_STARTCINEMA, P_SC, (int)sizeof(P_SC), 15000);
        if (waitedSc < 0) {
            ff13loga("[FF13-SRP] startCinema @0x%08X never matched -> the cover comes down on the "
                     "fight rather than on the cutscene\n", (unsigned)(g_base + RVA_STARTCINEMA));
        } else {
            void* sc = ff13_install_stackarg_hook(g_base + RVA_STARTCINEMA, P_SC, (int)sizeof(P_SC),
                                                  (void*)on_start_cinema, 4);
            (sc ? ff13logv : ff13loga)("[FF13-SRP] cutscene-start hook @0x%08X: %s\n",
                     (unsigned)(g_base + RVA_STARTCINEMA), sc ? "OK" : "FAIL");
        }
    }

    // The one that matters for ordinary enemies: the field-encounter scene-id store in the
    // encounter manager (0xB50326). Ordinary symbols never reach startBattle_l -- they come
    // through doEncountCheck -> 0xB50270 -> this store. The key override inside getBattleScene
    // lets a NAMED scene (a story boss) be resolved by a fight that only has a number.
    int waitedEs = wait_for_bytes(g_base + RVA_ENCSCENE, P_ENC, (int)sizeof(P_ENC), 15000);
    if (waitedEs < 0) {
        ff13loga("[FF13-SRP] encounter scene store @0x%08X never matched its expected bytes -> "
                 "no field battle swap (site wrong?)\n", (unsigned)(g_base + RVA_ENCSCENE));
    } else {
        g_encCave = install_encscene_cave(g_base + RVA_ENCSCENE, (void*)on_encount_scene);
        (g_encCave ? ff13logv : ff13loga)("[FF13-SRP] battle-swap hook (field path) @0x%08X: %s (target=btsc%05u, %s; "
                 "decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_ENCSCENE), g_encCave ? "OK" : "FAIL",
                 g_swapBtscId, g_swapEnabled ? "ARMED" : "disarmed", waitedEs);
    }

    // Read-only collection hooks (see the block above): which scene id the fight is really built
    // from, and the touched-object id it all derives from. Both non-fatal.
    int waitedGb = wait_for_bytes(g_base + RVA_GETBTSC, P_GBS, (int)sizeof(P_GBS), 15000);
    if (waitedGb < 0) {
        ff13loga("[FF13-SRP] getBattleScene @0x%08X never matched -> no scene-lookup log\n",
                 (unsigned)(g_base + RVA_GETBTSC));
    } else {
        void* gb = ff13_install_argptr_hook(g_base + RVA_GETBTSC, P_GBS, (int)sizeof(P_GBS),
                                            (void*)on_get_btsc, 0, 4, 8);
        (gb ? ff13logv : ff13loga)("[FF13-SRP] getBattleScene log hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_GETBTSC), gb ? "OK" : "FAIL");
    }
    // Read-only: borrow the object the game asks "is this charaspec read" on.
    int waitedIr = wait_for_bytes(g_base + RVA_ISREAD_CALL, P_ISREAD, (int)sizeof(P_ISREAD), 15000);
    if (waitedIr < 0) {
        ff13loga("[FF13-SRP] charaspec-read call @0x%08X never matched -> probe stays blind\n",
                 (unsigned)(g_base + RVA_ISREAD_CALL));
    } else {
        void* ir = ff13_install_ecx_hook(g_base + RVA_ISREAD_CALL, P_ISREAD, (int)sizeof(P_ISREAD),
                                         (void*)on_isread_call);
        (ir ? ff13logv : ff13loga)("[FF13-SRP] charaspec-read hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ISREAD_CALL), ir ? "OK" : "FAIL");
    }
    // Read-only: the charaspec load requests the game itself issues, with the names it uses.
    int waitedCq = wait_for_bytes(g_base + RVA_CSREQ, P_CSREQ, (int)sizeof(P_CSREQ), 15000);
    if (waitedCq < 0) {
        ff13loga("[FF13-SRP] charaspec request site @0x%08X never matched -> not logged\n",
                 (unsigned)(g_base + RVA_CSREQ));
    } else {
        void* cq = ff13_install_argptr_hook(g_base + RVA_CSREQ, P_CSREQ, (int)sizeof(P_CSREQ),
                                            (void*)on_charaspec_request, 0, 4, 8);
        (cq ? ff13logv : ff13loga)("[FF13-SRP] preload request hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_CSREQ), cq ? "OK" : "FAIL");
    }
    // Read-only: the actor's resource section, which the residency probe needs.
    int waitedAs = wait_for_bytes(g_base + RVA_ACTORSECT, P_ASECT, (int)sizeof(P_ASECT), 15000);
    if (waitedAs < 0) {
        ff13loga("[FF13-SRP] actor section site @0x%08X never matched -> residency probe off\n",
                 (unsigned)(g_base + RVA_ACTORSECT));
    } else {
        void* as = ff13_install_ecx_hook(g_base + RVA_ACTORSECT, P_ASECT, (int)sizeof(P_ASECT),
                                         (void*)on_actor_section);
        (as ? ff13logv : ff13loga)("[FF13-SRP] actor section hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ACTORSECT), as ? "OK" : "FAIL");
    }
    // The conditional-target resolver, so a buff's target can be watched (and later chosen).
    int waitedCt = wait_for_bytes(g_base + RVA_RESOLVETGT, P_RESOLVETGT, (int)sizeof(P_RESOLVETGT),
                                  15000);
    if (waitedCt < 0) {
        ff13loga("[FF13-SRP] conditional-target site @0x%08X never matched -> not watched\n",
                 (unsigned)(g_base + RVA_RESOLVETGT));
    } else {
        void* ct = ff13_install_argptr_hook(g_base + RVA_RESOLVETGT, P_RESOLVETGT,
                                            (int)sizeof(P_RESOLVETGT), (void*)on_cond_target,
                                            8, 0xC, 4);
        (ct ? ff13logv : ff13loga)("[FF13-SRP] conditional-target hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_RESOLVETGT), ct ? "OK" : "FAIL");
    }
    // The preparation menu a restart branch opens: made to return without opening while a picked
    // fight is being staged (see menuskip_install).
    {
        int ms = menuskip_install();
        (ms ? ff13logv : ff13loga)("[FF13-SRP] preparation-menu skip @0x%08X: %s\n",
                 (unsigned)(g_base + 0x7E80D0), ms ? "OK" : "FAIL");
    }
    // The ability door, upstream of the charaspec one (see on_ability_label).
    int waitedAb = wait_for_bytes(g_base + RVA_ABILLABEL, P_ABILLABEL, (int)sizeof(P_ABILLABEL),
                                  15000);
    if (waitedAb < 0) {
        ff13loga("[FF13-SRP] ability label site @0x%08X never matched -> that door is off\n",
                 (unsigned)(g_base + RVA_ABILLABEL));
    } else {
        void* ab = ff13_install_ecx_stackarg_hook(g_base + RVA_ABILLABEL, P_ABILLABEL,
                                                  (int)sizeof(P_ABILLABEL),
                                                  (void*)on_ability_label, 4);
        (ab ? ff13logv : ff13loga)("[FF13-SRP] ability label hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ABILLABEL), ab ? "OK" : "FAIL");
    }
    // The ATB-speed setter game-speed leans on: called, not hooked, so the wait is the only proof
    // the bytes are the function we read -- and calling into the wrong bytes under SteamStub is how
    // a mod takes the game with it.
    int waitedAtb = wait_for_bytes(g_base + RVA_SETATBSPEED, P_SETATB, (int)sizeof(P_SETATB), 15000);
    g_atbOk = waitedAtb >= 0;
    (g_atbOk ? ff13logv : ff13loga)("[FF13-SRP] ATB speed setter @0x%08X: %s\n", (unsigned)(g_base + RVA_SETATBSPEED),
             g_atbOk ? "OK" : "NOT THERE -- game-speed leaves the gauges alone");
    // The AI's after-exec queue, so a bent form spends its own mode in the enemy's bookkeeping
    // rather than the one the AI rolled (see on_ai_afterexec).
    int waitedAe = wait_for_bytes(g_base + RVA_AIAFTER, P_AIAFTER, (int)sizeof(P_AIAFTER), 15000);
    if (waitedAe < 0) {
        ff13loga("[FF13-SRP] AI after-exec queue @0x%08X never matched -> a pinned mode can repeat\n",
                 (unsigned)(g_base + RVA_AIAFTER));
    } else {
        void* ae = ff13_install_stackarg_hook(g_base + RVA_AIAFTER, P_AIAFTER, (int)sizeof(P_AIAFTER),
                                              (void*)on_ai_afterexec, 4);
        (ae ? ff13logv : ff13loga)("[FF13-SRP] AI after-exec hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_AIAFTER), ae ? "OK" : "FAIL");
    }
    {
        int wc = wait_for_bytes(g_base + RVA_AICOND, P_AICOND, (int)sizeof(P_AICOND), 15000);
        void* cd = wc < 0 ? NULL
                 : ff13_install_esp_hook(g_base + RVA_AICOND, P_AICOND, (int)sizeof(P_AICOND),
                                         (void*)on_ai_cond);
        (cd ? ff13logv : ff13loga)("[FF13-SRP] AI condition hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_AICOND), cd ? "OK" : "FAIL");
    }
    {
        int ws = wait_for_bytes(g_base + RVA_AISTATE, P_AISTATE, (int)sizeof(P_AISTATE), 15000);
        void* st = ws < 0 ? NULL
                 : ff13_install_argptr_hook(g_base + RVA_AISTATE, P_AISTATE,
                                            (int)sizeof(P_AISTATE), (void*)on_ai_state, 4, 0, 0x10);
        (st ? ff13logv : ff13loga)("[FF13-SRP] AI state hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_AISTATE), st ? "OK" : "FAIL");
    }
    // The charaspec swap, so a picked form can be made to stick (see on_spec_replace).
    int waitedSr = wait_for_bytes(g_base + RVA_SPECREPL, P_SPECREPL, (int)sizeof(P_SPECREPL), 15000);
    if (waitedSr < 0) {
        ff13loga("[FF13-SRP] charaspec swap @0x%08X never matched -> form pin off\n",
                 (unsigned)(g_base + RVA_SPECREPL));
    } else {
        void* sr = ff13_install_ecx_stackarg_hook(g_base + RVA_SPECREPL, P_SPECREPL,
                                                  (int)sizeof(P_SPECREPL),
                                                  (void*)on_spec_replace, 4);
        (sr ? ff13logv : ff13loga)("[FF13-SRP] charaspec swap hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_SPECREPL), sr ? "OK" : "FAIL");
    }
    // Read-only: which camera sequence the fight is given (see on_cam_seq).
    int waitedCam = wait_for_bytes(g_base + RVA_CAMSEQ, P_CAMSEQ, (int)sizeof(P_CAMSEQ), 15000);
    if (waitedCam < 0) {
        ff13loga("[FF13-SRP] battle camera setter @0x%08X never matched -> not logged\n",
                 (unsigned)(g_base + RVA_CAMSEQ));
    } else {
        void* cm = ff13_install_stackarg_hook(g_base + RVA_CAMSEQ, P_CAMSEQ, (int)sizeof(P_CAMSEQ),
                                              (void*)on_cam_seq, 4);
        (cm ? ff13logv : ff13loga)("[FF13-SRP] battle camera log hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_CAMSEQ), cm ? "OK" : "FAIL");
    }
#ifdef FF13_DIAG
    if (wait_for_bytes(g_base + RVA_HASCAM, P_HASCAM, (int)sizeof(P_HASCAM), 15000) >= 0) {
        void* hc = ff13_install_ecx_hook(g_base + RVA_HASCAM, P_HASCAM, (int)sizeof(P_HASCAM),
                                         (void*)on_has_cam);
        (hc ? ff13logv : ff13loga)("[FF13-SRP] hasIntroCamera hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_HASCAM), hc ? "OK" : "FAIL");
    }
    // The game's own battle-camera narration, which retail compiled out (see on_trace).
    if (wait_for_bytes(g_base + RVA_TRACE, P_TRACE, (int)sizeof(P_TRACE), 15000) >= 0) {
        void* tr = ff13_install_argptr_hook(g_base + RVA_TRACE, P_TRACE, (int)sizeof(P_TRACE),
                                            (void*)on_trace, 4, 8, 8);
        (tr ? ff13logv : ff13loga)("[FF13-SRP] battle trace hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_TRACE), tr ? "OK" : "FAIL");
    }
#endif
#ifdef FF13_DIAG
    g_getCamOk = wait_for_bytes(g_base + RVA_GETCAM, P_GETCAM, (int)sizeof(P_GETCAM), 15000) >= 0;
    (g_getCamOk ? ff13logv : ff13loga)("[FF13-SRP] current-camera reader @0x%08X: %s\n", (unsigned)(g_base + RVA_GETCAM),
             g_getCamOk ? "OK" : "never matched -> the camera will not be watched");
#endif
    // The battle camera control, which is where an arena fight is told where to open from.
    if (wait_for_bytes(g_base + RVA_WIDESHOT, P_WIDESHOT, (int)sizeof(P_WIDESHOT), 15000) < 0) {
        ff13loga("[FF13-SRP] battle camera control @0x%08X never matched -> arena fights open from "
                 "wherever the field camera stood\n", (unsigned)(g_base + RVA_WIDESHOT));
    } else {
        void* ws = ff13_install_esp_hook(g_base + RVA_WIDESHOT, P_WIDESHOT, (int)sizeof(P_WIDESHOT),
                                         (void*)on_wide_shot);
        (ws ? ff13logv : ff13loga)("[FF13-SRP] battle camera hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_WIDESHOT), ws ? "OK" : "FAIL");
    }
    g_camRecOk = wait_for_bytes(g_base + RVA_CAM_RECENTER, P_CAMREC, (int)sizeof(P_CAMREC), 15000) >= 0;
    (g_camRecOk ? ff13logv : ff13loga)("[FF13-SRP] field camera recenter @0x%08X: %s\n", (unsigned)(g_base + RVA_CAM_RECENTER),
             g_camRecOk ? "OK" : "never matched -> the camera will not be snapped");
    // Not hooked -- called (see cam_supply). The wait is the proof the bytes are the function.
    if (wait_for_bytes(g_base + RVA_CAMREQ, P_CAMREQ, (int)sizeof(P_CAMREQ), 15000) < 0) {
        ff13loga("[FF13-SRP] battle camera request @0x%08X never matched -> no camera supplied\n",
                 (unsigned)(g_base + RVA_CAMREQ));
    } else {
        // valOff 0 = the return address (which call site asked), ptrOff1 4 = the slot holding the
        // request struct, whose first 16 bytes are the sequence name.
        void* cr = ff13_install_argptr_hook(g_base + RVA_CAMREQ, P_CAMREQ, (int)sizeof(P_CAMREQ),
                                            (void*)on_cam_request, 0, 4, 4);
        g_camSetOk = cr ? 1 : 0;
        (cr ? ff13logv : ff13loga)("[FF13-SRP] battle camera request hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_CAMREQ), cr ? "OK" : "FAIL");
    }
    // The scene number at its source, before the fight is assembled from it (see on_battle_entry).
    // Non-fatal: without it the getBattleScene hook still substitutes, it just leaves the two
    // stored copies of the number pointing at the scene we replaced.
    int waitedSt = wait_for_bytes(g_base + RVA_STARTBTL, P_SBTL, (int)sizeof(P_SBTL), 15000);
    if (waitedSt < 0) {
        ff13loga("[FF13-SRP] startBattle @0x%08X never matched -> scene number substituted at the "
                 "lookups only\n", (unsigned)(g_base + RVA_STARTBTL));
    } else {
        // valOff 0 = the return address (which of the two doors this fight came through),
        // ptrOff1 4 = &btsc, ptrOff2 8 = &flags.
        void* sb = ff13_install_argptr_hook(g_base + RVA_STARTBTL, P_SBTL, (int)sizeof(P_SBTL),
                                            (void*)on_battle_entry, 0, 4, 8);
        (sb ? ff13logv : ff13loga)("[FF13-SRP] startBattle hook @0x%08X: %s (decrypted after ~%dms)\n",
                 (unsigned)(g_base + RVA_STARTBTL), sb ? "OK" : "FAIL", waitedSt);
    }
    // Not a hook -- the trigger dispatcher is CALLED (see restart_watch).
    g_trigOk = wait_for_bytes(g_base + RVA_SENDTRIG, P_SENDTRIG, (int)sizeof(P_SENDTRIG), 15000) >= 0;
    (g_trigOk ? ff13logv : ff13loga)("[FF13-SRP] state-machine trigger @0x%08X: %s\n",
             (unsigned)(g_base + RVA_SENDTRIG), g_trigOk ? "OK" : "never matched");
    // Not hooked -- called (see vm_load_chara).
    g_lchOk = wait_for_bytes(g_base + RVA_LOADCHARA, P_LCH, (int)sizeof(P_LCH), 15000) >= 0;
    g_ilcOk = wait_for_bytes(g_base + RVA_ISLOADEDCHARA, P_ILC, (int)sizeof(P_ILC), 15000) >= 0;
    (g_lchOk && g_ilcOk ? ff13logv : ff13loga)("[FF13-SRP] per-character loader @0x%08X: %s (readback %s)\n",
             (unsigned)(g_base + RVA_LOADCHARA),
             g_lchOk ? "OK" : "never matched -> charaspecs in no set stay unreachable",
             g_ilcOk ? "OK" : "unavailable");
    // Read-only: what a cutscene sets the paradigms to (see on_optima_regist).
    if (wait_for_bytes(g_base + RVA_OPTIMA_REGIST, P_OPTREG, (int)sizeof(P_OPTREG), 15000) >= 0) {
        void* o1 = ff13_install_stackarg_hook(g_base + RVA_OPTIMA_REGIST, P_OPTREG,
                                              (int)sizeof(P_OPTREG), (void*)on_optima_regist, 4);
        void* o2 = NULL;
        if (wait_for_bytes(g_base + RVA_OPTIMA_SET, P_OPTSET, (int)sizeof(P_OPTSET), 15000) >= 0)
            o2 = ff13_install_stackarg_hook(g_base + RVA_OPTIMA_SET, P_OPTSET,
                                            (int)sizeof(P_OPTSET), (void*)on_optima_set, 4);
        if (wait_for_bytes(g_base + RVA_OPTIMA_CLEAR, P_OPTCLR, (int)sizeof(P_OPTCLR), 15000) >= 0)
            ff13_install_stackarg_hook(g_base + RVA_OPTIMA_CLEAR, P_OPTCLR,
                                       (int)sizeof(P_OPTCLR), (void*)on_optima_clear, 4);
        if (wait_for_bytes(g_base + RVA_OPTIMA_CLRALL, P_OPTALL, (int)sizeof(P_OPTALL), 15000) >= 0)
            ff13_install_stackarg_hook(g_base + RVA_OPTIMA_CLRALL, P_OPTALL,
                                       (int)sizeof(P_OPTALL), (void*)on_optima_clrall, 4);
        if (wait_for_bytes(g_base + RVA_ADDROLE, P_ADDROLE, (int)sizeof(P_ADDROLE), 15000) >= 0)
            ff13_install_stackarg_hook(g_base + RVA_ADDROLE, P_ADDROLE,
                                       (int)sizeof(P_ADDROLE), (void*)on_add_role, 4);
        (o1 && o2 ? ff13logv : ff13loga)("[FF13-SRP] paradigm deck log @0x%08X/0x%08X: %s/%s\n",
                 (unsigned)(g_base + RVA_OPTIMA_REGIST), (unsigned)(g_base + RVA_OPTIMA_SET),
                 o1 ? "OK" : "FAIL", o2 ? "OK" : "FAIL");
    }
    // Read-only: the one frame where a real symbol contact was thrown away (see on_encount_refused).
    {
        void* rf = NULL;
        if (wait_for_bytes(g_base + RVA_ENCREFUSE, P_ENCREFUSE, (int)sizeof(P_ENCREFUSE), 15000) >= 0)
            rf = ff13_install_ecx_hook(g_base + RVA_ENCREFUSE, P_ENCREFUSE,
                                       (int)sizeof(P_ENCREFUSE), (void*)on_encount_refused);
        (rf ? ff13logv : ff13loga)("[FF13-SRP] refused-touch hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ENCREFUSE), rf ? "OK" : "FAIL");
    }
    // Where the free hit is decided (see on_enc_field4). Read-only unless the ini asks otherwise.
    {
        void* e4 = NULL;
        if (wait_for_bytes(g_base + RVA_ENCF4, P_ENCF4, (int)sizeof(P_ENCF4), 15000) >= 0)
            e4 = ff13_install_ecx_hook(g_base + RVA_ENCF4, P_ENCF4, (int)sizeof(P_ENCF4),
                                       (void*)on_enc_field4);
        (e4 ? ff13logv : ff13loga)("[FF13-SRP] encounter field-4 hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ENCF4), e4 ? "OK" : "FAIL");
        if (e4) preds_verify();
    }
    // Read-only: how long the loading indicator is actually up (see on_load_show).
    {
        void* l1 = NULL, * l2 = NULL;
        if (wait_for_bytes(g_base + RVA_LOADSHOW, P_LOADSHOW, (int)sizeof(P_LOADSHOW), 15000) >= 0)
            l1 = ff13_install_stackarg_hook(g_base + RVA_LOADSHOW, P_LOADSHOW,
                                            (int)sizeof(P_LOADSHOW), (void*)on_load_show, 4);
        if (wait_for_bytes(g_base + RVA_LOADHIDE, P_LOADHIDE, (int)sizeof(P_LOADHIDE), 15000) >= 0)
            l2 = ff13_install_ecx_hook(g_base + RVA_LOADHIDE, P_LOADHIDE,
                                       (int)sizeof(P_LOADHIDE), (void*)on_load_hide);
        (l1 && l2 ? ff13logv : ff13loga)("[FF13-SRP] loading-indicator log @0x%08X/0x%08X: %s/%s\n",
                 (unsigned)(g_base + RVA_LOADSHOW), (unsigned)(g_base + RVA_LOADHIDE),
                 l1 ? "OK" : "FAIL", l2 ? "OK" : "FAIL");
    }
    // The game-over menu, so a lost practice fight goes back without being asked.
    if (wait_for_bytes(g_base + RVA_GOMENU, P_GOMENU, (int)sizeof(P_GOMENU), 15000) < 0) {
        ff13loga("[FF13-SRP] game-over menu @0x%08X never matched -> it keeps asking\n",
                 (unsigned)(g_base + RVA_GOMENU));
    } else {
        void* gm = ff13_install_ecx_hook(g_base + RVA_GOMENU, P_GOMENU, (int)sizeof(P_GOMENU),
                                         (void*)on_gameover_menu);
        g_goOk = gm ? 1 : 0;
        (g_goOk ? ff13logv : ff13loga)("[FF13-SRP] game-over menu hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_GOMENU), g_goOk ? "OK" : "FAIL");
    }
    // ...and the sound it makes, which is none -- but only for a fight this mod started (go_se_set).
    g_goSeOk = wait_for_bytes(g_base + RVA_GOSE, P_GOSE, GO_SE_LEN, 15000) >= 0;
    (g_goSeOk ? ff13logv : ff13loga)("[FF13-SRP] game-over sound @0x%08X: %s\n", (unsigned)(g_base + RVA_GOSE),
             g_goSeOk ? "silenced on a picked fight, left alone otherwise" : "never matched");

    // What a picked fight costs (see on_item_take). The reading and the giving are calls; only the
    // taking is hooked.
    g_itemOk = wait_for_bytes(g_base + RVA_ITEMCOUNT, P_ITEMCOUNT, (int)sizeof(P_ITEMCOUNT), 15000) >= 0
            && wait_for_bytes(g_base + RVA_ITEMGIVE,  P_ITEMGIVE,  (int)sizeof(P_ITEMGIVE),  15000) >= 0;
    if (!g_itemOk) {
        ff13loga("[FF13-SRP] item count/give @0x%08X/0x%08X never matched -> what a picked fight "
                 "uses stays used\n", (unsigned)(g_base + RVA_ITEMCOUNT),
                 (unsigned)(g_base + RVA_ITEMGIVE));
    } else if (wait_for_bytes(g_base + RVA_ITEMTAKE, P_ITEMTAKE, (int)sizeof(P_ITEMTAKE), 15000) < 0) {
        g_itemOk = 0;
        ff13loga("[FF13-SRP] item take @0x%08X never matched -> what a picked fight uses stays "
                 "used\n", (unsigned)(g_base + RVA_ITEMTAKE));
    } else {
        void* it = ff13_install_esp_hook(g_base + RVA_ITEMTAKE, P_ITEMTAKE, (int)sizeof(P_ITEMTAKE),
                                         (void*)on_item_take);
        if (!it) g_itemOk = 0;
        (it ? ff13logv : ff13loga)("[FF13-SRP] item tally hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ITEMTAKE), it ? "OK" : "FAIL");
    }
    // TP, which is read and written rather than hooked (see tp_give_back).
    g_tpOk = wait_for_bytes(g_base + RVA_TPGET, P_TPGET, (int)sizeof(P_TPGET), 15000) >= 0
          && wait_for_bytes(g_base + RVA_TPSET, P_TPSET, (int)sizeof(P_TPSET), 15000) >= 0;
    (g_tpOk ? ff13logv : ff13loga)("[FF13-SRP] TP get/set @0x%08X/0x%08X: %s\n", (unsigned)(g_base + RVA_TPGET),
             (unsigned)(g_base + RVA_TPSET),
             g_tpOk ? "OK" : "never matched -> TP a picked fight spends stays spent");
    // Libra is neither called nor hooked: the arrays are read and written directly and the only
    // code borrowed is the lock the Datalog's own accessors take, so it is that pair that gets
    // checked. The Datalog pointer itself is read fresh every time (see libra_datalog).
    g_libraOk = wait_for_bytes(g_base + RVA_LOCK_ENTER, P_LOCKENTER, (int)sizeof(P_LOCKENTER), 15000) >= 0
             && wait_for_bytes(g_base + RVA_LOCK_LEAVE, P_LOCKLEAVE, (int)sizeof(P_LOCKLEAVE), 15000) >= 0;
    (g_libraOk ? ff13logv : ff13loga)("[FF13-SRP] Datalog lock @0x%08X/0x%08X: %s\n", (unsigned)(g_base + RVA_LOCK_ENTER),
             (unsigned)(g_base + RVA_LOCK_LEAVE),
             g_libraOk ? "OK -- what a picked fight teaches is put back"
                       : "never matched -> what a picked fight teaches stays learned");

#ifdef FF13_DIAG
    if (wait_for_bytes(g_base + RVA_WORKER_IN, P_WORKIN, (int)sizeof(P_WORKIN), 15000) >= 0) {
        void* w1 = ff13_install_esp_hook(g_base + RVA_WORKER_IN, P_WORKIN, (int)sizeof(P_WORKIN),
                                         (void*)on_worker_in);
        (w1 ? ff13logv : ff13loga)("[FF13-SRP] battle-over worker entry: %s\n", w1 ? "OK" : "FAIL");
    } else ff13loga("[FF13-SRP] battle-over worker entry never matched\n");
    if (wait_for_bytes(g_base + RVA_WORKER_ARM, P_WORKARM, (int)sizeof(P_WORKARM), 15000) >= 0) {
        void* w2 = ff13_install_esp_hook(g_base + RVA_WORKER_ARM, P_WORKARM, (int)sizeof(P_WORKARM),
                                         (void*)on_worker_arm);
        (w2 ? ff13logv : ff13loga)("[FF13-SRP] battle-over worker gate: %s\n", w2 ? "OK" : "FAIL");
    } else ff13loga("[FF13-SRP] battle-over worker gate never matched\n");
#endif
    // Where a won fight is told to end the way a lost one does (see on_worker_how).
    if (wait_for_bytes(g_base + RVA_WORKER_HOW, P_WORKHOW, (int)sizeof(P_WORKHOW), 15000) < 0) {
        ff13loga("[FF13-SRP] battle-over worker @0x%08X never matched -> a won fight goes on with "
                 "the story\n", (unsigned)(g_base + RVA_WORKER_HOW));
    } else {
        void* wh = ff13_install_esp_hook(g_base + RVA_WORKER_HOW, P_WORKHOW,
                                         (int)sizeof(P_WORKHOW), (void*)on_worker_how);
        g_workerHowOk = wh ? 1 : 0;
        (wh ? ff13logv : ff13loga)("[FF13-SRP] battle-over worker hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_WORKER_HOW), wh ? "OK" : "FAIL");
    }

    // Where an actor with no place of its own gets one (see on_place_join).
    if (wait_for_bytes(g_base + RVA_PLACEJOIN, P_PLACEJOIN, (int)sizeof(P_PLACEJOIN), 15000) < 0) {
        ff13loga("[FF13-SRP] actor placement join @0x%08X never matched -> an unplaced enemy stays "
                 "unplaced\n", (unsigned)(g_base + RVA_PLACEJOIN));
    } else {
        void* pj = ff13_install_esp_hook(g_base + RVA_PLACEJOIN, P_PLACEJOIN,
                                         (int)sizeof(P_PLACEJOIN), (void*)on_place_join);
        (pj ? ff13logv : ff13loga)("[FF13-SRP] actor placement join @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_PLACEJOIN), pj ? "OK" : "FAIL");
    }

    // The game's own warnings, captured during substituted fights (see on_game_log).
    int waitedGl = wait_for_bytes(g_base + RVA_GAMELOG, P_GLOG, (int)sizeof(P_GLOG), 15000);
    if (waitedGl < 0) {
        ff13loga("[FF13-SRP] game message printer @0x%08X never matched -> not captured\n",
                 (unsigned)(g_base + RVA_GAMELOG));
    } else {
        // valOff 0x14 = the format string; ptrOff1 8 = &file, which is the whole argument block
        // (file, line, category, fmt, varargs...) as one array.
        void* gl = ff13_install_argptr_hook(g_base + RVA_GAMELOG, P_GLOG, (int)sizeof(P_GLOG),
                                            (void*)on_game_log, 0x14, 8, 8);
        (gl ? ff13logv : ff13loga)("[FF13-SRP] game message hook @0x%08X: %s (counting always; written during a "
                 "battle at log level verbose, which is %s)\n",
                 (unsigned)(g_base + RVA_GAMELOG), gl ? "OK" : "FAIL",
                 g_logLevel >= 2 ? "on" : "off");
    }
    // BGM: log every track the game starts, and (while a swap fired) substitute the battle theme.
    // The music is chosen independently of the battle scene, so this is what makes a swapped-in
    // boss actually sound like one.
    int waitedBg = wait_for_bytes(g_base + RVA_BGMPLAY, P_BGM, (int)sizeof(P_BGM), 15000);
    if (waitedBg < 0) {
        ff13loga("[FF13-SRP] BGM play entry @0x%08X never matched -> no BGM log/swap\n",
                 (unsigned)(g_base + RVA_BGMPLAY));
    } else {
        // valOff 0 = return address, ptrOff1 8 = &arg2 (the name), ptrOff2 4 = &arg1 (unused).
        void* bg = ff13_install_argptr_hook(g_base + RVA_BGMPLAY, P_BGM, (int)sizeof(P_BGM),
                                            (void*)on_bgm_play, 0, 8, 4);
        (bg ? ff13logv : ff13loga)("[FF13-SRP] BGM hook @0x%08X: %s (every picked fight gets the right track for "
                 "it)\n", (unsigned)(g_base + RVA_BGMPLAY), bg ? "OK" : "FAIL");
    }

    // lazyPlay is the path most fight tracks actually start on; without this listener the learner
    // only ever saw the rare direct plays.
    int waitedLz = wait_for_bytes(g_base + RVA_BGMLAZY, P_BGMLAZY, (int)sizeof(P_BGMLAZY), 15000);
    if (waitedLz < 0) {
        ff13loga("[FF13-SRP] BGM lazyPlay @0x%08X never matched -> fights learned only off direct "
                 "plays\n", (unsigned)(g_base + RVA_BGMLAZY));
    } else {
        // valOff 0 = return address, ptrOff1 4 = &arg1 (the name), ptrOff2 8 = &arg2 (the fade).
        void* lz = ff13_install_argptr_hook(g_base + RVA_BGMLAZY, P_BGMLAZY, (int)sizeof(P_BGMLAZY),
                                            (void*)on_bgm_lazyplay, 0, 4, 8);
        (lz ? ff13logv : ff13loga)("[FF13-SRP] BGM lazyPlay hook @0x%08X: %s (real fights teach "
                 "their tracks)\n", (unsigned)(g_base + RVA_BGMLAZY), lz ? "OK" : "FAIL");
    }

    // Resource.loadSoundResource: learn the real (name, group) argument pair, so we can call it
    // ourselves to make an unloaded track playable.
    int waitedLs = wait_for_bytes(g_base + RVA_LOADSND, P_LSND, (int)sizeof(P_LSND), 15000);
    if (waitedLs < 0) {
        ff13loga("[FF13-SRP] loadSoundResource @0x%08X never matched -> no sound-load log\n",
                 (unsigned)(g_base + RVA_LOADSND));
    } else {
        void* ls = ff13_install_stackarg_hook(g_base + RVA_LOADSND, P_LSND, (int)sizeof(P_LSND),
                                              (void*)on_load_sound, 4);
        (ls ? ff13logv : ff13loga)("[FF13-SRP] loadSoundResource log hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_LOADSND), ls ? "OK" : "FAIL");
    }

    // Capture doEncountCheck's field manager (the battle picker posts a pending touch to it).
    static const uint8_t P_ECHK[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x34};
    int waitedEc = wait_for_bytes(g_base + RVA_ENCCHECK, P_ECHK, (int)sizeof(P_ECHK), 15000);
    if (waitedEc < 0) {
        ff13loga("[FF13-SRP] doEncountCheck @0x%08X never matched -> battle picker cannot start fights\n",
                 (unsigned)(g_base + RVA_ENCCHECK));
    } else {
        void* ec = ff13_install_ecx_hook(g_base + RVA_ENCCHECK, P_ECHK, (int)sizeof(P_ECHK),
                                         (void*)on_encount_check);
        (ec ? ff13logv : ff13loga)("[FF13-SRP] doEncountCheck capture hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ENCCHECK), ec ? "OK" : "FAIL");
    }

    int waitedLc = wait_for_bytes(g_base + RVA_LOADCHARASET, P_LCS, (int)sizeof(P_LCS), 15000);
    if (waitedLc < 0) {
        ff13loga("[FF13-SRP] loadCharaByCharaSet @0x%08X never matched -> no charaset log\n",
                 (unsigned)(g_base + RVA_LOADCHARASET));
    } else {
        void* lc = ff13_install_stackarg_hook(g_base + RVA_LOADCHARASET, P_LCS, (int)sizeof(P_LCS),
                                              (void*)on_load_charaset, 4);
        (lc ? ff13logv : ff13loga)("[FF13-SRP] loadCharaByCharaSet log hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_LOADCHARASET), lc ? "OK" : "FAIL");
    }

    int waitedLb = wait_for_bytes(g_base + RVA_LOADBG, P_LBG, (int)sizeof(P_LBG), 15000);
    if (waitedLb < 0) {
        ff13loga("[FF13-SRP] loadBg @0x%08X never matched -> no mapset log\n",
                 (unsigned)(g_base + RVA_LOADBG));
    } else {
        void* lb = ff13_install_stackarg_hook(g_base + RVA_LOADBG, P_LBG, (int)sizeof(P_LBG),
                                              (void*)on_load_bg, 4);
        (lb ? ff13logv : ff13loga)("[FF13-SRP] loadBg log hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_LOADBG), lb ? "OK" : "FAIL");
    }

    {
        static const uint8_t P_DEBG[9] = {0x55,0x8B,0xEC,0x81,0xEC,0x90,0x00,0x00,0x00};
        static const uint8_t P_DECH[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x58};
        int w1 = wait_for_bytes(g_base + RVA_DRAWENV_BG, P_DEBG, (int)sizeof(P_DEBG), 15000);
        int w2 = wait_for_bytes(g_base + RVA_DRAWENV_CHARA, P_DECH, (int)sizeof(P_DECH), 15000);
        void* h1 = (w1 < 0) ? NULL
            : ff13_install_stackarg_hook(g_base + RVA_DRAWENV_BG, P_DEBG, (int)sizeof(P_DEBG),
                                         (void*)on_drawenv_bg, 4);
        void* h2 = (w2 < 0) ? NULL
            : ff13_install_stackarg_hook(g_base + RVA_DRAWENV_CHARA, P_DECH, (int)sizeof(P_DECH),
                                         (void*)on_drawenv_chara, 4);
        (h1 && h2 ? ff13logv : ff13loga)("[FF13-SRP] draw-env log hooks @0x%08X/0x%08X: %s/%s\n",
                 (unsigned)(g_base + RVA_DRAWENV_BG), (unsigned)(g_base + RVA_DRAWENV_CHARA),
                 h1 ? "OK" : "FAIL", h2 ? "OK" : "FAIL");
    }

    {
        static const uint8_t P_BOX[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x28};
        static const uint8_t P_SEN[6] = {0x55,0x8B,0xEC,0x83,0xEC,0x18};
        int wb1 = wait_for_bytes(g_base + RVA_BOX_ON,  P_BOX, (int)sizeof(P_BOX), 15000);
        int wb2 = wait_for_bytes(g_base + RVA_BOX_OFF, P_BOX, (int)sizeof(P_BOX), 15000);
        int wb3 = wait_for_bytes(g_base + RVA_SCREENENV, P_SEN, (int)sizeof(P_SEN), 15000);
        void* b1 = (wb1 < 0) ? NULL : ff13_install_stackarg_hook(g_base + RVA_BOX_ON, P_BOX,
                        (int)sizeof(P_BOX), (void*)on_box_on, 4);
        void* b2 = (wb2 < 0) ? NULL : ff13_install_stackarg_hook(g_base + RVA_BOX_OFF, P_BOX,
                        (int)sizeof(P_BOX), (void*)on_box_off, 4);
        void* b3 = (wb3 < 0) ? NULL : ff13_install_stackarg_hook(g_base + RVA_SCREENENV, P_SEN,
                        (int)sizeof(P_SEN), (void*)on_screen_env, 4);
        int wl1 = wait_for_bytes(g_base + RVA_LAYOUT_ON,  P_BOX, (int)sizeof(P_BOX), 15000);
        int wl2 = wait_for_bytes(g_base + RVA_LAYOUT_OFF, P_BOX, (int)sizeof(P_BOX), 15000);
        void* l1 = (wl1 < 0) ? NULL : ff13_install_stackarg_hook(g_base + RVA_LAYOUT_ON, P_BOX,
                        (int)sizeof(P_BOX), (void*)on_layout_on, 4);
        void* l2 = (wl2 < 0) ? NULL : ff13_install_stackarg_hook(g_base + RVA_LAYOUT_OFF, P_BOX,
                        (int)sizeof(P_BOX), (void*)on_layout_off, 4);
        (b1 && b2 && b3 && l1 && l2 ? ff13logv : ff13loga)("[FF13-SRP] env log hooks (object on/off, screen env, layout show/hide): "
                 "%s/%s/%s/%s/%s\n", b1 ? "OK" : "FAIL", b2 ? "OK" : "FAIL", b3 ? "OK" : "FAIL",
                 l1 ? "OK" : "FAIL", l2 ? "OK" : "FAIL");
    }

    int waitedEi = wait_for_bytes(g_base + RVA_ENCID, P_ENCID, (int)sizeof(P_ENCID), 15000);
    if (waitedEi < 0) {
        ff13loga("[FF13-SRP] encounter-id entry @0x%08X never matched -> no touched-object log\n",
                 (unsigned)(g_base + RVA_ENCID));
    } else {
        void* ei = ff13_install_stackarg_hook(g_base + RVA_ENCID, P_ENCID, (int)sizeof(P_ENCID),
                                              (void*)on_encount_id, 4);
        (ei ? ff13logv : ff13loga)("[FF13-SRP] encounter-id log hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_ENCID), ei ? "OK" : "FAIL");
    }

#ifdef FF13_DIAG
    // Verbose only, so there is no hot-path overhead in normal use.
    if (g_logLevel >= 2) {
        void* dcave = ff13_install_ecx_hook(g_base + RVA_APPLYHP, P_APPLYHP,
                                            (int)sizeof(P_APPLYHP), (void*)on_apply_hp_delta);
        (dcave ? ff13logv : ff13loga)("[FF13-SRP] verbose diagnostic hook @0x%08X: %s\n",
                 (unsigned)(g_base + RVA_APPLYHP), dcave ? "OK" : "FAIL");
    }
#endif // FF13_DIAG

    // Save anywhere: its own eight hooks (save / load / zone state / charpool / setpos / save
    // screen / postcam x2 / reconcile), each waiting on its own prologue. See srp_save.c.
    save_install_hooks();

    overlay_init();

    ff13loga("[FF13-SRP] ready. help 0x%X = the hotkey list; kill_mode 0x%X = cycling OFF -> "
             "INSTANT WIN (+ one-hit-kill, "
             "arm it BEFORE the encounter; gate %s) -> ONE-HIT KILL -> OFF, now %s; enemy_suppress "
             "0x%X = every " SUPPRESS_PREFIX "* group; game_speed 0x%X = %s then off; "
             "warp 0x%X = warp (tap) / record (hold), field + same-zone; battle_overlay 0x%X = "
             "timer+HP toggle; battle_picker 0x%X = open the fight picker; save 0x%X = save where "
             "you stand and come back to it; camera_unstick 0x%X = free the camera after one.\n",
             g_helpVk,
             g_armVk, g_iwInstGate ? "live" : "MISSING", kill_mode_name_en(g_killMode),
             g_supVk, g_gsVk, gs_steps_str(), g_warpVk, g_boVk,
             g_bsVk, g_saveVk, g_camUnstickVk);
    ff13loga("[FF13-SRP] hotkeys: %s\n", g_hkFocusOnly
             ? "only while the game window has focus ([hotkeys] require_focus = 0 to lift that)"
             : "always, focus or not ([hotkeys] require_focus = 0)");

    // Hotkey polling. Every press goes through hk_edge, which is also where "not while the game is
    // in the background" lives.
    int wasArm = 0, wasSup = 0, wasGs = 0, wasCancel = 0, wasBo = 0, wasHelp = 0;
    hk_tap tpKey = { 0, 0, 0 };                  // the warp key's tap-or-hold state
    hk_tap bsKey = { 0, 0, 0 };                  // ...and the picker key's, on the same hold time

    hk_rep bsUp = { 0, 0 }, bsDown = { 0, 0 },   // the four that repeat while held
           bsLeft = { 0, 0 }, bsRight = { 0, 0 };
    DWORD crashRearmAt = 0;
    for (;;) {
        Sleep(HK_POLL_MS);
        DWORD hkNow = GetTickCount();
        // The game installs its own filter after this mod's, so the seat is re-taken on a timer.
        if ((int)(hkNow - crashRearmAt) >= 0) { crashRearmAt = hkNow + 2000; crash_report_rearm(); }
        if (hk_edge(g_helpVk, &wasHelp)) {
            g_helpOpen = !g_helpOpen;
            ff13log("[FF13-SRP] hotkey list %s\n", g_helpOpen ? "shown" : "hidden");
        }

        // The kill-mode cycle, strongest first (user-facing order):
        // OFF -> INSTANT WIN (+ one-hit-kill) -> ONE-HIT KILL -> OFF.
        if (hk_edge(g_armVk, &wasArm)) {
#ifdef FF13_DIAG
            if (g_killMode == KILL_OFF) g_seenCount = 0;   // fresh verbose window per arm session
#endif
            kill_mode_apply(g_killMode == KILL_OFF     ? KILL_INSTANT :
                            g_killMode == KILL_INSTANT ? KILL_ONEHIT  : KILL_OFF, 1);
            state_save();                          // persist across restarts
        }

        // Enemy suppression has to be enabled BEFORE loading the save: what it intercepts is the
        // restore script's activateArea calls, which happen during the load.
        if (hk_edge(g_supVk, &wasSup)) {
            g_suppressEnabled = !g_suppressEnabled;
            ff13log("[FF13-SRP] enemy suppression %s (every " SUPPRESS_PREFIX "* group). "
                     "Reload the save to apply.\n", g_suppressEnabled ? "ON" : "OFF");
            state_save();                          // persist the toggle across restarts
        }

        // gs_clock reads g_gsEnabled on every clock call, so the change takes effect immediately
        // and continuously (the accumulator keeps the clock monotonic).
        if (hk_edge(g_gsVk, &wasGs)) {
            gs_apply(g_gsStep + 1);                // ...and past the last one is off
            if (g_gsEnabled)
                ff13log("[FF13-SRP] game-speed ON at %.2fx (step %d of %d).\n",
                         g_gsFactor, g_gsStep + 1, g_gsStepN);
            else
                ff13log("[FF13-SRP] game-speed OFF.\n");
            state_save();                          // persist the step across restarts
        }

        // The in-battle overlay (per-fight GAME/REAL timer + enemy HP) toggles as one group.
        if (hk_edge(g_boVk, &wasBo)) {
            g_battleOverlay = !g_battleOverlay;
            ff13log("[FF13-SRP] battle overlay (timer + enemy HP) %s.\n", g_battleOverlay ? "ON" : "OFF");
            if (g_battleOverlay) flash_say(ui_text(T_BATTLE_OVL_ON));
            else                 flash_say_c(ui_text(T_BATTLE_OVL_OFF), OVL_FLASH_OFF);
            state_save();                          // persist the toggle across restarts
        }

        // The battle picker, on one key with the same tap-or-hold shape and hold time as the warp
        // key. TAP = open, or close what is open. HOLD = fight, list up or shut (shut takes the
        // row the list would have opened on). Only an empty list opens instead.
        // All game-state work happens on the game thread in bs_pump(); this only raises a request.
        {
            int b = hk_taphold(g_bsVk, &bsKey, hkNow, WARP_HOLD_MS);
            // Logged before anything else is touched, so a press is visible in the log even when
            // the request goes nowhere -- otherwise "the picker does not open" and "the key never
            // arrived" look identical.
            if (b) ff13log("[FF13-SRP] battle picker: key 0x%X %s (open=%d)\n",
                            g_bsVk, b == 2 ? "held" : "tapped", g_bsOpen);
            if (b == 2) {
                g_bsReq = g_bsOpen ? 2 : 3;        // start: the cursor's row / the remembered row
            } else if (b) {
                // A toggle decided on the VISIBLE state: the list opens on the game thread a moment
                // later, so while the open request is in flight g_bsOpen is 0 and a double press
                // asks to open twice rather than closing what has not appeared yet.
                // Closing remembers the row, as the cancel key below does.
                if (g_bsOpen) {
                    g_bsRememberDue = 1;       // the game thread does it: see pump_picker_input
                    g_bsOpen = 0;
                } else {
                    g_bsReq = 1;
                }
            }
        }

        // Stepped every tick, open or shut, because the down/up state has to stay honest: asked
        // only while the picker is up, a key already held when it opens looks like a fresh press.
        int bsUpFire    = hk_held(g_bsUpVk,    &bsUp,    hkNow);
        int bsDownFire  = hk_held(g_bsDownVk,  &bsDown,  hkNow);
        int bsLeftFire  = hk_held(g_bsLeftVk,  &bsLeft,  hkNow);
        int bsRightFire = hk_held(g_bsRightVk, &bsRight, hkNow);
        if (g_bsOpen && g_bsCount > 0) {
            if (bsUpFire)   g_bsSel = (g_bsSel > 0) ? g_bsSel - 1 : g_bsCount - 1;
            if (bsDownFire) g_bsSel = (g_bsSel + 1 < g_bsCount) ? g_bsSel + 1 : 0;
            // User-facing: a single step wraps, a PAGE stops at either end. Paging is how you
            // cross a long list, and a wrap there throws away where you were.
            if (bsLeftFire) {
                g_bsSel = (g_bsSel > BS_ROWS) ? g_bsSel - BS_ROWS : 0;
            }
            if (bsRightFire) {
                int last = g_bsCount - 1;
                g_bsSel = (g_bsSel + BS_ROWS < last) ? g_bsSel + BS_ROWS : last;
            }
        }

        // The way back from a picked fight, once the game thread says the field is settled. On this
        // thread rather than that one because the warp holds the commit override for ~700ms and the
        // game thread is the one doing the committing.
        if (g_homeDue == 2) {
            g_homeDue = 0; g_homeHave = 0;
            home_warp_back();
        }
        // The warp key, field-only and same-zone. HOLD records where you stand, straight away.
        // TAP warps to this zone's point and asks first; tap again and it goes.
        {
            int g = hk_taphold(g_warpVk, &tpKey, hkNow, WARP_HOLD_MS);
            if (g == 2) {
                // A hold outranks a warp question left open: dropped silently rather than through
                // warp_cancel, which would flash "cancelled" a frame before "recorded".
                g_warpPrompt = 0;
                warp_record();
            } else if (g == 1) {
                if (g_warpPrompt == 2) warp_confirm();      // the question is up; a tap is the answer
                else                 warp_ask();
            }
        }
        // There is no confirm key: each question is answered by the key that raised it. Cancel
        // closes the picker as well as the prompt -- never both at once, since the picker is closed
        // while a warp prompt is up.
        if (hk_edge(g_warpCancelVk, &wasCancel)) {
            if (g_warpPrompt)    warp_cancel();
            else if (g_bsOpen) {
                g_bsRememberDue = 1;           // ...likewise
                g_bsOpen = 0;      // no flash: the list vanishing already says it closed
            }
        }
        if (g_warpPrompt && (int)(g_warpPromptUntil - GetTickCount()) <= 0) warp_cancel();  // auto-cancel
        // Save anywhere rides this same tick: its hotkeys, the restore state machine and the
        // deferred file work all live in there (srp_save.c).
        save_poll_tick(hkNow);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        ff13loga("[FF13-SRP] DLL_PROCESS_ATTACH\n");
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        // DO NOTHING here. On process exit (reserved != NULL) this runs under the loader lock with
        // the other threads killed and the CRT half torn down: logging can deadlock, and clearing a
        // bit in memory about to be unmapped buys nothing.
        // The code caves are deliberately left in place either way -- pulling them out from under a
        // running game is the dangerous thing.
    }
    return TRUE;
}
