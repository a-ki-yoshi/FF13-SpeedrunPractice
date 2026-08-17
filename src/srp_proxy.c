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

// ---- proxy: forward DirectInput8Create to the real system dinput8 ----
// The full System32 path is required: a bare name would re-enter our own app-dir copy.
typedef HRESULT (WINAPI *DI8C_t)(HINSTANCE, DWORD, const void*, void**, void*);
static DI8C_t g_real = NULL;

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver,
                                                        const void* riid, void** out, void* punk) {
    if (!g_real) {
        HMODULE m = LoadLibraryA("C:\\Windows\\System32\\dinput8.dll");
        if (m) g_real = (DI8C_t)GetProcAddress(m, "DirectInput8Create");
    }
    if (!g_real) return (HRESULT)0x80004005L; // E_FAIL
    return g_real(hinst, ver, riid, out, punk);
}
