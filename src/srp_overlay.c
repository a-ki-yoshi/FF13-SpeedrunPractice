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

static char           g_flash[288];            // transient status line (recorded / cancelled / errors)
                                                 // Sized for UTF-8: a Japanese line runs to about
                                                 // three times the bytes of the English one, and a
                                                 // truncated one would be cut mid-character.
static volatile DWORD g_flashUntil = 0;        // g_flash visible until this GetTickCount
// ---- overlay typeface (the colors are in srp_internal.h) -------------------------------------
#define OVL_FONT_FACE  "Courier New"
#define OVL_FONT_FACE_JP "Meiryo UI"
// The Japanese face runs two points smaller: at the same requested height Meiryo's ideographs fill
// more of the cell than Courier's letters and read as the larger type on a mixed line.
#define OVL_FONT_H     20
#define OVL_FONT_H_JP  18
// Stands in only until the two faces have been asked for their metrics (see g_jpYoff).
#define OVL_JP_YOFF    ((OVL_FONT_H - OVL_FONT_H_JP) / 2)

static volatile DWORD  g_flashCol = OVL_FLASH;   // the colour THIS flash is drawn in
// Show a status line in the overlay for ~4s, in a colour of the caller's choosing.
void flash_say_c(const char* s, DWORD col) {
    strncpy(g_flash, s, sizeof g_flash - 1);
    g_flash[sizeof g_flash - 1] = 0;
    g_flashCol   = col;
    g_flashUntil = GetTickCount() + 4000;
}
void flash_say(const char* s) { flash_say_c(s, OVL_FLASH); }
// ============================================================================
// D3D9 overlay: draw the ON state of whatever is on, at the top-left corner.
// ----------------------------------------------------------------------------
// Vtable hook, no trampoline: EndScene and Reset are patched in the vtable of the game's own device
// (a throwaway device's vtable only as a fallback). D3D9/D3DX are resolved at runtime (LoadLibrary), so no link
// flags are added. Reads the feature globals only, never writes them.
//
// COM types are opaque here: interfaces are void* and their methods are reached through the vtable
// (`void** vt = *(void***)obj; vt[slot]`) with __stdcall signatures. Only the slot numbers matter:
//   IDirect3D9:        CreateDevice = 16
//   IDirect3DDevice9:  Reset = 16, EndScene = 42, Release = 2 (IUnknown)
//   ID3DXFont:         DrawTextA = 14, OnLostDevice = 16, OnResetDevice = 17
//   ID3DXSprite:       Begin = 8, End = 11, OnLostDevice = 12, OnResetDevice = 13

// Minimal, hand-rolled D3DPRESENT_PARAMETERS (all fields are 4 bytes on x86; the
// layout/order must match the real struct because CreateDevice reads it).
typedef struct {
    UINT BackBufferWidth, BackBufferHeight;
    UINT BackBufferFormat;            // D3DFORMAT (enum -> int)
    UINT BackBufferCount;
    UINT MultiSampleType;             // D3DMULTISAMPLE_TYPE
    DWORD MultiSampleQuality;
    UINT SwapEffect;                  // D3DSWAPEFFECT
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;      // D3DFORMAT
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
} FF13_D3DPP;

