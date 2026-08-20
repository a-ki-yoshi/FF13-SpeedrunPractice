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

// ---- the input gate ---------------------------------------------------------------------------
// Input is silenced HERE, at the DirectInput proxy, and not game-side through Chara.switchControl,
// which crashed on hardware (CODE_NOTES, the input-gate section). Everything the game plays with
// comes through this DLL: keyboard and mouse are DirectInput devices too, there is no XInput or
// RawInput, and the exe's one GetAsyncKeyState call checks VK_LCONTROL only. The mod's hotkeys poll
// GetAsyncKeyState, so they keep working while the gate is up.
//
// The gate expires on its own: a stuck gate would eat even the Esc that opens the quit dialog,
// so a bug elsewhere must not be able to leave the player sealed in.
#define DI_GATE_MAX_MS 30000

static volatile int   g_diGate      = 0;
static volatile DWORD g_diGateUntil = 0;

static volatile int g_diSaid = 0;      // one "a read was silenced" line per gate, not per frame
void di_gate_arm(void)  { g_diSaid = 0; g_diGateUntil = GetTickCount() + DI_GATE_MAX_MS; g_diGate = 1; }
// For a witness line elsewhere: is input being eaten right now, and for how much longer.
int di_gate_left_ms(void) {
    if (!g_diGate) return -1;
    int left = (int)(g_diGateUntil - GetTickCount());
    return left > 0 ? left : 0;
}
void di_gate_lift(void) { g_diGate = 0; }

static int di_gated(void) {
    if (!g_diGate) return 0;
    if ((int)(GetTickCount() - g_diGateUntil) >= 0) { g_diGate = 0; return 0; }
    return 1;
}

