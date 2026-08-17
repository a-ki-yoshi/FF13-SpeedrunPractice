/*
 * FF13-AnywhereSave — save-anywhere mod for FINAL FANTASY XIII (Steam).
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

#include <windows.h>
#include <string.h>
#include "ff13_hook.h"

// Core builder. `pushSeq`/`pushLen` = bytes that push the cdecl args (after
// pushad+pushfd); `argWords` = number of 4-byte args to clean afterwards. Cave:
//   pushad; pushfd; <pushSeq>; call handler; add esp,argWords*4; popfd; popad;
//   <original prologue>; jmp (hookAddr+prologueLen)
// then patch hookAddr with `jmp cave` (+ nop padding). Verifies the prologue first.
static void* build(uintptr_t hookAddr, const uint8_t* prologue, int prologueLen,
                   void* handler, const uint8_t* pushSeq, int pushLen, int argWords) {
    if (prologueLen < 5) return 0;
    if (memcmp((const void*)hookAddr, prologue, (size_t)prologueLen) != 0) return 0;

    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) return 0;

    uint8_t* p = cave;
    *p++ = 0x60;                         // pushad
    *p++ = 0x9C;                         // pushfd
    memcpy(p, pushSeq, (size_t)pushLen); p += pushLen;
    *p++ = 0xE8;                         // call rel32 -> handler
    int32_t relCall = (int32_t)((uintptr_t)handler - ((uintptr_t)p + 4));
    memcpy(p, &relCall, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = (uint8_t)(argWords * 4);   // add esp, argWords*4
    *p++ = 0x9D;                         // popfd
    *p++ = 0x61;                         // popad
    memcpy(p, prologue, (size_t)prologueLen); p += prologueLen;
    *p++ = 0xE9;                         // jmp rel32 -> hookAddr + prologueLen
    int32_t relBack = (int32_t)((hookAddr + prologueLen) - ((uintptr_t)p + 4));
    memcpy(p, &relBack, 4); p += 4;

    DWORD old;
    if (!VirtualProtect((void*)hookAddr, (size_t)prologueLen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return 0;
    }
    uint8_t* h = (uint8_t*)hookAddr;
    h[0] = 0xE9;
    int32_t relHook = (int32_t)((uintptr_t)cave - (hookAddr + 5));
    memcpy(h + 1, &relHook, 4);
    for (int i = 5; i < prologueLen; i++) h[i] = 0x90;
    VirtualProtect((void*)hookAddr, (size_t)prologueLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)hookAddr, (size_t)prologueLen);
    return cave;
}

void* ff13_install_ecx_hook(uintptr_t hookAddr, const uint8_t* prologue,
                            int prologueLen, void* handler) {
    static const uint8_t pushEcx[] = {0x51};   // push ecx
    return build(hookAddr, prologue, prologueLen, handler, pushEcx, 1, 1);
}

void* ff13_install_stackarg_hook(uintptr_t hookAddr, const uint8_t* prologue,
                                 int prologueLen, void* handler, int stackOff) {
    int disp = 0x24 + stackOff;
    if (disp < 0 || disp > 0x7F) return 0;
    uint8_t pushArg[] = {0xFF, 0x74, 0x24, (uint8_t)disp};   // push dword [esp+disp8]
    return build(hookAddr, prologue, prologueLen, handler, pushArg, 4, 1);
}

void* ff13_install_ecx_stackarg_hook(uintptr_t hookAddr, const uint8_t* prologue,
                                     int prologueLen, void* handler, int stackOff) {
    // handler(ecx, [esp+stackOff]): push arg2 (stack) then arg1 (ecx).
    int disp = 0x24 + stackOff;
    if (disp < 0 || disp > 0x7F) return 0;
    uint8_t seq[] = {0xFF, 0x74, 0x24, (uint8_t)disp, 0x51};  // push [esp+disp8]; push ecx
    return build(hookAddr, prologue, prologueLen, handler, seq, 5, 2);
}

void* ff13_install_esp_hook(uintptr_t hookAddr, const uint8_t* prologue,
                            int prologueLen, void* handler) {
    // pushad+pushfd have added 0x24, so [esp+0x24] is where esp stood at the hook.
    static const uint8_t seq[] = {0x8D, 0x44, 0x24, 0x24, 0x50};   // lea eax,[esp+0x24]; push eax
    return build(hookAddr, prologue, prologueLen, handler, seq, (int)sizeof(seq), 1);
}

// handler(ecx, [esp+valOff], &[esp+ptrOff1], &[esp+ptrOff2]). Pushes (cdecl, right->left):
// &ptr2, &ptr1, val, ecx. At the cave, pushad+pushfd have added 0x24, and each push shifts
// esp -4, so the displacements below compensate. The two ADDRESS args let the handler rewrite
// the caller's stack args in place (popad restores registers only, not the stack).
void* ff13_install_argptr_hook(uintptr_t hookAddr, const uint8_t* prologue, int prologueLen,
                               void* handler, int valOff, int ptrOff1, int ptrOff2) {
    int d2 = 0x24 + ptrOff2;          // &[ptrOff2], pushed first (esp unshifted)
    int d1 = 0x24 + ptrOff1 + 4;      // &[ptrOff1], after 1 push
    int dv = 0x24 + valOff  + 8;      // [valOff] value, after 2 pushes
    if (d2 < 0 || d2 > 0x7F || d1 < 0 || d1 > 0x7F || dv < 0 || dv > 0x7F) return 0;
    uint8_t seq[] = {
        0x8D, 0x44, 0x24, (uint8_t)d2, 0x50,   // lea eax,[esp+d2]; push eax   (&ptr2)
        0x8D, 0x44, 0x24, (uint8_t)d1, 0x50,   // lea eax,[esp+d1]; push eax   (&ptr1)
        0xFF, 0x74, 0x24, (uint8_t)dv,         // push dword [esp+dv]          (val)
        0x51,                                   // push ecx
    };
    return build(hookAddr, prologue, prologueLen, handler, seq, (int)sizeof(seq), 4);
}

void* ff13_install_savescreen_hook(uintptr_t hookAddr, uintptr_t doSaveAddr,
                                   uintptr_t openFlagAddr, uintptr_t inDoSaveAddr,
                                   uintptr_t recenterAddr, uintptr_t camSingletonAddr,
                                   uintptr_t camResetAddr) {
    static const uint8_t prologue[] = {0x55,0x8B,0xEC,0x83,0xEC,0x40};
    if (memcmp((const void*)hookAddr, prologue, 6) != 0) return 0;
    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) return 0;
    uint8_t* p = cave;
    uint32_t of = (uint32_t)openFlagAddr, ids = (uint32_t)inDoSaveAddr, zero = 0, one = 1;
    uint32_t cr = (uint32_t)camResetAddr, cs = (uint32_t)camSingletonAddr;
    uint8_t *jne1, *jne2, *jneCam, *jeCam;

    // --- save block: if openFlag && !inDoSave, open the save screen via doSave ---
    *p++=0x83; *p++=0x3D; memcpy(p,&of,4); p+=4; *p++=0x01;      // cmp [openFlag],1
    *p++=0x0F; *p++=0x85; jne1=p; p+=4;                          // jne AFTERSAVE
    *p++=0x83; *p++=0x3D; memcpy(p,&ids,4); p+=4; *p++=0x00;     // cmp [inDoSave],0
    *p++=0x0F; *p++=0x85; jne2=p; p+=4;                          // jne AFTERSAVE
    *p++=0xC7; *p++=0x05; memcpy(p,&of,4);  p+=4; memcpy(p,&zero,4); p+=4;  // mov [openFlag],0
    *p++=0xC7; *p++=0x05; memcpy(p,&ids,4); p+=4; memcpy(p,&one,4);  p+=4;  // mov [inDoSave],1
    *p++=0x60; *p++=0x9C;                                        // pushad; pushfd
    *p++=0xE8; { int32_t r=(int32_t)(doSaveAddr-((uintptr_t)p+4)); memcpy(p,&r,4); p+=4; } // call doSave
    *p++=0x9D; *p++=0x61;                                        // popfd; popad
    *p++=0xC7; *p++=0x05; memcpy(p,&ids,4); p+=4; memcpy(p,&zero,4); p+=4;  // mov [inDoSave],0

    // --- AFTERSAVE: camera-recenter block. If camReset flag set (and singleton valid),
    //     call the engine recenter 0x636A20 at this yield point (game thread). ---
    uint8_t* AFTERSAVE = p;
    { int32_t r=(int32_t)(AFTERSAVE-(jne1+4)); memcpy(jne1,&r,4); }
    { int32_t r=(int32_t)(AFTERSAVE-(jne2+4)); memcpy(jne2,&r,4); }
    *p++=0x83; *p++=0x3D; memcpy(p,&cr,4); p+=4; *p++=0x01;      // cmp [camReset],1
    *p++=0x0F; *p++=0x85; jneCam=p; p+=4;                        // jne FINAL
    *p++=0xC7; *p++=0x05; memcpy(p,&cr,4); p+=4; memcpy(p,&zero,4); p+=4;  // mov [camReset],0
    *p++=0xA1; memcpy(p,&cs,4); p+=4;                            // mov eax,[camSingleton]
    *p++=0x85; *p++=0xC0;                                        // test eax,eax
    *p++=0x0F; *p++=0x84; jeCam=p; p+=4;                         // je FINAL
    *p++=0x60; *p++=0x9C;                                        // pushad; pushfd
    *p++=0x8B; *p++=0xC8;                                        // mov ecx,eax (this=singleton)
    *p++=0xE8; { int32_t r=(int32_t)(recenterAddr-((uintptr_t)p+4)); memcpy(p,&r,4); p+=4; } // call 0x636A20
    *p++=0x9D; *p++=0x61;                                        // popfd; popad

    // --- FINAL: original prologue + jmp back ---
    uint8_t* FINAL = p;
    { int32_t r=(int32_t)(FINAL-(jneCam+4)); memcpy(jneCam,&r,4); }
    { int32_t r=(int32_t)(FINAL-(jeCam+4)); memcpy(jeCam,&r,4); }
    *p++=0x55; *p++=0x8B; *p++=0xEC; *p++=0x83; *p++=0xEC; *p++=0x40;  // original prologue
    *p++=0xE9; { int32_t r=(int32_t)((hookAddr+6)-((uintptr_t)p+4)); memcpy(p,&r,4); p+=4; } // jmp back

    DWORD old;
    if (!VirtualProtect((void*)hookAddr, 6, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(cave, 0, MEM_RELEASE); return 0;
    }
    uint8_t* h = (uint8_t*)hookAddr;
    h[0]=0xE9; { int32_t r=(int32_t)((uintptr_t)cave-(hookAddr+5)); memcpy(h+1,&r,4); }
    h[5]=0x90;
    VirtualProtect((void*)hookAddr, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)hookAddr, 6);
    return cave;
}