typedef void* (__stdcall *Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (__stdcall *CreateDevice_t)(void*, UINT Adapter, DWORD DevType, HWND hFocus,
                                            DWORD BehaviorFlags, FF13_D3DPP*, void** ppDevice);
typedef HRESULT (__stdcall *EndScene_t)(void*);
typedef HRESULT (__stdcall *Reset_t)(void*, FF13_D3DPP*);
typedef ULONG   (__stdcall *Release_t)(void*);
typedef HRESULT (__stdcall *FontOp_t)(void*);   // OnLostDevice / OnResetDevice
typedef HRESULT (__stdcall *PreloadChars_t)(void*, UINT first, UINT last);
typedef HDC     (__stdcall *FontGetDC_t)(void*);
typedef BOOL    (__stdcall *GetTextMetricsA_t)(void* font, TEXTMETRICA* tm);
typedef INT     (__stdcall *DrawTextA_t)(void* font, void* sprite, LPCSTR str, INT count,
                                         RECT* rect, DWORD fmt, DWORD color);   // D3DXFONT::DrawTextA
typedef INT     (__stdcall *DrawTextW_t)(void* font, void* sprite, const unsigned short* str,
                                         INT count, RECT* rect, DWORD fmt, DWORD color);
typedef HRESULT (WINAPI *D3DXCreateFontA_t)(void* dev, INT Height, UINT Width, UINT Weight,
                                            UINT MipLevels, BOOL Italic, DWORD CharSet,
                                            DWORD OutputPrecision, DWORD Quality,
                                            DWORD PitchAndFamily, LPCSTR pFaceName, void** ppFont);
typedef HRESULT (WINAPI *D3DXCreateSprite_t)(void* dev, void** ppSprite);
typedef HRESULT (__stdcall *SpriteBegin_t)(void*, DWORD flags);

typedef HRESULT (__stdcall *GetRenderTarget_t)(void*, DWORD idx, void** ppSurface);
typedef HRESULT (__stdcall *Clear_t)(void*, DWORD count, const void* rects, DWORD flags,
                                     DWORD color, float z, DWORD stencil);
typedef HRESULT (__stdcall *GetBackBuffer_t)(void*, UINT swap, UINT bb, DWORD type, void** ppSurface);
typedef HRESULT (__stdcall *CreateStateBlock_t)(void*, DWORD type, void** ppSB);
typedef HRESULT (__stdcall *SetRenderState_t)(void*, DWORD state, DWORD value);
typedef HRESULT (__stdcall *SetTexture_t)(void*, DWORD stage, void* tex);
typedef HRESULT (__stdcall *SetTexStage_t)(void*, DWORD stage, DWORD type, DWORD value);
typedef HRESULT (__stdcall *SetFVF_t)(void*, DWORD fvf);
typedef HRESULT (__stdcall *SetShader_t)(void*, void* shader);
typedef HRESULT (__stdcall *DrawPrimUP_t)(void*, DWORD type, UINT primCount, const void* data,
                                          UINT stride);
typedef HRESULT (__stdcall *SBApply_t)(void*);

#define D3DDEV_VT_RESET           16
#define D3DDEV_VT_GETBACKBUFFER   18
#define D3DDEV_VT_GETRENDERTARGET 38
#define D3DDEV_VT_ENDSCENE        42
#define D3DDEV_VT_CLEAR           43
#define D3DDEV_VT_SETRENDERSTATE  57
#define D3DDEV_VT_CREATESTATEBLOCK 59
#define D3DDEV_VT_SETTEXTURE      65
#define D3DDEV_VT_SETTEXSTAGE     67
#define D3DDEV_VT_DRAWPRIMUP      83
#define D3DDEV_VT_SETFVF          89
#define D3DDEV_VT_SETVSHADER      92
#define D3DDEV_VT_SETPSHADER      107
#define SB_VT_CAPTURE             4
#define SB_VT_APPLY               5
#define IUNK_VT_RELEASE     2
#define FONT_VT_GETTEXTMETRICSA 6
#define FONT_VT_GETDC       8
#define FONT_VT_PRELOADCHARS 10
#define FONT_VT_DRAWTEXTA  14
#define FONT_VT_DRAWTEXTW  15
#define FONT_VT_ONLOST     16
#define FONT_VT_ONRESET    17
#define SPRITE_VT_BEGIN     8
#define SPRITE_VT_END      11
#define SPRITE_VT_ONLOST   12
#define SPRITE_VT_ONRESET  13
#define D3DXSPRITE_ALPHABLEND_F 0x10
#define DT_NOCLIP_FLAG   0x100


static void* g_origEndScene = NULL;   // saved IDirect3DDevice9::EndScene
static void* g_origReset    = NULL;   // saved IDirect3DDevice9::Reset
// The back buffer's identity. The pointer is only ever COMPARED, never dereferenced: the device
// owns the surface, and it stays the same surface until a Reset recreates it -- which is why
// hkReset drops the cache.
static void* g_bbCache    = NULL;
static void* g_bbCacheDev = NULL;     // the device g_bbCache belongs to (refetched if it changes)
static int   g_bbWidth    = 0;
static void* g_font         = NULL;   // ID3DXFont* (lazily created on the real device)
static void* g_sprite       = NULL;   // ID3DXSprite* every DrawText draws through
static void* g_spriteActive = NULL;   // = g_sprite only between Begin/End; what the helpers pass
// How far a Japanese run drops so both faces sit on ONE baseline. DrawText aligns the cell top to
// rect.top, putting the baseline at top + tmAscent, so the drop is the two faces' ascent difference
// -- asked of the fonts themselves, because it belongs to whichever face the machine mapped. Set
// once at font creation; the fixed nudge stands in until then.
static int g_jpYoff = OVL_JP_YOFF;
static volatile LONG g_fontTried    = 0;   // font creation settled: made, or given up after the retries
static int   g_fontAttempts = 0;
static DWORD g_fontRetryAt  = 0;
// One failed creation must not leave the overlay blank for the session (a pass that lands on a
// lost or resetting device fails it); a few spaced retries cover that without a busy loop.
#define OVL_FONT_TRIES     10
#define OVL_FONT_RETRY_MS  2000
static volatile LONG g_overlayReady = 0;   // EndScene hook installed
volatile int g_ovlTopPx = OVL_TOP_DEF;

void vk_label(UINT b, char* out, size_t n) { ff13_vk_label(b, out, n); }

void tidy_ok_phrase(char* out, size_t n) {
    char k[32];
    vk_label(g_menuOkVk, k, sizeof k);
    snprintf(out, n, ui_text(T_OK_KEY), k);
}


// The black behind every line: 0 = a single shadow (2 draws per line), 1 = the four diagonals (5),
// 2 = all eight neighbours (9).
// Shipped at 0. The frame-rate cost measured for 2 (2026-08-12) was the per-call state swap, which
// the shared sprite removed -- 1 and 2 only add quads now.
#define OVL_OUTLINE 0
// How far the black sits from the text, in pixels -- a separate knob from the shape, because a wide
// shadow reads as depth while a wide outline looks like a second, blurry copy of the glyph.
#define OVL_OFFSET_PX 2
// Corners first, then edges, so the 4-offset setting is simply the first four -- the diagonals are
// the ones worth having if only four are drawn, each covering two sides of a glyph at once.
static const int kOvlOutline[8][2] = { {-1,-1}, {1,-1}, {-1,1}, {1,1},
                                       { 0,-1}, {0, 1}, {-1,0}, {1,0} };
#define OVL_OUTLINE_N (OVL_OUTLINE == 2 ? 8 : 4)

// A blended quad behind overlay text. The full device state is captured and put back through a
// state block (D3DSBT_ALL = 1): D3DXFont's own draws follow on this same pass, and enumerating
// the dozen states a quad touches is how one gets missed. No state block = no draw.
// hkReset MUST release this -- an outstanding state block makes the game's own Reset fail.
static void* g_backdropSB = NULL;
static void overlay_backdrop(void* dev, float x0, float y0, float x1, float y1, DWORD argb) {
    void** vt = *(void***)dev;
    if (!g_backdropSB)
        ((CreateStateBlock_t)vt[D3DDEV_VT_CREATESTATEBLOCK])(dev, 1, &g_backdropSB);
    void* sb = g_backdropSB;
    if (!sb) return;
    if (((SBApply_t)(*(void***)sb)[SB_VT_CAPTURE])(sb) < 0) return;
    ((SetShader_t)vt[D3DDEV_VT_SETVSHADER])(dev, NULL);
    ((SetShader_t)vt[D3DDEV_VT_SETPSHADER])(dev, NULL);
    ((SetTexture_t)vt[D3DDEV_VT_SETTEXTURE])(dev, 0, NULL);
    SetTexStage_t ts = (SetTexStage_t)vt[D3DDEV_VT_SETTEXSTAGE];
    ts(dev, 0, 1 /*COLOROP*/, 2 /*SELECTARG1*/); ts(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
    ts(dev, 0, 4 /*ALPHAOP*/, 2);                ts(dev, 0, 5 /*ALPHAARG1*/, 0);
    ts(dev, 1, 1, 1 /*DISABLE*/);                ts(dev, 1, 4, 1);
    SetRenderState_t rs = (SetRenderState_t)vt[D3DDEV_VT_SETRENDERSTATE];
    rs(dev,  27, 1);     // ALPHABLENDENABLE
    rs(dev,  19, 5);     // SRCBLEND  = SRCALPHA
    rs(dev,  20, 6);     // DESTBLEND = INVSRCALPHA
    rs(dev,   7, 0);     // ZENABLE off
    rs(dev,  14, 0);     // ZWRITEENABLE off
    rs(dev,  15, 0);     // ALPHATESTENABLE off
    rs(dev,  22, 1);     // CULLMODE = NONE
    rs(dev,  28, 0);     // FOGENABLE off
    rs(dev,  52, 0);     // STENCILENABLE off
    rs(dev, 168, 0xF);   // COLORWRITEENABLE = all
    rs(dev, 174, 0);     // SCISSORTESTENABLE off
    struct { float x, y, z, rhw; DWORD c; } v[4] = {
        { x0, y0, 0.f, 1.f, argb }, { x1, y0, 0.f, 1.f, argb },
        { x0, y1, 0.f, 1.f, argb }, { x1, y1, 0.f, 1.f, argb },
    };
    ((SetFVF_t)vt[D3DDEV_VT_SETFVF])(dev, 0x44 /*XYZRHW|DIFFUSE*/);
    ((DrawPrimUP_t)vt[D3DDEV_VT_DRAWPRIMUP])(dev, 5 /*TRIANGLESTRIP*/, 2, v, sizeof v[0]);
    ((SBApply_t)(*(void***)sb)[SB_VT_APPLY])(sb);
}

// Draw one line, outlined in black for readability. All calls guarded.
static void overlay_line(void* font, int x, int y, const char* text, DWORD color) {
    if (!font || !text) return;
    void** vt = *(void***)font;
    if (!vt) return;
    DrawTextA_t p = (DrawTextA_t)vt[FONT_VT_DRAWTEXTA];
    if (!p) return;
    RECT r;
#if OVL_OUTLINE == 0
    r.left = x + OVL_OFFSET_PX; r.top = y + OVL_OFFSET_PX;
    r.right = r.left + 1200; r.bottom = r.top + 60;
    p(font, g_spriteActive, text, -1, &r, DT_NOCLIP_FLAG, 0xFF000000);   // shadow
#else
    for (int i = 0; i < OVL_OUTLINE_N; i++) {
        r.left = x + kOvlOutline[i][0] * OVL_OFFSET_PX;
        r.top  = y + kOvlOutline[i][1] * OVL_OFFSET_PX;
        r.right = r.left + 1200; r.bottom = r.top + 60;
        p(font, g_spriteActive, text, -1, &r, DT_NOCLIP_FLAG, 0xFF000000);
    }
#endif
    r.left = x; r.top = y; r.right = x + 1200; r.bottom = y + 60;
    p(font, g_spriteActive, text, -1, &r, DT_NOCLIP_FLAG, color);        // body, last so it sits on top
}

// Is a typeface actually installed? Needed because the GDI font mapper substitutes silently instead
// of failing, which makes a missing font look exactly like a working one.
static int CALLBACK font_seen_cb(const LOGFONTA* lf, const TEXTMETRICA* tm, DWORD type, LPARAM p) {
    (void)lf; (void)tm; (void)type;
    *(int*)p = 1;
    return 0;                                    // one hit is enough; stop enumerating
}
static int font_installed(const char* face) {
    HDC dc = GetDC(NULL);
    if (!dc) return 0;
    LOGFONTA lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    int found = 0;
    EnumFontFamiliesExA(dc, &lf, font_seen_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

// The ASCII face is monospace, so one measurement gives every column position. Measured through the
// font's own DT_CALCRECT rather than assumed, because the advance depends on the height the font
// was created at. Cached; reset with the font on a device reset.
#define DT_CALCRECT_FLAG 0x400
static int g_ovlCw = 0;
static int overlay_char_w(void* font) {
    if (g_ovlCw) return g_ovlCw;
    if (!font) return 0;
    void** vt = *(void***)font;
    if (!vt) return 0;
    DrawTextA_t p = (DrawTextA_t)vt[FONT_VT_DRAWTEXTA];
    if (!p) return 0;
    RECT r; r.left = 0; r.top = 0; r.right = 0; r.bottom = 0;
    p(font, NULL, "MMMMMMMMMMMMMMMMMMMM", 20, &r, DT_CALCRECT_FLAG, 0);
    int w = (int)(r.right - r.left);
    g_ovlCw = (w > 0) ? (w + 19) / 20 : 12;      // 12 = a sane guess if the measure fails
    return g_ovlCw;
}

static int font_ascent(void* font) {
    if (!font) return 0;
    void** vt = *(void***)font;
    if (!vt || !vt[FONT_VT_GETTEXTMETRICSA]) return 0;
    TEXTMETRICA tm;
    memset(&tm, 0, sizeof tm);
    if (!((GetTextMetricsA_t)vt[FONT_VT_GETTEXTMETRICSA])(font, &tm)) return 0;
    return (int)tm.tmAscent;
}

// The Japanese text path: UTF-16 through DrawTextW -- the ANSI path would render only on a machine
// whose codepage happens to be 932. A mixed line is split into runs; runs under 0x80 use the ASCII
// face so they stay monospaced, which the column maths below assumes of them. An ASCII run's
// advance is chars * overlay_char_w, which also sidesteps DT_CALCRECT's habit of dropping trailing
// spaces; a Japanese run is measured with DT_CALCRECT. All shadows go down before any body, so a
// run's outline never clips the glyph drawn before it.
#define OVL_RUNS_MAX 24
#define JPW_CACHE_N 128
static struct { unsigned h; int len; int w; } g_jpw[JPW_CACHE_N];
static int jp_run_width(void* font, const unsigned short* s, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ s[i]) * 16777619u;
    unsigned slot = h % JPW_CACHE_N;
    if (g_jpw[slot].h == h && g_jpw[slot].len == len && g_jpw[slot].w > 0) return g_jpw[slot].w;
    void** vt = *(void***)font;
    DrawTextW_t p = vt ? (DrawTextW_t)vt[FONT_VT_DRAWTEXTW] : NULL;
    if (!p) return 0;
    RECT m; m.left = 0; m.top = 0; m.right = 0; m.bottom = 0;
    p(font, NULL, s, len, &m, DT_CALCRECT_FLAG, 0);
    int w = (int)(m.right - m.left);
    if (w > 0) { g_jpw[slot].h = h; g_jpw[slot].len = len; g_jpw[slot].w = w; }
    return w;
}
// The English arrows (U+25C4/U+25BA) are Courier New's own glyphs, drawn in the ASCII face: a
// non-Japanese Windows often lacks Meiryo UI, and its stand-in may lack the arrows.
static int latin_face(unsigned short c) { return c < 0x80 || c == 0x25C4 || c == 0x25BA; }
// Must split and measure exactly as overlay_line_w draws, or a caret placed at x + this drifts.
static int overlay_width_w(const unsigned short* text) {
    if (!text || !text[0]) return 0;
    int cw = g_font ? overlay_char_w(g_font) : 0;
    int w = 0, i = 0;
    while (text[i]) {
        int start = i, ascii = latin_face(text[i]);
        while (text[i] && latin_face(text[i]) == ascii) i++;
        if (ascii && g_font && cw) { w += cw * (i - start); continue; }
        if (!g_fontJp) continue;
        w += jp_run_width(g_fontJp, text + start, i - start);
    }
    return w;
}
static int overlay_text_w(const char* utf8) {
    if (!utf8 || !utf8[0]) return 0;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p && *p < 0x80) p++;
    if (!*p) return (g_font ? overlay_char_w(g_font) : 0) * (int)strlen(utf8);
    unsigned short w[320];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t*)w, (int)(sizeof w / sizeof w[0])) <= 0)
        return 0;
    return overlay_width_w(w);
}
static void overlay_line_w(int x, int y, const unsigned short* text, DWORD color) {
    if (!text || !text[0]) return;
    struct { void* font; const unsigned short* s; int len; int x; } runs[OVL_RUNS_MAX];
    int nr = 0, cx = x, i = 0;
    int cw = g_font ? overlay_char_w(g_font) : 0;
    while (text[i] && nr < OVL_RUNS_MAX) {
        int start = i, ascii = latin_face(text[i]);
        if (nr == OVL_RUNS_MAX - 1) {          // out of slots: the last run takes all that is left,
            while (text[i]) i++;               // in the Japanese face, which renders either kind
            ascii = 0;
        } else {
            while (text[i] && latin_face(text[i]) == ascii) i++;
        }
        void* f = (ascii && g_font && cw) ? g_font : g_fontJp;
        if (!f) continue;                      // no Japanese face: that run is left out
        runs[nr].font = f; runs[nr].s = text + start; runs[nr].len = i - start; runs[nr].x = cx;
        if (f == g_font) {
            cx += cw * runs[nr].len;
        } else if (text[i]) {                  // the last run's width has no taker: skip the measure
            cx += jp_run_width(f, runs[nr].s, runs[nr].len);
        }
        nr++;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nr; k++) {
            void** vt = *(void***)runs[k].font;
            if (!vt) return;
            DrawTextW_t p = (DrawTextW_t)vt[FONT_VT_DRAWTEXTW];
            if (!p) return;
            int yy = (runs[k].font == g_fontJp) ? y + g_jpYoff : y;   // onto the ASCII baseline
            RECT r;
            if (pass == 0) {
#if OVL_OUTLINE == 0
                r.left = runs[k].x + OVL_OFFSET_PX; r.top = yy + OVL_OFFSET_PX;
                r.right = r.left + 1200; r.bottom = r.top + 60;
                p(runs[k].font, g_spriteActive, runs[k].s, runs[k].len, &r, DT_NOCLIP_FLAG, 0xFF000000);
#else
                for (int o = 0; o < OVL_OUTLINE_N; o++) {
                    r.left = runs[k].x + kOvlOutline[o][0] * OVL_OFFSET_PX;
                    r.top  = yy + kOvlOutline[o][1] * OVL_OFFSET_PX;
                    r.right = r.left + 1200; r.bottom = r.top + 60;
                    p(runs[k].font, g_spriteActive, runs[k].s, runs[k].len, &r, DT_NOCLIP_FLAG, 0xFF000000);
                }
#endif
            } else {
                r.left = runs[k].x; r.top = yy; r.right = r.left + 1200; r.bottom = yy + 60;
                p(runs[k].font, g_spriteActive, runs[k].s, runs[k].len, &r, DT_NOCLIP_FLAG, color);
            }
        }
    }
}