// What CreateDevice was asked for decides how a silenced read looks: zeroed keys and zeroed
// relative mouse axes ARE neutral, but a pad's absolute stick axes are not -- zero can be a full
// deflection, so those get the centre of the range the game itself configured.
#define DI_KIND_KBD   1
#define DI_KIND_MOUSE 2
#define DI_KIND_PAD   3
static const uint8_t kGuidSysMouse[16]    = {0x60,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,
                                             0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
static const uint8_t kGuidSysKeyboard[16] = {0x61,0x2B,0x1D,0x6F,0xA0,0xD5,0xCF,0x11,
                                             0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};

#define DI_DEVS 12
static struct { void* dev; int kind; int haveC; LONG center[8]; } g_diDev[DI_DEVS];
static volatile int g_diDevN = 0;

// vtable slots, fixed by the COM ABI: IDirectInput8::CreateDevice = 3;
// IDirectInputDevice8::GetProperty = 5, GetDeviceState = 9, GetDeviceData = 10.
typedef HRESULT (__stdcall *DICreateDev_t)(void*, const void*, void**, void*);
typedef HRESULT (__stdcall *DIGetProp_t)(void*, const void*, void*);
typedef HRESULT (__stdcall *DIGetState_t)(void*, DWORD, void*);
typedef HRESULT (__stdcall *DIGetData_t)(void*, DWORD, void*, DWORD*, DWORD);

// The device classes inside dinput8.dll share vtables, so the originals are kept per VTABLE, not
// per device. Entries are published (g_diVtN raised) BEFORE the vtable is patched: the reverse
// order would let another thread land in the hook while its original is still unfindable.
#define DI_VTS 4
static struct { void** vt; DIGetState_t state; DIGetData_t data; } g_diVt[DI_VTS];
static volatile int g_diVtN = 0;

static int di_dev_find(void* dev) {
    for (int i = 0; i < g_diDevN; i++) if (g_diDev[i].dev == dev) return i;
    return -1;
}

// DIPROP_RANGE is MAKEDIPROP(4) -- a fake pointer, the API's own convention.
struct di_proprange { DWORD dwSize, dwHeaderSize, dwObj, dwHow; LONG lMin, lMax; };
#define DIPH_BYOFFSET 1

static void di_pad_centers(int di, void* dev) {
    if (g_diDev[di].haveC) return;
    void** vt = *(void***)dev;
    DIGetProp_t getProp = (DIGetProp_t)vt[5];
    for (int a = 0; a < 8; a++) {                      // X Y Z RX RY RZ slider0 slider1
        struct di_proprange r;
        memset(&r, 0, sizeof r);
        r.dwSize = sizeof r; r.dwHeaderSize = 16;
        r.dwObj = (DWORD)(a * 4); r.dwHow = DIPH_BYOFFSET;
        g_diDev[di].center[a] =
            (getProp(dev, (const void*)(uintptr_t)4, &r) >= 0) ? (r.lMin + r.lMax) / 2 : 0;
    }
    g_diDev[di].haveC = 1;
}

static HRESULT __stdcall di_hook_state(void* dev, DWORD cb, void* buf) {
    void** vt = *(void***)dev;
    DIGetState_t orig = NULL;
    for (int i = 0; i < g_diVtN; i++) if (g_diVt[i].vt == vt) { orig = g_diVt[i].state; break; }
    if (!orig) return (HRESULT)0x80004005L;            // unreachable: only patched vtables land here
    HRESULT hr = orig(dev, cb, buf);
    if (hr < 0 || !buf || !di_gated()) return hr;
    int di = di_dev_find(dev);
    if (di < 0) return hr;                             // a device this proxy never saw created
    if (!g_diSaid) {
        g_diSaid = 1;
        ff13logv("[FF13-SRP] input gate: reads are being silenced\n");
    }
    switch (g_diDev[di].kind) {
    case DI_KIND_KBD:
    case DI_KIND_MOUSE:
        memset(buf, 0, cb);                            // keys up; relative axes still
        break;
    case DI_KIND_PAD:
        // DIJOYSTATE/DIJOYSTATE2 both lay out 8 axis LONGs at 0, 4 POV DWORDs at 32 (centred is
        // 0xFFFFFFFF, NOT 0 -- 0 is "up"), buttons from 48. DIJOYSTATE2's extra axes past 176 are
        // velocity/acceleration/force, where zero is genuinely neutral.
        if (cb >= 0x50) {
            memset((uint8_t*)buf + 32, 0, cb - 32);
            for (int p = 0; p < 4; p++) *(DWORD*)((uint8_t*)buf + 32 + 4u * p) = 0xFFFFFFFFu;
            di_pad_centers(di, dev);
            for (int a = 0; a < 8; a++) *(LONG*)((uint8_t*)buf + 4u * a) = g_diDev[di].center[a];
        } else {
            memset(buf, 0, cb);                        // a format this small has no absolute axes
        }
        break;
    }
    return hr;
}

static HRESULT __stdcall di_hook_data(void* dev, DWORD cbObj, void* rgdod, DWORD* pn, DWORD flags) {
    void** vt = *(void***)dev;
    DIGetData_t orig = NULL;
    for (int i = 0; i < g_diVtN; i++) if (g_diVt[i].vt == vt) { orig = g_diVt[i].data; break; }
    if (!orig) return (HRESULT)0x80004005L;
    HRESULT hr = orig(dev, cbObj, rgdod, pn, flags);
    // The buffered path: the real read drains the queue, then reports nothing arrived. Dropping
    // rather than deferring is the point -- input from inside the gate must not replay after it.
    if (hr >= 0 && pn && di_gated() && di_dev_find(dev) >= 0) *pn = 0;
    return hr;
}

static DICreateDev_t g_diOrigCreate = NULL;
static void**        g_diPatchedDiVt = NULL;    // the ONE IDirectInput8 vtable that was patched

static HRESULT __stdcall di_hook_create(void* di8, const void* rguid, void** outDev, void* punk) {
    HRESULT hr = g_diOrigCreate(di8, rguid, outDev, punk);
    if (hr < 0 || !outDev || !*outDev || !rguid) return hr;
    void* dev = *outDev;
    int kind = DI_KIND_PAD;
    if      (memcmp(rguid, kGuidSysKeyboard, 16) == 0) kind = DI_KIND_KBD;
    else if (memcmp(rguid, kGuidSysMouse,    16) == 0) kind = DI_KIND_MOUSE;
    int di = di_dev_find(dev);
    if (di >= 0) { g_diDev[di].kind = kind; g_diDev[di].haveC = 0; }
    else if (g_diDevN < DI_DEVS) {
        g_diDev[g_diDevN].dev = dev; g_diDev[g_diDevN].kind = kind; g_diDev[g_diDevN].haveC = 0;
        g_diDevN++;
    }
    void** vt = *(void***)dev;
    int have = 0;
    for (int i = 0; i < g_diVtN; i++) if (g_diVt[i].vt == vt) { have = 1; break; }
    if (!have && g_diVtN < DI_VTS) {
        DWORD old;
        if (VirtualProtect(&vt[9], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            int n = g_diVtN;
            g_diVt[n].vt = vt;
            g_diVt[n].state = (DIGetState_t)vt[9];
            g_diVt[n].data  = (DIGetData_t)vt[10];
            g_diVtN = n + 1;                           // published first -- see the note above
            vt[9]  = (void*)di_hook_state;
            vt[10] = (void*)di_hook_data;
            VirtualProtect(&vt[9], 2 * sizeof(void*), old, &old);
        }
    }
    ff13logv("[FF13-SRP] input gate: device %p registered (%s)\n", dev,
             kind == DI_KIND_KBD ? "keyboard" : kind == DI_KIND_MOUSE ? "mouse" : "pad");
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver,
                                                        const void* riid, void** out, void* punk) {
    if (!g_real) {
        HMODULE m = LoadLibraryA("C:\\Windows\\System32\\dinput8.dll");
        if (m) g_real = (DI8C_t)GetProcAddress(m, "DirectInput8Create");
    }
    if (!g_real) return (HRESULT)0x80004005L; // E_FAIL
    HRESULT hr = g_real(hinst, ver, riid, out, punk);
    if (hr < 0 || !out || !*out) return hr;
    void** vt = *(void***)*out;
    // One vtable only: g_diOrigCreate is a single seat, and a second interface class (A vs W)
    // showing up with it already taken would be given a wrong original. The game asks for one.
    if (vt[3] != (void*)di_hook_create && (!g_diPatchedDiVt || g_diPatchedDiVt == vt)) {
        DWORD old;
        if (VirtualProtect(&vt[3], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            g_diOrigCreate = (DICreateDev_t)vt[3];
            g_diPatchedDiVt = vt;
            vt[3] = (void*)di_hook_create;
            VirtualProtect(&vt[3], sizeof(void*), old, &old);
        }
    }
    return hr;
}
