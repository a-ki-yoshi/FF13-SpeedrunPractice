/*
 * FF13 mods — shared modules for the FINAL FANTASY XIII / XIII-2 practice mods (Steam).
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

// Minimal inline-hook installers.
#ifndef FF13_HOOK_H
#define FF13_HOOK_H
#include <stdint.h>

// Install an inline hook at `hookAddr` (an absolute VA; ASLR is off so these are
// fixed).  Builds an executable cave that:
//   pushad; pushfd; push ecx; call handler; add esp,4; popfd; popad;
//   <original prologue bytes>; jmp (hookAddr + prologueLen)
// then overwrites the prologue with `jmp cave` (+ nop padding).
// The handler is `void __cdecl handler(uint32_t ecx)`.
// SAFETY: the current bytes at hookAddr are compared to `prologue` first; on any
// mismatch nothing is patched and 0 is returned.  prologueLen must be >= 5.
// Returns the cave address on success, 0 on failure.
void* ff13_install_ecx_hook(uintptr_t hookAddr, const uint8_t* prologue,
                            int prologueLen, void* handler);

// Same, but instead of ecx it passes the original stack value at [esp+stackOff]
// (the value there at the hooked instruction, before our pushad) to the handler.
// Use for hooks that read function arguments off the stack (e.g. the zone-state
// transition at 0x80F340 reads the new state at [esp+8]).
void* ff13_install_stackarg_hook(uintptr_t hookAddr, const uint8_t* prologue,
                                 int prologueLen, void* handler, int stackOff);

// Passes BOTH ecx and the original [esp+stackOff] to handler(ecx, stackVal).
// Use for the position-commit hook 0xD6D2D0 (ecx=object, [esp+4]=Vector4* pos).
void* ff13_install_ecx_stackarg_hook(uintptr_t hookAddr, const uint8_t* prologue,
                                     int prologueLen, void* handler, int stackOff);

// Passes the caller's ESP at the hooked instruction (before our pushad) to
// handler(uint32_t esp).  For hooks that need a stack slot too far out for the
// disp8 forms above -- the encounter camera at 0x8136BD reads its matrix at
// [esp+0x1D0] -- and for hooks that want to write one back: the handler holds a
// real pointer into the caller's frame, and popad restores registers only, so a
// write through it survives.
void* ff13_install_esp_hook(uintptr_t hookAddr, const uint8_t* prologue,
                            int prologueLen, void* handler);

// Passes ecx, one stack VALUE [esp+valOff], and the ADDRESSES of two stack args
// &[esp+ptrOff1] / &[esp+ptrOff2] to handler(ecx, val, uint32_t* p1, uint32_t* p2).
// Because popad restores only registers (not the stack), a value the handler writes
// through p1/p2 into the caller's stack args survives into the hooked function. Used by
// the reconcile hook 0x42b9a0 to append a cell to desired[] (p1) and bump count (p2).
// Offsets are relative to the hooked instruction (before our pushad). prologueLen >= 5.
void* ff13_install_argptr_hook(uintptr_t hookAddr, const uint8_t* prologue, int prologueLen,
                               void* handler, int valOff, int ptrOff1, int ptrOff2);

// Conditional REPLACEMENT hook at a function's ENTRY. Unlike every hook above, the handler decides
// whether the original function runs at all:
//   int __cdecl handler(uint32_t ecx, uint32_t esp, int* retval)
//     ecx    = ecx at the hook site (the `this` of a thiscall entry)
//     esp    = the CALLER's esp there, so [esp] is the return address and [esp+4] is the first
//              stack argument; writes through it survive, popad restores registers only
//     retval = where to put the eax the caller will see. It is NOT pre-initialised, so a handler
//              that returns non-zero MUST write it.
//   returns 0        -> the relocated prologue runs and execution falls into the original function
//   returns non-zero -> the original never runs: eax = *retval and the cave does `ret retBytes`
// retBytes is the ORIGINAL function's own stack cleanup (0 = cdecl/`ret`, N = `ret N`); a wrong
// value corrupts the caller's stack. As with the others, the bytes at hookAddr must equal
// `prologue` (>= 5, whole instructions, position-independent) or nothing is patched and 0 returned.
void* ff13_install_replace_hook(uintptr_t hookAddr, const uint8_t* prologue, int prologueLen,
                                void* handler, int retBytes);

// Save-screen hook at the yield-function entry 0x40FA50 (prologue 55 8B EC 83 EC 40).
// Byte-only cave: when *openFlag==1 && *inDoSave==0, clears openFlag, sets inDoSave, and calls
// the interactive save entry `doSaveAddr` (0x40EA40) in the yield coroutine context
// (pushad/pushfd, ambient ecx=GameMain), then clears inDoSave. No C handler on purpose: doSave
// has to run at exactly this stack level or the yield freezes. Returns the cave (nonzero) on
// success.
// Also emits a camera-recenter block: when *camResetAddr==1 (and *camSingletonAddr is
// non-null), calls recenterAddr (0x636A20) with ecx=that singleton at this yield point
// (game thread), then clears the flag. Pass camResetAddr=0 to omit the block.
void* ff13_install_savescreen_hook(uintptr_t hookAddr, uintptr_t doSaveAddr,
                                   uintptr_t openFlagAddr, uintptr_t inDoSaveAddr,
                                   uintptr_t recenterAddr, uintptr_t camSingletonAddr,
                                   uintptr_t camResetAddr);

#endif