// One line of the overlay's own text, whatever language it turned out to be in: callers format
// into a plain char buffer and this picks the path, so only a line with a byte over 0x7F pays for
// the conversion. A failed conversion falls back to the ASCII path -- mojibake looks like exactly
// what it is, where a blank line would look like the feature having stopped working.
static void overlay_say(void* font, int x, int y, const char* utf8, DWORD color);
// A "\n" in the text is a PREFERRED break, not a hard one: it is used only when the line will not
// fit, and otherwise it DISAPPEARS -- put any space the joined text needs BEFORE it, never after.
#define OVL_WRAP_LINES 3
// One character's advance as overlay_line_w lays it: the ASCII face's cell, or the run face's own
// measure (cached per character by jp_run_width).
static int char_px(const unsigned char* q, int len, int cw) {
    if (len == 1) return cw;
    unsigned short w[2];
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)q, len, (wchar_t*)w, 2);
    if (n <= 0) return 2 * cw;
    if (n == 1 && latin_face(w[0])) return cw;
    int px = g_fontJp ? jp_run_width(g_fontJp, w, n) : 0;
    return px > 0 ? px : 2 * cw;
}
static int is_jp_stop(const unsigned char* q, int len) {      // U+3002
    return len == 3 && q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x82;
}
static int is_jp_comma(const unsigned char* q, int len) {     // U+3001
    return len == 3 && q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x81;
}
// draw = 0 only counts the rows, so a backdrop can be sized before the text goes over it.
// endX, when given, gets the pixel x where the last row's text ends.
// Measured in pixels, not cells: a Japanese character is narrower than two ASCII cells, and
// counting it as two broke lines well short of the screen's edge. Where a line has to break and the
// text names no place for it, the last full stop, then the last comma, then the last space is taken.
static int overlay_wrap_ex(void* font, int x, int contX, int y, const char* utf8, DWORD color,
                           int lineH, int draw, int* endX) {
    int cw = overlay_char_w(font);
    int budget = (g_bbWidth > 0 && cw > 0) ? (g_bbWidth - x - 14) / cw : 0;
    if (endX) *endX = x;
    if (budget < 16) {
        if (draw) overlay_say(font, x, y, utf8, color);
        if (endX) *endX = x + overlay_text_w(utf8);
        return 1;
    }
    const unsigned char* p = (const unsigned char*)utf8;
    char line[512];
    int used = 0;
    while (*p && used < OVL_WRAP_LINES) {
        int lx = used ? contX : x;
        int limPx = g_bbWidth - lx - 14;
        int px = 0, n = 0;
        int wantN = -1, spaceN = -1, stopN = -1, commaN = -1;
        const unsigned char* wantQ = NULL, *spaceQ = NULL, *stopQ = NULL, *commaQ = NULL;
        const unsigned char* q = p;
        while (*q && n < (int)sizeof line - 4) {
            if (*q == '\n') { wantN = n; wantQ = q + 1; q++; continue; }
            int len = (*q < 0x80) ? 1 : (*q < 0xE0) ? 2 : (*q < 0xF0) ? 3 : 4;
            int w = char_px(q, len, cw);
            if (px + w > limPx) break;
            if (*q == ' ') { spaceN = n; spaceQ = q + 1; }
            int stop = is_jp_stop(q, len), comma = is_jp_comma(q, len);
            for (int i = 0; i < len && *q; i++) line[n++] = (char)*q++;
            if (stop)  { stopN = n;  stopQ = q; }
            if (comma) { commaN = n; commaQ = q; }
            px += w;
        }
        if (*q) {
            if (wantN >= 0)       { n = wantN;  q = wantQ; }
            else if (stopN > 0)   { n = stopN;  q = stopQ; }
            else if (commaN > 0)  { n = commaN; q = commaQ; }
            else if (spaceN >= 0 && *q != ' ' && (unsigned char)*q < 0x80) { n = spaceN; q = spaceQ; }
        }
        line[n] = 0;
        if (draw) overlay_say(font, lx, y + used * lineH, line, color);
        if (endX) *endX = lx + overlay_text_w(line);
        used++;
        while (*q == ' ' || *q == '\n') q++;
        p = q;
    }
    return used ? used : 1;
}
static int overlay_say_wrap(void* font, int x, int contX, int y, const char* utf8, DWORD color,
                            int lineH) {
    return overlay_wrap_ex(font, x, contX, y, utf8, color, lineH, 1, NULL);
}

static void overlay_say(void* font, int x, int y, const char* utf8, DWORD color) {
    if (!utf8) return;
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p && *p < 0x80) p++;
    if (!*p) { overlay_line(font, x, y, utf8, color); return; }
    unsigned short w[320];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t*)w,
                                (int)(sizeof w / sizeof w[0]));
    if (n <= 0) { overlay_line(font, x, y, utf8, color); return; }
    overlay_line_w(x, y, w, color);
}

// The exported face of the helpers above, for a block drawn from another file inside overlay_draw.
static void* g_passDev = NULL;             // the device overlay_draw is drawing with this pass
int  ovl_char_w(void) { return overlay_char_w(g_font); }
int  ovl_text_w(const char* utf8) { return overlay_text_w(utf8); }
// Spaces of several widths, measured and laid widest-first, so a gap narrower than an ASCII cell can
// still be closed (a kana in the proportional face is a few px). A space the face lacks measures 0
// and is skipped; under ui_ascii_only only the ASCII one is used.
int ovl_space(char* out, size_t n, int px) {
    static const char* const kFill[] = { "\xE3\x80\x80", " ", "\xE2\x80\x82", "\xE2\x80\x89",
                                         "\xE2\x80\x8A" };
    enum { FILLS = sizeof kFill / sizeof kFill[0] };
    size_t pl = 0;
    if (n) out[0] = 0;
    int ascii = ui_ascii_only();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < FILLS; i++) {
            if (ascii && i != 1) continue;
            int fw = overlay_text_w(kFill[i]);
            if (fw <= 0) continue;
            size_t fl = strlen(kFill[i]);
            while (px >= (pass ? (fw + 1) / 2 : fw) && pl + fl < n) {
                memcpy(out + pl, kFill[i], fl);
                pl += fl;
                px -= fw;
                if (pass) break;
            }
        }
    if (n) out[pl < n ? pl : n - 1] = 0;
    return px;
}
void ovl_fit(char* v, size_t n, const char* const* cands, int cn) {
    int cw = overlay_char_w(g_font);
    if (!cw || !v) return;
    int mx = 0;
    for (int i = 0; i < cn; i++) {
        int w = cands[i] ? overlay_text_w(cands[i]) : 0;
        if (w > mx) mx = w;
    }
    size_t len = strlen(v);
    int w = overlay_text_w(v);
    while (mx - w >= (cw + 1) / 2 && len + 1 < n) { v[len++] = ' '; v[len] = 0; w += cw; }
}
// The category list padded to its widest name (and <new category>, where that is a stop).
static void fit_cat(char* v, size_t n, const char* extra) {
    const char* cands[CAT_MAX + 1];
    int cn = 0, cc = cat_count();
    for (int i = 0; i < cc && i < CAT_MAX; i++) if (cat_name_at(i)) cands[cn++] = cat_name_at(i);
    if (extra) cands[cn++] = extra;
    ovl_fit(v, n, cands, cn);
}
void ovl_say(int x, int y, const char* utf8, DWORD argb) { overlay_say(g_font, x, y, utf8, argb); }
int  ovl_say_wrap(int x, int contX, int y, const char* utf8, DWORD argb, int lineH) {
    return overlay_wrap_ex(g_font, x, contX, y, utf8, argb, lineH, 1, NULL);
}
int  ovl_wrap_count(int x, int contX, const char* utf8) {
    return overlay_wrap_ex(g_font, x, contX, 0, utf8, 0, OVL_LINE_H, 0, NULL);
}
int  ovl_wrap_tail(int x, int contX, const char* utf8, const char* tail, int* tailX, int* tailRow) {
    int endX = x;
    int rows = overlay_wrap_ex(g_font, x, contX, 0, utf8, 0, OVL_LINE_H, 0, &endX);
    int tw = overlay_text_w(tail);
    if (g_bbWidth <= 0 || endX + tw <= g_bbWidth - 14) {
        *tailX = endX; *tailRow = rows - 1;
        return rows;
    }
    *tailX = contX; *tailRow = rows;
    return rows + 1;
}
void ovl_backdrop(int x0, int y0, int x1, int y1, DWORD argb) {
    if (g_passDev) overlay_backdrop(g_passDev, (float)x0, (float)y0, (float)x1, (float)y1, argb);
}
static void overlay_band(void* dev, int y, int rows) {
    overlay_backdrop(dev, 0.f, (float)(y - 2), 4096.f, (float)(y + rows * OVL_LINE_H + 2),
                     OVL_MENU_BAND);
}
void ovl_menu_band(int y, int rows) { if (g_passDev) overlay_band(g_passDev, y, rows); }

// The battle overlay's own row: a label on the left, a figure right-aligned against a fixed edge so
// the digits stay under each other as they change. The figure is ASCII in either language, so the
// ASCII font's character width places it exactly; the label may be translated and is drawn from the
// left. 18 must clear indent + longest label + figure together, since the two are drawn from
// opposite ends: "ATB 12s" (7) against "READY " (6) leaves three characters. A full-width glyph is
// about twice an ASCII one, so keep these labels short.
#define OVL_BATTLE_COL 18           // where the figure ends, in characters from x=14
#define OVL_BATTLE_INDENT 2         // ...and where a row's label starts, under its heading
static void overlay_battle_head(void* font, int y, const char* label) {
    overlay_say(font, 14, y, label, OVL_BATTLE_HEAD);
}

static void overlay_battle_row(void* font, int y, const char* label, const char* value) {
    int cw = overlay_char_w(font);
    overlay_say(font, 14 + OVL_BATTLE_INDENT * cw, y, label, OVL_BATTLE);
    int n  = (int)strlen(value);
    int x  = 14 + (OVL_BATTLE_COL - n) * cw;
    overlay_line(font, x, y, value, OVL_BATTLE);
}

// The game's name for a scene, or NULL when the names header was not generated.
static const unsigned short* btsc_name(uint32_t btsc) {
#ifdef FF13_HAVE_BTSC_NAMES
    int lo = 0, hi = FF13_BTSC_NAMES_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned v = kBtscNames[mid].btsc;
        if (v == btsc) return &kBtscNamePool[kBtscNames[mid].off];
        if (v < btsc) lo = mid + 1; else hi = mid - 1;
    }
#else
    (void)btsc;
#endif
    return NULL;
}

// True while a battle is active (BattleMain state [BM+0x44] != 0). Guarded.
int overlay_in_battle(void) {
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_STATE + 4)) return 0;
    return *(int*)(bm + OFF_BM_STATE) != 0;
}

// ---- battle timer (per-fight GAME + REAL time, asl-style) ----
// GAME = the in-game battle timer at [BM+0x60] (ms). REAL = elapsed time measured with our OWN
// QueryPerformanceCounter, so game-speed (which warps the game clock) does not distort it.
#define OFF_BM_GAMETIME 0x60     // [BM+0x60] = in-game battle timer (ms)
// Fight-boundary signals (same source; module-relative RVAs, deref then offset).
#define RVA_TARGET_PTR  RVA_BTRESDB_PTR // db_bt_result: +0x3C = sheet 0's target, nonzero once a fight
                                        // resolves. Every pick zeroes it (btres_clear_for_pick).
#define OFF_TARGET      0x3C
#define RVA_ENEMYPT_PTR 0x23FD20C // [g_base+..] -> ptr; +0x12C = enemy_point (clears when fight ends)
#define OFF_ENEMYPT     0x12C
static LARGE_INTEGER g_qpcFreq;                 // set once in overlay_init
static LARGE_INTEGER g_realStart, g_realEnd;    // QPC ticks at fight start / end
static int g_realRunning = 0;                   // REAL timer running
// Fight boundaries: REAL starts when GAME (battletime) goes 0->nonzero and ends when the fight
// resolves (target appears, or enemy_point clears while in a fight) -- same boundaries as GAME.
static int g_inFight = 0, g_prevBt = 0, g_prevTarget = 0, g_prevEnemyPt = 0;
static int g_prevInBattle = 0;                  // ...and whether the battle screen was up

// -1 if unavailable. Guarded read-only.
static int battle_gametime_ms(void) {
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_GAMETIME + 4)) return -1;
    return *(int*)(bm + OFF_BM_GAMETIME);
}

// Read an int through [g_base+ptrRva] then +off; 0 if unreadable. Read-only.
int read_via_ptr(unsigned ptrRva, int off) {
    char* p = *(char**)(g_base + ptrRva);
    if (!p || IsBadReadPtr(p, off + 4)) return 0;
    return *(int*)(p + off);
}

// ---- paradigm shift -> party-wide ATB refill ----
#define OFF_BM_BTLCLOCK  0x4C       // battle clock (ms). NOT +0x60: that one pauses on another flag
#define OFF_BM_OPTSTAMP  0xE0       // the battle clock reading when the last shift refilled ATB
#define OFF_BM_FLAGS     0x38       // battle flags, a dword bitset
#define BFLAG_EASY       0x49       // set = the interval is scaled by fEasyOptCh
#define RVA_OPT_ATB_TIME 0x23FD47C  // iOptAtbTime; 0 until the battle database has loaded
#define RVA_EASY_OPT_CH  0x23FD4C0  // fEasyOptCh

// Milliseconds until a paradigm shift refills the party's ATB again; <= 0 = the next one does.
static int atb_shift_left_ms(int* out, int* period) {
    char* bm = *(char**)(g_base + RVA_BM_PTR);
    if (!bm || IsBadReadPtr(bm, OFF_BM_OPTSTAMP + 4)) return 0;
    int iv = *(int*)(g_base + RVA_OPT_ATB_TIME);
    if (iv <= 0) return 0;                       // constants not loaded yet, not "always ready"
    const uint32_t* fl = (const uint32_t*)(bm + OFF_BM_FLAGS);
    if (fl[BFLAG_EASY >> 5] & (1u << (BFLAG_EASY & 31)))
        iv = (int)((float)iv * *(float*)(g_base + RVA_EASY_OPT_CH));
    *out = iv - (*(int*)(bm + OFF_BM_BTLCLOCK) - *(int*)(bm + OFF_BM_OPTSTAMP));
    if (period) *period = iv;                    // easy mode scales it, so never hardcode the 12
    return 1;
}

// ---- the target's chain gauge -> how long it still holds ----
#define OFF_CHARA_HP     0x290

// The probe span deliberately reaches the chain object -- far past any field read here, because
// callers read that one off the returned pointer. Two enemies on equal HP resolve to the lower
// actor id.
static uintptr_t target_actor(int hp) {
    uint32_t* tbl = actor_table();
    if (!tbl) return 0;
    for (int i = 0; i < ACTOR_SLOTS; i++) {
        uintptr_t a = tbl[i];
        if (!a || IsBadReadPtr((void*)a, OFF_CHARA_CHAIN + OFF_CHAIN_LEFT + 4)) continue;
        // The table holds the party too, and a party member on the target's exact HP would
        // answer for it: only the enemy side may match. NOT the +0x24E bit (see OFF_TEAM).
        if (IsBadReadPtr((void*)(a + OFF_GUARD324), 4) || !*(int*)(a + OFF_GUARD324)) continue;
        if (*(int*)(a + OFF_CHARA_HP) != hp) continue;
        return a;
    }
    return 0;
}

// Milliseconds as m:ss.cc (minutes : seconds . centiseconds).
static void fmt_mmsscc(int ms, char* out, size_t n) {
    if (ms < 0) ms = 0;
    int m = ms / 60000, rem = ms % 60000, s = rem / 1000, cc = (rem % 1000) / 10;
    snprintf(out, n, "%d:%02d.%02d", m, s, cc);
}

#ifdef FF13_DIAG
// Per-5s draw-cost census. drawn/s above the frame rate = several back-buffer EndScene passes in
// one frame, each repainting the overlay.
static DWORD    g_diagCalls = 0, g_diagDraws = 0;
static LONGLONG g_diagTicks = 0;
static char g_passTrace[49]; static int g_passN = 0;
static void overlay_diag(LONGLONG dt) {
    static DWORD win = 0;
    g_diagCalls++; g_diagTicks += dt;
    DWORD now = GetTickCount();
    if (!win) { win = now; return; }
    if (now - win < 5000) return;
    // Also logged with zero draws: EndScene arriving but no pass ever matching the back buffer is
    // the one overlay failure that leaves no other trace.
    if (g_diagCalls && g_qpcFreq.QuadPart > 0) {
        double secs = (now - win) / 1000.0;
        double us   = (double)g_diagTicks * 1e6 / (double)g_qpcFreq.QuadPart;
        g_passTrace[g_passN] = 0;
        ff13loga("[FF13-SRP] overlay diag: %.0f EndScene/s, %.0f drawn/s, cpu %.0fus/s "
                 "(%.0fus per drawn pass) passes \"%s\"\n", g_diagCalls / secs,
                 g_diagDraws / secs, us / secs, g_diagDraws ? us / g_diagDraws : 0.0, g_passTrace);
    }
    win = now; g_diagCalls = 0; g_diagDraws = 0; g_diagTicks = 0; g_passN = 0;
}
#endif

// Nonzero once the fight has RESOLVED -- the same signal the battle timer stops REAL on, which is
// well before the battle state machine goes idle. Anything that has to come back out of a fight
// before the game writes its result down has to do it here rather than at the end.
int battle_resolved(void) { return read_via_ptr(RVA_TARGET_PTR, OFF_TARGET) != 0; }

// Runs on the game (render) thread from hkEndScene. Creates the font lazily against the REAL
// device.
static void overlay_draw(void* dev) {
    if (!dev) return;
    unsigned char a = g_armed, s = g_suppressEnabled, w = g_gsEnabled;
    int inBattle = overlay_in_battle();
    // Edges are per-frame stable, so the extra EndScene passes are idempotent.
    int bt = battle_gametime_ms(); if (bt < 0) bt = 0;
    int tgt = read_via_ptr(RVA_TARGET_PTR, OFF_TARGET);
    int ep  = read_via_ptr(RVA_ENEMYPT_PTR, OFF_ENEMYPT);
    // The battle screen comes up a moment before its clock starts, so REAL is zeroed on the frame
    // the screen appears -- but never over a clock already running.
    if (inBattle && !g_prevInBattle && !g_realRunning) {
        QueryPerformanceCounter(&g_realStart);
        g_realEnd = g_realStart;
    }
    g_prevInBattle = inBattle;
    if (bt != 0 && g_prevBt == 0) { QueryPerformanceCounter(&g_realStart); g_realRunning = 1; g_inFight = 1; }
    if (tgt != 0 && g_prevTarget == 0) { QueryPerformanceCounter(&g_realEnd); g_realRunning = 0; g_inFight = 0; }
    if (g_inFight && ep == 0 && g_prevEnemyPt != 0) { QueryPerformanceCounter(&g_realEnd); g_realRunning = 0; g_inFight = 0; }
    g_prevBt = bt; g_prevTarget = tgt; g_prevEnemyPt = ep;

    int tpShow = (g_warpPrompt != 0) || ((int)(g_flashUntil - GetTickCount()) > 0);  // warp UI up
    int showBattle = inBattle && g_battleOverlay;  // battle timer + HP only when the toggle is ON
    // showPicker and black MUST both appear in the early-out below: they are drawn further down, so
    // leaving either out gates it on the other indicators (an open list vanished once the open
    // flash expired). black is also computed before the font, which the blackout does not need.
    int showPicker = g_bsOpen && g_bsCount > 0;
    int showSort = g_ssOpen;
    int showLsel = g_lselOpen;
    int showEdit = g_labelEdit;
    int black = blackout_on();
    // Enforced on the render thread because a stalled worker cannot enforce anything; the 2s delay
    // is what keeps the frame between "menu opened" and "input gate armed" from counting as lapsed.
    {
        static DWORD staleSince = 0;
        if ((showSort || showLsel) && !showEdit && di_gate_left_ms() <= 0) {
            DWORD nowT = GetTickCount();
            if (!staleSince) staleSince = nowT;
            else if (nowT - staleSince > 2000) {
                staleSince = 0;
                g_ssOpen = 0; g_lselOpen = 0;
                showSort = showLsel = 0;
                ff13loga("[FF13-SRP] save menu: the input gate lapsed under it -- closed\n");
            }
        } else staleSince = 0;
    }
    // Called on every pass, before the early-out: it carries the screen's own lapse guard.
    int showSetup = setup_visible() && !showSort && !showLsel && !showEdit;
    // g_fontTried holds the first pass open even with nothing to show: the fonts must exist
    // BEFORE the first flash_say resolves its language, or a JP session's first flash comes
    // out English (ui_text falls back while g_fontJp is still NULL).
    if (!a && !s && !w && !showBattle && !tpShow && !showPicker && !showSort
        && !showLsel && !showEdit && !black && !showSetup && g_fontTried) return;

    // Draw ONLY on the pass whose render target is the back buffer. FF13 calls EndScene many times
    // per frame (scene -> intermediate RTs -> post-processing -> back buffer), and drawing our
    // bright text on an intermediate RT lets the game's bloom smear it across the whole screen.
    void** dvt = *(void***)dev;
    if (dev != g_bbCacheDev) {
        void* bb = NULL;
        ((GetBackBuffer_t)dvt[D3DDEV_VT_GETBACKBUFFER])(dev, 0, 0, 0 /*D3DBACKBUFFER_TYPE_MONO*/, &bb);
        if (!bb) return;                       // cannot identify the pass -- same outcome as before
        ((Release_t)(*(void***)bb)[IUNK_VT_RELEASE])(bb);   // identity kept, reference dropped
        g_bbCache = bb; g_bbCacheDev = dev;
        // D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality,
        // Width, Height -- eight DWORDs, so the width is the seventh. IDirect3DSurface9::GetDesc
        // is vtable slot 12.
        {
            DWORD d[8] = {0};
            typedef HRESULT (__stdcall *GetDesc_t)(void*, void*);
            void** bvt = *(void***)bb;
            if (bvt && ((GetDesc_t)bvt[12])(bb, d) >= 0 && d[6] > 100) g_bbWidth = (int)d[6];
        }
    }
    void* rt = NULL;
    ((GetRenderTarget_t)dvt[D3DDEV_VT_GETRENDERTARGET])(dev, 0, &rt);
    int onBackbuffer = (rt != NULL && rt == g_bbCache);
    if (rt) ((Release_t)(*(void***)rt)[IUNK_VT_RELEASE])(rt);
    // Draw on the FIRST back-buffer pass of the run only. MEASURED 2026-08-27: the frame binds the
    // back buffer for TWO EndScene passes in a row, and drawing on both painted the overlay twice.
    // First-of-run, not second, so a frame with a single bb pass still gets its draw.
    {
        static int prevWasBb = 0;
        int repeatBb = onBackbuffer && prevWasBb;
        prevWasBb = onBackbuffer;
#ifdef FF13_DIAG
        if (g_passN < 48) g_passTrace[g_passN++] = onBackbuffer ? (repeatBb ? 'b' : 'B') : '-';
#endif
        if (!onBackbuffer || repeatBb) return;
    }
#ifdef FF13_DIAG
    g_diagDraws++;
#endif

    // Cover the staging. A clear of the whole target rather than a blended quad: it needs no render
    // state of its own, so there is nothing to save and nothing to put back. The overlay's own
    // lines are drawn after it -- a screen that goes black and says nothing reads as a hang.
    if (black)
        ((Clear_t)dvt[D3DDEV_VT_CLEAR])(dev, 0, NULL, 0x1 /*D3DCLEAR_TARGET*/, 0xFF000000, 1.0f, 0);

    if (!g_font && !g_fontTried &&
        (g_fontAttempts == 0 || (int)(GetTickCount() - g_fontRetryAt) >= 0)) {
        g_fontAttempts++;
        const char* why = NULL;
        HMODULE hx = GetModuleHandleA("d3dx9_43.dll");
        if (!hx) hx = LoadLibraryA("d3dx9_43.dll");
        if (!hx) why = "d3dx9_43.dll could not be loaded";
        if (hx) {
            D3DXCreateFontA_t pCreateFont =
                (D3DXCreateFontA_t)GetProcAddress(hx, "D3DXCreateFontA");
            if (!pCreateFont) why = "d3dx9_43.dll has no D3DXCreateFontA";
            if (pCreateFont) {
                void* font = NULL;
                // Weight=700(bold), CharSet=1(DEFAULT), Quality=5(CLEARTYPE).
                HRESULT hr = pCreateFont(dev, OVL_FONT_H, 0, 700, 1, FALSE, 1, 0, 5, 0, OVL_FONT_FACE, &font);
                if (hr >= 0 && font) {
                    g_font = font;
                    // ASCII into the glyph atlas now, not on a digit's first appearance mid-fight.
                    ((PreloadChars_t)(*(void***)font)[FONT_VT_PRELOADCHARS])(font, 0x20, 0x7E);
                    // A face without the English arrows would still show them, drawn by GDI from a
                    // linked font, so only the face's own glyph table tells.
                    {
                        static const WCHAR kArrows[2] = { 0x25C4, 0x25BA };
                        WORD gi[2];
                        HDC fdc = ((FontGetDC_t)(*(void***)font)[FONT_VT_GETDC])(font);
                        if (fdc && GetGlyphIndicesW(fdc, kArrows, 2, gi, GGI_MARK_NONEXISTING_GLYPHS) == 2)
                            ff13loga("[FF13-SRP] overlay: %s has the English arrows U+25C4/U+25BA: %s"
                                     " (glyphs %u, %u)\n", OVL_FONT_FACE,
                                     (gi[0] != 0xFFFF && gi[1] != 0xFFFF) ? "yes" : "NO",
                                     (unsigned)gi[0], (unsigned)gi[1]);
                    }
                    // Asked for by name ONLY after checking the name is installed: the GDI mapper
                    // never fails, it substitutes.
                    const char* face = font_installed(OVL_FONT_FACE_JP) ? OVL_FONT_FACE_JP : NULL;
                    void* fj = NULL;
                    // CharSet=128 (SHIFTJIS). A NULL face name is the fallback: it lets the mapper
                    // pick whatever Japanese-capable font the machine has.
                    if (pCreateFont(dev, OVL_FONT_H_JP, 0, 700, 1, FALSE, 128, 0, 5, 0, face, &fj) >= 0)
                        g_fontJp = fj;
                    ff13loga("[FF13-SRP] overlay: font ready%s%s\n",
                             g_fontJp ? " (+ Japanese: " : " (no Japanese font -- names hidden)",
                             g_fontJp ? (face ? OVL_FONT_FACE_JP ")" : "system default, "
                                               OVL_FONT_FACE_JP " not installed)") : "");
                    if (g_fontJp) {
                        int aA = font_ascent(g_font), aJ = font_ascent(g_fontJp);
                        if (aA > 0 && aJ > 0) g_jpYoff = aA - aJ;
                        ff13loga("[FF13-SRP] overlay: ascent ascii %d, jp %d -> jp runs drawn %+dpx"
                                 " (one baseline)\n", aA, aJ, g_jpYoff);
                    }
                    D3DXCreateSprite_t pCS =
                        (D3DXCreateSprite_t)GetProcAddress(hx, "D3DXCreateSprite");
                    void* sp = NULL;
                    if (pCS && pCS(dev, &sp) >= 0 && sp) g_sprite = sp;
                    ff13loga("[FF13-SRP] overlay: %s\n",
                             g_sprite ? "text batched through one sprite"
                                      : "no sprite -- per-call draws");
                } else {
                    static char buf[48];
                    snprintf(buf, sizeof buf, "D3DXCreateFontA failed hr=0x%08X", (unsigned)hr);
                    why = buf;
                }
            }
        }
        if (g_font) {
            g_fontTried = 1;
        } else if (g_fontAttempts >= OVL_FONT_TRIES) {
            g_fontTried = 1;
            ff13loga("[FF13-SRP] overlay: %s -- gave up after %d tries, no overlay text\n",
                     why ? why : "font not created", g_fontAttempts);
        } else {
            g_fontRetryAt = GetTickCount() + OVL_FONT_RETRY_MS;
            ff13loga("[FF13-SRP] overlay: %s (try %d of %d, again in %d s)\n", why ? why : "font not created",
                     g_fontAttempts, OVL_FONT_TRIES, OVL_FONT_RETRY_MS / 1000);
        }
    }
    if (!g_font) return;

    // Plain ALPHABLEND on purpose: SORT_TEXTURE reorders across textures, and a JP run's shadow
    // (2px off) could then land on top of its ASCII neighbour's body.
    if (g_sprite) {
        void** svt = *(void***)g_sprite;
        if (svt && ((SpriteBegin_t)svt[SPRITE_VT_BEGIN])(g_sprite, D3DXSPRITE_ALPHABLEND_F) >= 0)
            g_spriteActive = g_sprite;
    }

    // One row clear of the top edge: Steam's FPS counter and the like sit in the screen corners,
    // and drawn over them the first line (a menu title, the battle timer) is unreadable.
    int y = g_ovlTopPx;
    char buf[288], key[32];      // buf holds a formatted line -- see g_flash on the size
    if (showSetup) {
        g_passDev = dev;
        y = setup_draw(14, y);
        g_passDev = NULL;
    }

    // Battle picker: the scenes this zone has loaded, as a scrolling window around the selection.
    // Drawn BEFORE the status lines: stacked under them it slid down the screen as features were
    // toggled on, which read as "the picker stopped opening".
    if (showPicker) {
        char up[32], down[32], left[32], right[32], pick[32], cancel[32];
        vk_label(g_bsUpVk, up, sizeof up);         vk_label(g_bsDownVk, down, sizeof down);
        vk_label(g_bsLeftVk, left, sizeof left);   vk_label(g_bsRightVk, right, sizeof right);
        // The start key named here is the picker's own -- held, not tapped, as the text says.
        vk_label(g_bsVk, pick, sizeof pick);         vk_label(g_warpCancelVk, cancel, sizeof cancel);
        snprintf(buf, sizeof buf, ui_text(T_PICKER_HEAD), g_bsSel + 1, g_bsCount);
        overlay_say(g_font, 14, y, buf, OVL_TEXT); y += 24;
        int sel = g_bsSel, cnt = g_bsCount;
        int first = sel - BS_ROWS / 2; if (first < 0) first = 0;
        if (first > cnt - BS_ROWS) first = cnt - BS_ROWS;
        if (first < 0) first = 0;
        // Every field is padded to a fixed width and the name column sits at a fixed offset --
        // placed after the longest line on screen instead, a single <MISSION> or "1/2" mark
        // scrolling into view shifts the whole name column sideways.
        char rows[BS_ROWS][80];
        const unsigned short* tags[BS_ROWS];
        int nrows = 0;
        for (int i = first; i < cnt && i < first + BS_ROWS; i++) {
            const ff13_btsc_info* bi = btsc_info(g_bsList[i]);
            // The picker table's own kind column decides first and the game's own flags (bt_scene.wdb) are
            // the fallback: the game's label is a starting point rather than the last word -- it
            // calls a good many story fights nothing at all. <STORY> stays derived, meaning "this
            // save has seen a script start it", which is not something anyone can type in.
            int hkind = 0;
            btsc_hand(g_bsList[i], 0, NULL, NULL, NULL, NULL, &hkind);
            const char* label =
                hkind == FF13_NAME_KIND_BOSS           ? "<BOSS>" :
                hkind == FF13_NAME_KIND_EVENT          ? "<EVENT>" :
                hkind == FF13_NAME_KIND_MISSION        ? "<MISSION>" :
                (bi && (bi->kind & FF13_BTSC_BOSS))    ? "<BOSS>" :
                (bi && (bi->kind & FF13_BTSC_EVENT))   ? "<EVENT>" :
                (bi && (bi->kind & FF13_BTSC_MISSION)) ? "<MISSION>" :
                is_boss_scene(g_bsList[i])             ? "<STORY>" : "";
            // The tag is UTF-16 (the marks people want do not survive the ASCII path), so its
            // column is left blank here and drawn over the top below.
            tags[nrows] = NULL;
            btsc_hand(g_bsList[i], g_bsVar[i], NULL, NULL, NULL, &tags[nrows], NULL);
            snprintf(rows[nrows], sizeof rows[0], "%s btsc%05u  %-9s %-2s %-3s %-4s",
                     (i == sel) ? ">" : " ", g_bsList[i], label,
                     scene_has_cutscene(g_bsList[i]) ? "CS" : "", "",
                     (g_bsList[i] == g_lastBtscId) ? "here" : "");
            nrows++;
        }
        {   // What each fight is called, in a second column. English goes through the ASCII path
            // and the ordinary font; Japanese needs the separate SHIFTJIS one, which a machine may
            // not have. A row with no English name yet falls back to the Japanese.
            int english = ui_english();
            int nameX = 14 + (BS_NAME_COL + 1) * overlay_char_w(g_font);
            // Counted off the format string above, the same way BS_NAME_COL is -- every literal
            // and every field width up to the tag:
            //   "%s"(1) " btsc"(5) "%05u"(5) "  "(2) "%-9s"(9) " "(1) "%-2s"(2) " "(1) = 26.
            int tagX  = 14 + 26 * overlay_char_w(g_font);
            for (int r = 0; r < nrows; r++) {
                int i = first + r;
                DWORD col = (i == sel) ? OVL_PICK_SEL : OVL_SWAP;
                overlay_line(g_font, 14, y, rows[r], col);
                if (tags[r]) overlay_line_w(tagX, y, tags[r], col);
                const unsigned short* jp = NULL;
                const char* en = NULL;
                btsc_hand(g_bsList[i], g_bsVar[i], &jp, english ? &en : NULL, NULL, NULL, NULL);
                if (en)      overlay_line(g_font, nameX, y, en, col);
                else if (jp) overlay_line_w(nameX, y, jp, col);
                y += 24;
            }
        }
        char foot[288];
        snprintf(foot, sizeof foot, ui_text(T_PICKER_FOOT),
                 up, down, left, right, pick, cancel);
        overlay_say(g_font, 14, y, foot, OVL_TEXT); y += 24;
        // The status lines follow rather than being skipped: skipped, toggling game-speed or the
        // kill mode with the list open did nothing visible. Below the list, they move nothing
        // above them.
        y += 12;                   // half a line, so the two blocks read as two blocks
    }

    // Drawn before the save-list menu below, so the editor sits above it on screen.
    if (showEdit) {
        int em = edit_mode();
        overlay_backdrop(dev, 0.f, (float)(y - 4), 4096.f, (float)(y + 5 * 24 + 8), 0x99000000);
        if (em == EDIT_LABEL) snprintf(buf, sizeof buf, ui_text(T_LABEL_EDIT_HEAD), g_labelEditSlot + 1);
        else snprintf(buf, sizeof buf, "%s",
                      ui_text(em == EDIT_CAT_RENAME  ? T_SS_CAT_RENAME_HEAD
                            : em == EDIT_CAT_NEW_ALL ? T_SS_CAT_NAME_HEAD_ALL
                                                     : T_SS_CAT_NAME_HEAD));
        overlay_say(g_font, 14, y, buf, OVL_BATTLE); y += 24;
        {
            const unsigned short* t = (const unsigned short*)label_edit_text();
            int x0 = 14 + 4 * overlay_char_w(g_font);
            overlay_line(g_font, 14, y, "  >", OVL_MENU_SEL);
            if (t && t[0]) overlay_line_w(x0, y, t, OVL_MENU_SEL);
            overlay_line(g_font, x0 + overlay_width_w(t), y, "_", OVL_MENU_SEL);
            y += 24;
        }
        if (em != EDIT_LABEL) {
            int b = label_edit_bad();
            y += 24 * overlay_say_wrap(g_font, 14, 14, y,
                                       ui_text(b == EDIT_BAD_EXISTS ? T_SS_CAT_EXISTS
                                             : b == EDIT_BAD_NAME   ? T_SS_CAT_BADNAME
                                                                    : T_SS_CAT_EDIT_HELP),
                                       b ? OVL_INSTANT : OVL_TEXT, 24);
        } else if (label_edit_bad()) {
            snprintf(buf, sizeof buf, ui_text(T_LABEL_EDIT_FULL), LABEL_TEXT_MAX);
            y += 24 * overlay_say_wrap(g_font, 14, 14, y, buf, OVL_INSTANT, 24);
        } else {
            y += 24 * overlay_say_wrap(g_font, 14, 14, y, ui_text(T_LABEL_EDIT_HELP), OVL_TEXT, 24);
        }
        { char lk[32], ok[40]; vk_label(g_ssVk, lk, sizeof lk); tidy_ok_phrase(ok, sizeof ok);
          snprintf(buf, sizeof buf,
                   ui_text(em == EDIT_LABEL ? T_LABEL_EDIT_KEYS : T_SS_CAT_EDIT_KEYS), ok, lk); }
        overlay_say(g_font, 14, y, buf, OVL_TEXT); y += 24;
        y += 12;
    }

    if (showSort) {
        char up[32], down[32], cancel[32], ok[40];
        vk_label(g_bsUpVk, up, sizeof up);   vk_label(g_bsDownVk, down, sizeof down);
        vk_label(g_warpCancelVk, cancel, sizeof cancel); tidy_ok_phrase(ok, sizeof ok);
        static const ovl_msg kModes[SS_MODES] = { T_SS_FILTER_ALL,
                                                  T_SS_CAT_PICK_NONE, T_SS_CAT_NEW,
                                                  T_SS_CAT_NEW_ALL, T_SS_CAT_RENAME,
                                                  T_SS_CAT_DEL_IDLE,
                                                  T_SS_ORDER_DATE,
                                                  T_SS_RENUM };
        // The order row draws its current value, so its entry above is only the first of these.
        static const ovl_msg kOrder[SS_ORDER_MODES] = { T_SS_ORDER_DATE, T_SS_ORDER_ASC,
                                                        T_SS_ORDER_DESC, T_SS_ORDER_NUMBER };
        // The heading each row opens a group with; -1 = the row continues the group above it.
        static const int kGroup[SS_MODES] = { T_SS_GRP_FILTER,
                                              T_SS_GRP_CAT, -1, -1, -1, -1,
                                              T_SS_GRP_SORT,
                                              T_SS_GRP_RENUM };
        int sel = g_ssSel;
        char val[160], pickA[320], swA[320];
        const char* pick = cat_name_sel();
        if (pick) {
            char pp[240];
            snprintf(pp, sizeof pp, "%s", pick);
            fit_cat(pp, sizeof pp, NULL);
            ui_arrows(pickA, sizeof pickA, pp);
        }
        {
            const char* sw = cat_sw_name_sel();
            char pp[240];
            snprintf(pp, sizeof pp, "%s", sw ? sw : ui_text(T_SS_CAT_UNPICKED));
            fit_cat(pp, sizeof pp, ui_text(T_SS_CAT_UNPICKED));
            ui_arrows(swA, sizeof swA, pp);
        }
        static char rowT[SS_MODES][256];
        DWORD rowC[SS_MODES];
        for (int i = 0; i < SS_MODES; i++) {
            char* row = rowT[i];
            size_t rn = sizeof rowT[i];
            if (i == SS_FILTER) {
                if (g_filterChapter)
                    snprintf(val, sizeof val, ui_text(T_SS_FILTER_ONE),
                             g_filterChapter, filter_count_of(g_filterChapter));
                else snprintf(val, sizeof val, "%s", ui_text(T_SS_FILTER_ALL));
                static char chs[32][64];
                const char* cands[33];
                int cn = 0;
                cands[cn++] = ui_text(T_SS_FILTER_ALL);
                for (int c = 1; c < 32; c++) {
                    if (filter_count_of(c) <= 0) continue;
                    snprintf(chs[c], sizeof chs[c], ui_text(T_SS_FILTER_ONE), c, filter_count_of(c));
                    cands[cn++] = chs[c];
                }
                ovl_fit(val, sizeof val, cands, cn);
                ui_arrows(row, rn, val);
            } else if (i == SS_CAT_PICK && cat_count())
                snprintf(row, rn, ui_text(T_SS_CAT_PICK), swA, cat_name_cur());
            else if (i == SS_CAT_PICK)
                snprintf(row, rn, ui_text(T_SS_CAT_PICK_NONE), cat_name_cur());
            else if (i == SS_ORDER)
            {
                const char* cands[SS_ORDER_MODES];
                for (int o = 0; o < SS_ORDER_MODES; o++) cands[o] = ui_text(kOrder[o]);
                snprintf(val, sizeof val, "%s",
                         cands[(g_sortOrder >= 0 && g_sortOrder < SS_ORDER_MODES) ? g_sortOrder : 0]);
                ovl_fit(val, sizeof val, cands, SS_ORDER_MODES);
                ui_arrows(row, rn, val);
            }
            else if (i == SS_CAT_DEL && pick && cat_del_armed())
                // The armed variant also wants the confirm phrase; the idle one ignores it.
                snprintf(row, rn, ui_text(T_SS_CAT_DEL_ARM), pick, ok);
            else if (i == SS_CAT_DEL && pick)
                snprintf(row, rn, ui_text(T_SS_CAT_DEL), pickA, ok);
            else if (i == SS_RENUM && renum_armed_n())
                snprintf(row, rn, ui_text(T_SS_RENUM_ARM), renum_armed_n(), ok);
            else if (i == SS_CAT_NEW || i == SS_CAT_NEW_ALL || i == SS_CAT_RENAME)
                snprintf(row, rn, "%s%s", ui_text(kModes[i]), ui_ellipsis());
            else
                snprintf(row, rn, "%s", ui_text(kModes[i]));
            rowC[i] = (i == sel) ? OVL_MENU_SEL : OVL_MENU;
        }
        int selY = y + 24;
        for (int i = 0; i < SS_MODES; i++) {
            if (kGroup[i] >= 0 && i <= sel) selY += 24;
            if (i < sel) selY += 24;
        }
        int rows = SS_MODES + 6;              // title + four headings + the rows + the footer
        overlay_backdrop(dev, 0.f, (float)(y - 4), 4096.f, (float)(y + rows * 24 + 8), 0x99000000);
        if (sel >= 0 && sel < SS_MODES) overlay_band(dev, selY, 1);
        overlay_say(g_font, 14, y, ui_text(T_SS_HEAD), OVL_BATTLE); y += 24;
        for (int i = 0; i < SS_MODES; i++) {
            if (kGroup[i] >= 0) {
                overlay_say(g_font, 14, y, ui_text((ovl_msg)kGroup[i]), OVL_FLASH); y += 24;
            }
            snprintf(buf, sizeof buf, "   %s %s", (i == sel) ? ">" : " ", rowT[i]);
            overlay_say(g_font, 14, y, buf, rowC[i]); y += 24;
        }
        if (g_ssSel == SS_FILTER || g_ssSel == SS_CAT_PICK || g_ssSel == SS_CAT_DEL
            || g_ssSel == SS_ORDER) {
            char l[32], r[32];
            vk_label(g_bsLeftVk, l, sizeof l); vk_label(g_bsRightVk, r, sizeof r);
            snprintf(buf, sizeof buf,
                     ui_text(g_ssSel == SS_FILTER   ? T_SS_FILTER_KEYS
                           : g_ssSel == SS_ORDER    ? T_SS_ORDER_KEYS
                           : g_ssSel == SS_CAT_PICK ? T_SS_CAT_KEYS
                                                    : T_SS_CAT_DEL_KEYS), l, r, ok);
        }
        else { char tk[32]; vk_label(g_ssVk, tk, sizeof tk);
               snprintf(buf, sizeof buf, ui_text(T_SS_FOOT), up, down, ok, cancel, tk); }
        overlay_say(g_font, 14, y, buf, OVL_TEXT); y += 24;
        y += 12;
    }

    if (showLsel) {
        char up[32], down[32], l[32], r[32], k[32], ok[40];
        vk_label(g_bsUpVk, up, sizeof up);       vk_label(g_bsDownVk, down, sizeof down);
        vk_label(g_bsLeftVk, l, sizeof l);       vk_label(g_bsRightVk, r, sizeof r);
        vk_label(g_ssVk, k, sizeof k);           tidy_ok_phrase(ok, sizeof ok);
        // No group headings: every row acts on the save in the title, and the range rows must not
        // read as part of the send.
        int sel = g_lselSel;
        static char rowT[LSEL_ROWS][256];
        DWORD rowC[LSEL_ROWS];
        {
            int anc = range_anchor(), rn = range_shown_count();
            const char* dest = send_dest_name();
            char destA[320], dp[240];
            snprintf(dp, sizeof dp, "%s", dest ? dest : ui_text(T_SEND_NEWCAT));
            fit_cat(dp, sizeof dp, ui_text(T_SEND_NEWCAT));
            ui_arrows(destA, sizeof destA, dp);
            for (int i = 0; i < LSEL_ROWS; i++) {
                char* row = rowT[i];
                size_t n = sizeof rowT[i];
                if (i == LSEL_MARK && anc >= 0)
                    snprintf(row, n, "%s", ui_text(T_LSEL_MARK_CLR));
                else if (i == LSEL_MARK)
                    snprintf(row, n, ui_text(T_LSEL_MARK_SET), g_lselSlot + 1);
                else if (i == LSEL_DEL && range_del_armed())
                    snprintf(row, n, ui_text(T_LSEL_DEL_ARM), range_del_count(), ok);
                else if (i == LSEL_DEL && range_del_ready())
                    snprintf(row, n, ui_text(T_LSEL_DEL), anc + 1, g_lselSlot + 1, rn);
                else if (i == LSEL_DEL)
                    snprintf(row, n, "%s", ui_text(T_LSEL_DEL_IDLE));
                else if ((i == LSEL_COPY || i == LSEL_MOVE) && anc >= 0 && rn > 0)
                    snprintf(row, n, ui_text(i == LSEL_COPY ? T_SEND_COPY_N : T_SEND_MOVE_N),
                             anc + 1, g_lselSlot + 1, rn, destA);
                else if (i == LSEL_COPY || i == LSEL_MOVE)
                    snprintf(row, n, ui_text(i == LSEL_COPY ? T_SEND_COPY : T_SEND_MOVE), destA);
                else
                    snprintf(row, n, "%s%s", ui_text(T_LSEL_NOTE), ui_ellipsis());
                // A row that cannot run stays dim even under the cursor: the ">" marks where you are.
                rowC[i] = (i == LSEL_DEL && !range_del_ready()) ? OVL_MENU_OFF
                        : (i == sel) ? OVL_MENU_SEL : OVL_MENU;
            }
        }
        int rows = LSEL_ROWS + 2;             // title + the rows + the footer
        overlay_backdrop(dev, 0.f, (float)(y - 4), 4096.f, (float)(y + rows * 24 + 8), 0x99000000);
        if (sel >= 0 && sel < LSEL_ROWS) overlay_band(dev, y + (1 + sel) * 24, 1);
        // The key that changes WHICH save rides the line that names it, not the footer.
        char pick[80] = "";
        if (g_listPrevVk && g_listNextVk) {
            char pp[32], pn[32];
            vk_label(g_listPrevVk, pp, sizeof pp);
            vk_label(g_listNextVk, pn, sizeof pn);
            snprintf(pick, sizeof pick, ui_text(T_LSEL_PICK), pp, pn);
        }
        snprintf(buf, sizeof buf, ui_text(T_LSEL_HEAD), g_lselSlot + 1, pick);
        overlay_say(g_font, 14, y, buf, OVL_BATTLE); y += 24;
        for (int i = 0; i < LSEL_ROWS; i++) {
            snprintf(buf, sizeof buf, "   %s %s", (i == sel) ? ">" : " ", rowT[i]);
            overlay_say(g_font, 14, y, buf, rowC[i]); y += 24;
        }
        // No cancel key here: the game's own save list reads that one as "back" and would take
        // itself away with it.
        if (g_lselSel == LSEL_NOTE || g_lselSel == LSEL_MARK || g_lselSel == LSEL_DEL)
            snprintf(buf, sizeof buf, ui_text(T_LSEL_KEYS), up, down, ok, k);
        else snprintf(buf, sizeof buf, ui_text(T_LSEL_SEND_KEYS), up, down, l, r,
                      ui_text(g_lselSel == LSEL_COPY ? T_SEND_DEST_COPY : T_SEND_DEST_MOVE), ok, k);
        overlay_say(g_font, 14, y, buf, OVL_TEXT); y += 24;
        y += 12;
    }


    // One line for whatever the kill mode is set to; instant-win gets the louder colour and sits
    // above game-speed.
    if (a) {
        vk_label(g_armVk, key, sizeof key);
        snprintf(buf, sizeof buf, ui_text(T_KILL_LINE), key, kill_mode_name(g_killMode));
        overlay_say(g_font, 14, y, buf,
                     g_killMode == KILL_INSTANT ? OVL_INSTANT : OVL_ONEHIT); y += 24;
    }
    if (w) {                                       // lime-green, field + battle
        vk_label(g_gsVk, key, sizeof key);
        snprintf(buf, sizeof buf, ui_text(T_GAMESPEED), key, g_gsFactor);
        overlay_say(g_font, 14, y, buf, OVL_GAMESPEED); y += 24;
    }
    if (s && !inBattle) {                          // violet, field only
        vk_label(g_supVk, key, sizeof key);
        snprintf(buf, sizeof buf, ui_text(T_SUPPRESS), key);
        overlay_say(g_font, 14, y, buf, OVL_SUPPRESS); y += 24;
    }
    // A fight is armed and has not started yet: the gap between choosing one and being in it.
    if (g_swapEnabled && g_swapBtscId) {           // amber, field + battle
        overlay_say(g_font, 14, y, ui_text(T_SWAP), OVL_SWAP); y += 24;
    }
    if (showBattle) {
        char t[24], lbl[24];
        overlay_battle_head(g_font, y, ui_text(T_BATTLE_HEAD_TIME)); y += 24;
        int gms = battle_gametime_ms();
        if (gms >= 0) {
            fmt_mmsscc(gms, t, sizeof t);
            overlay_battle_row(g_font, y, ui_text(T_TIMER_GAME), t); y += 24;
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        LONGLONG end = g_realRunning ? now.QuadPart : g_realEnd.QuadPart;
        int rms = (g_qpcFreq.QuadPart > 0)
                    ? (int)((end - g_realStart.QuadPart) * 1000 / g_qpcFreq.QuadPart) : 0;
        fmt_mmsscc(rms, t, sizeof t);
        overlay_battle_row(g_font, y, ui_text(T_TIMER_REAL), t); y += 24;
        int left = 0, period = 0;
        if (atb_shift_left_ms(&left, &period)) {
            // The trailing space is load-bearing: the row is right-aligned as a whole and GAME /
            // REAL carry two decimals where this carries one -- without it the points misalign.
            if (left <= 0) snprintf(t, sizeof t, "READY ");
            else           snprintf(t, sizeof t, "%d.%d ", left / 1000, (left % 1000) / 100);
            snprintf(lbl, sizeof lbl, ui_text(T_ATB_SHIFT), (period + 500) / 1000);
            overlay_battle_row(g_font, y, lbl, t); y += 24;
        }
        int hp = read_via_ptr(RVA_TARGETHP_PTR, OFF_TARGETHP);       // current target HP (asl)
        if (hp > 0) {
            overlay_battle_head(g_font, y, ui_text(T_BATTLE_HEAD_TARGET)); y += 24;
            snprintf(t, sizeof t, "%d", hp);
            overlay_battle_row(g_font, y, ui_text(T_ENEMY_HP), t); y += 24;
            uintptr_t a = target_actor(hp);
            if (a) {
                // 1000 raw = one gauge segment (the unit the player's own bar shows); 100 raw =
                // the cost of one ability point.
                int atb = *(int*)(a + OFF_CHARA_ATB); if (atb < 0) atb = 0;
                snprintf(t, sizeof t, "%d.%d ", atb / 1000, (atb % 1000) / 100);
                overlay_battle_row(g_font, y, ui_text(T_ENEMY_ATB), t); y += 24;
                int st = *(int*)(a + OFF_CHARA_CHAIN + OFF_CHAIN_STATE);
                int cl = *(int*)(a + OFF_CHARA_CHAIN + OFF_CHAIN_LEFT);
                int chain = (st && cl > 0) ? cl : 0;   // state 0: +0x18 is stale, not short
                snprintf(t, sizeof t, "%d.%d ", chain / 1000, (chain % 1000) / 100);
                overlay_battle_row(g_font, y, ui_text(T_CHAIN_LEFT), t); y += 24;
            }
        }
    }
    // Warp in-game dialog: the confirm question with its OK/Cancel keys, or a transient status line.
    if (g_warpPrompt != 0) {
        overlay_say(g_font, 14, y, g_warpPromptMsg, OVL_WARP_PROMPT); y += 24;
        char again[32], cancel[32], line[224];
        // The warp key is named twice on purpose: it answers the question, and the same key held is
        // the OTHER thing it does (moving the point being asked about).
        vk_label(g_warpVk, again, sizeof again); vk_label(g_warpCancelVk, cancel, sizeof cancel);
        snprintf(line, sizeof line, ui_text(T_WARP_ASK_KEYS), again, again, cancel);
        overlay_say(g_font, 14, y, line, OVL_WARP_PROMPT); y += 24;
    } else if ((int)(g_flashUntil - GetTickCount()) > 0) {
        y += 24 * overlay_say_wrap(g_font, 14, 14, y, g_flash, g_flashCol, 24);
    }

    if (g_spriteActive) {                 // buffered text renders here, over any backdrop quad
        void* sp = g_spriteActive;
        g_spriteActive = NULL;
        ((FontOp_t)(*(void***)sp)[SPRITE_VT_END])(sp);
    }
}

// EndScene hook: draw first, then chain to the original.
static HRESULT __stdcall hkEndScene(void* dev) {
#ifdef FF13_DIAG
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    overlay_draw(dev);
    QueryPerformanceCounter(&t1);
    overlay_diag(t1.QuadPart - t0.QuadPart);
#else
    overlay_draw(dev);
#endif
    EndScene_t o = (EndScene_t)g_origEndScene;
    return o ? o(dev) : 0;
}

static void com_op(void* obj, int slot) {
    if (!obj) return;
    void** vt = *(void***)obj;
    if (vt && vt[slot]) ((FontOp_t)vt[slot])(obj);
}

// Reset hook: release GPU-side font resources across a device reset (resolution change / device
// lost), chain to the original, then restore them on success.
static HRESULT __stdcall hkReset(void* dev, FF13_D3DPP* pp) {
    g_bbCache = NULL; g_bbCacheDev = NULL; g_bbWidth = 0;   // Reset recreates it; refetch lazily
    // ALL THREE, or the game's own Reset fails with D3DERR_INVALIDCALL: each holds default-pool
    // GPU objects, and one left unreleased is enough.
    com_op(g_font,   FONT_VT_ONLOST);
    com_op(g_fontJp, FONT_VT_ONLOST);
    com_op(g_sprite, SPRITE_VT_ONLOST);
    if (g_backdropSB) {
        ((Release_t)(*(void***)g_backdropSB)[IUNK_VT_RELEASE])(g_backdropSB);
        g_backdropSB = NULL;
    }
    Reset_t o = (Reset_t)g_origReset;
    HRESULT hr = o ? o(dev, pp) : 0;
    if (hr >= 0) {
        com_op(g_font,   FONT_VT_ONRESET);
        com_op(g_fontJp, FONT_VT_ONRESET);
        com_op(g_sprite, SPRITE_VT_ONRESET);
    }
    return hr;
}

// Patch one vtable slot to `repl`, saving the original into *saved.
static int patch_vtable_slot(void** vtbl, int slot, void* repl, void** saved) {
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (saved) *saved = vtbl[slot];
    vtbl[slot] = repl;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void*));
    return 1;
}

// One-time overlay init. Fully non-fatal: on any failure the overlay is absent and the mod
// continues.
// Patches EndScene/Reset in `dvt` and says whose vtable it was (d3d9.dll, a wrapper, DXVK...).
static void overlay_hook_vtable(void** dvt, const char* via) {
    int okEnd = patch_vtable_slot(dvt, D3DDEV_VT_ENDSCENE, (void*)hkEndScene, &g_origEndScene);
    patch_vtable_slot(dvt, D3DDEV_VT_RESET, (void*)hkReset, &g_origReset);
    char mod[MAX_PATH] = "?";
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)g_origEndScene, &hm) && hm) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(hm, full, sizeof(full))) {
            const char* b = strrchr(full, '\\');
            snprintf(mod, sizeof(mod), "%s+0x%X", b ? b + 1 : full,
                     (unsigned)((uintptr_t)g_origEndScene - (uintptr_t)hm));
        }
    }
    if (okEnd) {
        g_overlayReady = 1;
        ff13loga("[FF13-SRP] overlay: EndScene hooked (%s, in %s)\n", via, mod);
    } else {
        ff13loga("[FF13-SRP] overlay: EndScene vtable patch failed -> no overlay\n");
    }
}

void overlay_init(void) {
    QueryPerformanceFrequency(&g_qpcFreq);         // for the REAL battle timer (own clock)
    // The game's own device first: its vtable is the one EndScene is called through, whatever
    // flags or wrapper made it. A stand-in device only shares it when a wrapper (FF13Fix, DXVK)
    // hands every device one vtable -- plain d3d9.dll gives the game's PUREDEVICE its own.
    {
        void* volatile* pDev = (void* volatile*)(g_base + RVA_D3DDEV_PTR);
        void* dev = NULL;
        for (int i = 0; i < 150 && !(dev = *pDev); i++) Sleep(200);
        if (dev && *(void***)dev) { overlay_hook_vtable(*(void***)dev, "the game's device"); return; }
        ff13loga("[FF13-SRP] overlay: the game's D3D9 device did not appear within 30 s -- "
                 "hooking through a stand-in device instead\n");
    }
    // d3d9 should already be loaded by the game; wait up to ~10s, then give up.
    HMODULE hD3D9 = NULL;
    for (int i = 0; i < 50; i++) {
        hD3D9 = GetModuleHandleA("d3d9.dll");
        if (hD3D9) break;
        Sleep(200);
    }
    if (!hD3D9) hD3D9 = LoadLibraryA("d3d9.dll");
    // Always logged, not verbose: each of these leaves the overlay blank, and a default-level log
    // that says nothing about the overlay cannot be told from one where it never started.
    if (!hD3D9) { ff13loga("[FF13-SRP] overlay: d3d9 not found -> no overlay\n"); return; }

    Direct3DCreate9_t pDirect3DCreate9 =
        (Direct3DCreate9_t)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pDirect3DCreate9) { ff13loga("[FF13-SRP] overlay: Direct3DCreate9 missing -> no overlay\n"); return; }

    void* d3d = pDirect3DCreate9(32);              // D3D_SDK_VERSION = 32
    if (!d3d) { ff13loga("[FF13-SRP] overlay: Direct3DCreate9 returned NULL -> no overlay\n"); return; }

    // Hidden throwaway window to own the dummy device.
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "FF13IWOverlayWnd";
    RegisterClassA(&wc);                            // dup registration is harmless
    HWND hwnd = CreateWindowExA(0, "FF13IWOverlayWnd", "ff13iw", WS_OVERLAPPED,
                                0, 0, 32, 32, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        ((Release_t)(*(void***)d3d)[IUNK_VT_RELEASE])(d3d);
        ff13loga("[FF13-SRP] overlay: temp window creation failed\n");
        return;
    }

    FF13_D3DPP pp; memset(&pp, 0, sizeof(pp));
    pp.Windowed         = TRUE;
    pp.SwapEffect       = 1;                        // D3DSWAPEFFECT_DISCARD
    pp.BackBufferFormat = 0;                        // D3DFMT_UNKNOWN
    pp.BackBufferCount  = 1;
    pp.hDeviceWindow    = hwnd;

    // ONE attempt per device type -- do NOT add a retry loop. One launch in six answers
    // D3DERR_NOTAVAILABLE here (the driver refusing a second HAL device while the game initialises
    // its own on another thread), and the ten-attempt retry version recovered nothing: it crashed
    // the game INSIDE this function, where the one-attempt version merely logs "no overlay".
    void** d3dvt = *(void***)d3d;
    CreateDevice_t pCreateDevice = (CreateDevice_t)d3dvt[16];   // IDirect3D9::CreateDevice
    void* dev = NULL;
    HRESULT hr = pCreateDevice(d3d, 0 /*DEFAULT*/, 1 /*HAL*/, hwnd,
                               0x20 /*SOFTWARE_VERTEXPROCESSING*/, &pp, &dev);
    if (hr < 0 || !dev) {
        // Exclusive fullscreen refuses the second HAL device (D3DERR_DEVICELOST, measured).
        // A NULLREF device touches neither adapter nor driver. Its vtable is the
        // game's only under a wrapper that gives every device one vtable; otherwise the patch
        // lands where the game never calls -- a no-op, not a crash. NOWINDOWCHANGES keeps the
        // dummy from touching focus while the game is fullscreen.
        dev = NULL;
        HRESULT hr2 = pCreateDevice(d3d, 0 /*DEFAULT*/, 4 /*NULLREF*/, hwnd,
                                    0x20 | 0x800 /*NOWINDOWCHANGES*/, &pp, &dev);
        ff13loga("[FF13-SRP] overlay: HAL CreateDevice failed hr=0x%08X -> NULLREF fallback %s "
                 "(hr=0x%08X)\n", (unsigned)hr, (hr2 >= 0 && dev) ? "OK" : "FAIL", (unsigned)hr2);
        if (hr2 < 0 || !dev) {
            DestroyWindow(hwnd);
            UnregisterClassA("FF13IWOverlayWnd", wc.hInstance);
            ((Release_t)(*(void***)d3d)[IUNK_VT_RELEASE])(d3d);
            ff13loga("[FF13-SRP] overlay: no vtable donor -> no overlay\n");
            return;
        }
    }

    // The patch outlives the dummy device; it reaches the game's device only if they share the vtable.
    void** dvt = *(void***)dev;
    overlay_hook_vtable(dvt, "a stand-in device");

    // Tear down the dummy device + window; the patched vtable persists.
    ((Release_t)dvt[IUNK_VT_RELEASE])(dev);
    DestroyWindow(hwnd);
    UnregisterClassA("FF13IWOverlayWnd", wc.hInstance);
    ((Release_t)(*(void***)d3d)[IUNK_VT_RELEASE])(d3d);
}
