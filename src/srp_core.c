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

// FF13-SpeedrunPractice -- speedrun practice DLL for FINAL FANTASY XIII (Steam).
// What -DFF13_DIAG leaves out: build.sh.

// ---- config (ff13-srpractice.ini next to the exe; loaded once at worker start) ----
// These defaults ARE the shipped layout: ff13-srpractice.ini.example, README.ja.md and the key
// editor all name the same map, so changing a default here means changing it in all three.
UINT g_armVk    = VK_F3;       // kill-mode cycle hotkey (default F3)
UINT g_supVk    = VK_F5;       // enemy-group suppression toggle hotkey (default F5)
UINT g_gsVk     = VK_F2;       // game-speed toggle hotkey (default F2)
UINT g_helpVk   = VK_F1;       // hotkey-list overlay toggle (default F1)
UINT g_warpVk     = VK_F4;       // warp: tap = warp, hold = record (default F4)
UINT g_warpCancelVk = VK_BACK;   // warp: cancel the in-overlay prompt (default Backspace;
                                      // NOT Esc -- Esc opens the game's own quit dialog)
UINT g_boVk       = VK_F10;    // battle overlay (timer + enemy HP) toggle (default F10)
UINT g_saveVk     = VK_F7;     // save-screen hotkey (default F7; ini [hotkeys] save overrides)
UINT g_camUnstickVk = VK_F8;  // "un-stick camera" hotkey (ini [hotkeys] camera_unstick; 0 = disabled)
UINT g_ssVk       = VK_F9;     // (default F9; ini [hotkeys] save_tidy)
UINT g_menuOkVk   = VK_RETURN; // tidy-menu confirm (default Enter; ini [hotkeys] menu_ok). Safe as
                               // a game key because those menus hold the DirectInput gate; acted on
                               // ONLY while one of them is up.
// Walks THE GAME'S save selection while our save-list menu is up (default A/D; ini [hotkeys]
// list_prev/list_next; see di_fake_dik -- the gate has the keyboard, so the game is handed a
// keystroke rather than allowed the real one). Nothing of OURS moves on these, and they are acted
// on only over the save list, never in the picker, which holds no gate.
UINT g_listPrevVk  = 'A';
UINT g_listNextVk  = 'D';
// Hotkeys only while the game is the focused window unless this is on. GetAsyncKeyState reads the
// keyboard, not the window, so without the test every key here fires from inside a browser. A
// setting rather than a rule because a wrong focus test takes EVERY hotkey with it, and turning
// this on is the only way back from inside the game.
volatile int g_hkGlobal = 0;
// ---- what the kill hotkey is set to -------------------------------------------------------
// User-facing cycle order, strongest first: OFF -> INSTANT WIN (+ one-hit-kill) -> ONE-HIT KILL ->
// OFF. The two are not redundant: instant-win only acts while a fight is being BUILT, so
// one-hit-kill is what covers the fight you are already in, and any fight the gate finds nothing
// to suppress in.
volatile int g_killMode = KILL_OFF;
volatile unsigned char g_battleOverlay = 1;  // battle timer + enemy HP shown in battle (default ON)
int  g_logLevel = 1;           // 0 quiet / 1 normal / 2 verbose

// ---- game-speed state (the frame-tick pump in srp_speed.c reads these) ----
// [game_speed] multiplier is a LIST; the hotkey steps through it and then off, so a list of one
// behaves as a plain toggle. Which step is not an ini setting -- the sidecar restores it.
double g_gsSteps[GS_STEPS_MAX] = { 3.0 };
int    g_gsStepN = 1;
volatile int g_gsStep = -1;               // -1 = off, else an index into g_gsSteps
volatile unsigned char g_gsEnabled = 0;   // g_gsStep >= 0, kept as its own byte for the pump
double g_gsFactor = 3.0;                  // ...and the multiplier of that step, likewise
// One place decides both, so the pump can never read a factor from a step it is not on.
void gs_apply(int step) {
    if (step >= g_gsStepN) step = -1;
    g_gsStep = step;
    g_gsEnabled = (step >= 0);
    if (step >= 0) g_gsFactor = g_gsSteps[step];
}
// Off first, then the copy, then back on: the pump reads g_gsEnabled/g_gsFactor on the game
// thread, and a step must never point into a half-copied list.
void gs_steps_set(const double* v, int n) {
    if (n < 1) return;
    if (n > GS_STEPS_MAX) n = GS_STEPS_MAX;
    double on = g_gsStep >= 0 ? g_gsSteps[g_gsStep] : 0.0;
    gs_apply(-1);
    for (int i = 0; i < n; i++) g_gsSteps[i] = v[i];
    g_gsStepN = n;
    if (on > 0.0) {
        int at = -1;
        for (int i = 0; i < n; i++) {
            double d = v[i] - on;
            if (d < 0.005 && d > -0.005) { at = i; break; }
        }
        gs_apply(at);
        if (at >= 0)
            ff13log("[FF13-SRP] game speed: list changed, still ON at %.2fx (step %d of %d)\n",
                    g_gsSteps[at], at + 1, n);
        else
            ff13log("[FF13-SRP] game speed: list changed, %.2fx is gone -> OFF\n", on);
    }
    state_save();
}

// ---- ARM state (the runtime gate the cave reads) ----
// A single byte so the cave's `cmp byte ptr [g_armed],0` is endian-agnostic and so reads/writes are
// naturally atomic across the game thread (cave) and the hotkey thread.
volatile unsigned char g_armed = 0;

// ---- logging (next to the exe; its dir is writable) ----
// ff13loga = always; ff13log = normal (g_logLevel>=1); ff13logv = verbose (>=2).
static int exe_sibling(char* out, size_t n, const char* name) {
    char p[MAX_PATH];
    if (!GetModuleFileNameA(NULL, p, MAX_PATH)) return 0;
    char* s = strrchr(p, '\\'); if (!s) return 0;
    *(s + 1) = 0;
    if (strlen(p) + strlen(name) >= n) return 0;
    snprintf(out, n, "%s%s", p, name);
    return 1;
}
// ---- where the files this mod WRITES go -------------------------------------------------------
// Into <exe dir>\ff13-mods\, not loose beside the exe. The ini is the exception and stays beside
// the exe -- you place that one by hand, so it has to be where you would look.
int  g_dataDirOk = -1;             // -1 = not tried, 1 = use the folder, 0 = beside the exe
static char g_dataDirPath[MAX_PATH] = "";  // "<exe dir>\ff13-mods\" once it exists
static void data_dir_ready(void) {
    if (g_dataDirOk >= 0) return;
    char dir[MAX_PATH];
    if (!exe_sibling(dir, sizeof(dir), MOD_DATA_DIR)) { g_dataDirOk = 0; return; }
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_dataDirOk = 0;                  // read-only install or no permission: carry on as before
        return;
    }
    snprintf(g_dataDirPath, sizeof(g_dataDirPath), "%s\\", dir);
    g_dataDirOk = 1;
}
// Falls back to beside the exe if the folder cannot be made, so a locked-down install keeps working
// instead of quietly losing its sidecars.
int data_file(char* out, size_t n, const char* name) {
    data_dir_ready();
    if (g_dataDirOk != 1) return exe_sibling(out, n, name);
    if (strlen(g_dataDirPath) + strlen(name) >= n) return 0;
    snprintf(out, n, "%s%s", g_dataDirPath, name);
    return 1;
}
static void ff13log_write(const char* fmt, va_list ap) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice.log")) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    vfprintf(f, fmt, ap);
    fclose(f);
}
// The diagnostic build keeps the log across launches instead of truncating: a hang investigation
// needs the lines from the launch that froze, which the next launch would otherwise wipe.
#ifndef FF13_BUILD_ID
#define FF13_BUILD_ID "unknown"
#endif
void ff13log_reset(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice.log")) return;
#ifdef FF13_DIAG
    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "\n[FF13-SRP] ==== new session (diagnostic build keeps the log across launches)"
                   " -- build %s ====\n", FF13_BUILD_ID);
        fclose(f);
    }
#else
    // The launch before is kept as prev.log: a crash investigation needs the lines from the run that
    // died, which a fresh log would otherwise wipe.
    char prev[MAX_PATH];
    if (data_file(prev, sizeof(prev), "prev.log")) MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "[FF13-SRP] ==== FF13-SpeedrunPractice, build %s ====\n", FF13_BUILD_ID);
        fclose(f);
    }
#endif
}
void ff13loga(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); ff13log_write(fmt, ap); va_end(ap);
}
void ff13log(const char* fmt, ...) {
    if (g_logLevel < 1) return;
    va_list ap; va_start(ap, fmt); ff13log_write(fmt, ap); va_end(ap);
}
void ff13logv(const char* fmt, ...) {
    if (g_logLevel < 2) return;
    va_list ap; va_start(ap, fmt); ff13log_write(fmt, ap); va_end(ap);
}

// ---- captured state ----
uintptr_t g_base = 0;               // ffXiiiimg.exe base (GetModuleHandle(NULL))

#undef GetTickCount
DWORD srp_ticks(void) {
    static volatile LONG base = 0;
    if (!base) InterlockedCompareExchange(&base, (LONG)(GetTickCount() - 0x10000u), 0);
    return GetTickCount() - (DWORD)base;
}
#define GetTickCount() srp_ticks()

// ---- what language the mod speaks -------------------------------------------------------------
// The game resolves its text language once at startup into a .data global, long after this DLL is
// loaded -- so the value is read late and re-read until it answers (-1 = not yet). Policy:
// Japanese for a Japanese game, English for every other one.
#define RVA_LANGUAGE 0x22A19B8      // VA 0x026A19B8, int
static volatile int g_langSeen = -1;              // what the game last answered, -1 = not yet

// 1 = English.
int ui_english(void) {
    if (!g_base) return 0;
    int idx = *(volatile int*)(uintptr_t)(g_base + RVA_LANGUAGE);
    if (idx < 0 || idx > 10) return g_langSeen > 0;     // not resolved yet -- keep the last answer
    if (idx != g_langSeen) {
        g_langSeen = idx;
        static const char* const kLang[11] =
            { "jp", "us", "uk", "it", "gr", "fr", "sp", "ru", "kr", "ck", "ch" };
        ff13loga("[FF13-SRP] the game is running in \"%s\" -- overlay and fight names in %s\n",
                 kLang[idx], idx ? "English" : "Japanese");
    }
    return idx != 0;
}

// Created on the first overlay draw. NULL means the machine has no SHIFTJIS face and Japanese would
// draw as nothing at all -- so in that case the mod keeps speaking English.
void* g_fontJp = NULL;


// One line of the overlay's own text, in whichever language is in force.
const char* ui_text(ovl_msg m) {
    if ((unsigned)m >= (unsigned)T_COUNT) return "";
    if (ui_ascii_only()) return kOvlEn[m];
    const char* jp = kOvlJp[m];
    return (jp && jp[0]) ? jp : kOvlEn[m];
}
int ui_ascii_only(void) { return ui_english() || !g_fontJp; }
void ui_arrows(char* out, size_t n, const char* value) {
    if (ui_ascii_only()) snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", value ? value : "");
    else snprintf(out, n, "\xE2\x97\x80 %s \xE2\x96\xB6", value ? value : "");
}
const char* ui_ellipsis(void) { return ui_ascii_only() ? "..." : "\xE2\x80\xA6"; }


// ---- config .ini parser (no deps): [section], key = value, ; / # comments ----
static void cfg_trim(char* s) {
    char* p = s;
    while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = 0;
}
int cfg_ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == 0 && *b == 0;
}

// ---- what config_load refused (the help overlay shows it; the log has the line-level detail) ----
static UINT* g_badVk[16];
static int   g_badVkN = 0;
static char  g_cfgBad[160] = "";
static void cfg_note_bad(const char* name) {
    size_t l = strlen(g_cfgBad);
    snprintf(g_cfgBad + l, sizeof g_cfgBad - l, "%s%s", l ? ", " : "", name);
}
static void cfg_bad_vk(const char* name, UINT* vk) {
    if (g_badVkN < (int)(sizeof g_badVk / sizeof g_badVk[0])) g_badVk[g_badVkN++] = vk;
    cfg_note_bad(name);
}
int cfg_vk_bad(const UINT* vk) {
    for (int i = 0; i < g_badVkN; i++) if (g_badVk[i] == vk) return 1;
    return 0;
}
const char* cfg_bad_list(void) { return g_cfgBad; }
void cfg_bad_clear(const char* name) {
    char out[sizeof g_cfgBad] = "";
    size_t o = 0, nl = strlen(name);
    const char* p = g_cfgBad;
    while (*p) {
        const char* e = strstr(p, ", ");
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (!(len == nl && !memcmp(p, name, nl)))
            o += (size_t)snprintf(out + o, sizeof out - o, "%s%.*s", o ? ", " : "", (int)len, p);
        p += len;
        if (e) p += 2;
    }
    memcpy(g_cfgBad, out, sizeof g_cfgBad);
}
void cfg_vk_clear_bad(UINT* vk) {
    int w = 0;
    for (int i = 0; i < g_badVkN; i++) if (g_badVk[i] != vk) g_badVk[w++] = g_badVk[i];
    g_badVkN = w;
    for (int i = 0; i < hk_binding_count(); i++)
        if (hk_binding_vk(i) == vk) cfg_bad_clear(hk_binding_name(i));
}

// [hotkeys] name -> the key it sets. What each one DOES is documented in the README, not here.
static const struct { const char* name; UINT* vk; } kHotkeys[] = {
    { "help",           &g_helpVk },
    { "kill_mode",      &g_armVk },
    { "enemy_suppress", &g_supVk },
    { "game_speed",     &g_gsVk },
    { "warp",           &g_warpVk },
    // Not named after the warp: it answers the warp question AND closes the picker.
    { "cancel",         &g_warpCancelVk },
    { "battle_picker",  &g_bsVk },
    { "battle_overlay", &g_boVk },
    { "picker_up",      &g_bsUpVk },   { "picker_down",  &g_bsDownVk },
    { "picker_left",    &g_bsLeftVk }, { "picker_right", &g_bsRightVk },
    { "save",           &g_saveVk },
    { "camera_unstick", &g_camUnstickVk },
    { "save_tidy",      &g_ssVk },
    { "menu_ok",        &g_menuOkVk },
    { "list_prev",      &g_listPrevVk }, { "list_next",  &g_listNextVk },
};
int hk_binding_count(void) { return (int)(sizeof kHotkeys / sizeof kHotkeys[0]); }
const char* hk_binding_name(int i) {
    return (i >= 0 && i < hk_binding_count()) ? kHotkeys[i].name : NULL;
}
UINT* hk_binding_vk(int i) {
    return (i >= 0 && i < hk_binding_count()) ? kHotkeys[i].vk : NULL;
}
// "3.0x<arrow>OFF" or "2.0x<arrow>3.0x<arrow>OFF" -- the help row's cycle, in the caller's arrow
// (the JP arrow only renders where the JP font does, so the caller picks it with the language).
const char* gs_steps_arrow(const char* arrow) {
    static char buf[GS_STEPS_MAX * 16 + 8];
    int n = 0;
    for (int i = 0; i < g_gsStepN && n < (int)sizeof(buf) - 16; i++) {
        char f[16];
        snprintf(f, sizeof f, "%.2f", g_gsSteps[i]);
        size_t l = strlen(f);
        while (l > 1 && f[l-1] == '0' && f[l-2] != '.') f[--l] = 0;
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%sx%s", f, arrow);
    }
    snprintf(buf + n, sizeof buf - (size_t)n, "OFF");
    return buf;
}
// "3.00" or "3.00 / 4.00 / 0.50" -- the steps the hotkey walks through, in order.
const char* gs_steps_str(void) {
    static char buf[GS_STEPS_MAX * 8];
    int n = 0;
    for (int i = 0; i < g_gsStepN && n < (int)sizeof(buf) - 8; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%.2f", i ? " / " : "", g_gsSteps[i]);
    return buf;
}
void config_load(void) {
    g_badVkN = 0; g_cfgBad[0] = 0;
    // require_focus is the inverse key older ini files carry; global_hotkeys wins over it.
    int hkGlobal = -1, hkFocus = -1;
    char path[MAX_PATH];
    FILE* f = exe_sibling(path, sizeof(path), "ff13-srpractice.ini") ? fopen(path, "r") : NULL;
    if (f) {
        char line[512], section[64] = {0};
        while (fgets(line, sizeof(line), f)) {
            cfg_trim(line);
            if (!line[0] || line[0]==';' || line[0]=='#') continue;
            if (line[0]=='[') {
                char* end = strchr(line, ']');
                if (end) { *end = 0; strncpy(section, line+1, sizeof(section)-1);
                           section[sizeof(section)-1]=0; cfg_trim(section); }
                continue;
            }
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            // val is MAX_PATH wide because one setting is a path ([save] save_dir).
            char key[64], val[MAX_PATH];
            strncpy(key, line, sizeof(key)-1); key[sizeof(key)-1]=0;
            strncpy(val, eq+1, sizeof(val)-1); val[sizeof(val)-1]=0;
            cfg_trim(key); cfg_trim(val);
            // strip an inline comment (';' or '#' preceded by whitespace), then re-trim.
            for (char* c = val; *c; c++)
                if ((*c==';' || *c=='#') && c > val && (c[-1]==' ' || c[-1]=='\t')) { *c = 0; break; }
            cfg_trim(val);
            if (cfg_ieq(section, "hotkeys")) {
                int on = cfg_ieq(val, "1") || cfg_ieq(val, "true") || cfg_ieq(val, "on");
                if (cfg_ieq(key, "global_hotkeys")) {
                    hkGlobal = on;
                } else if (cfg_ieq(key, "require_focus")) {
                    hkFocus = on;
                } else {
                    for (int i = 0; i < (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0])); i++) {
                        if (!cfg_ieq(key, kHotkeys[i].name)) continue;
                        // Any key can be unbound: 0 is never a real binding, and hk_edge /
                        // hk_down on 0 cannot fire. off / none stay accepted for the ini.examples
                        // up to 1.3.0 that taught them. NOT "0": that is the digit key, a binding.
                        if (!val[0] || cfg_ieq(val, "off") || cfg_ieq(val, "none")) {
                            *kHotkeys[i].vk = 0;
                            break;
                        }
                        UINT vk = ff13_vk_parse(val);
                        if (vk) *kHotkeys[i].vk = vk;
                        else {
                            cfg_bad_vk(kHotkeys[i].name, kHotkeys[i].vk);
                            ff13loga("[FF13-SRP] config: unrecognized hotkey '%s=%s' -> keeping 0x%X\n",
                                     key, val, *kHotkeys[i].vk);
                        }
                        break;
                    }
                }
            } else if (cfg_ieq(section, "game_speed")) {
                if (cfg_ieq(key, "multiplier")) {
                    // One typo should not take the other steps with it: an out-of-range value is
                    // dropped and reported, and the rest of the list is kept.
                    int n = 0, bad = 0;
                    const char* p = val;
                    for (; *p && n < GS_STEPS_MAX; ) {
                        while (*p == ' ' || *p == '\t' || *p == ',') p++;
                        if (!*p) break;
                        char* end = NULL;
                        double fv = strtod(p, &end);
                        if (end == p) break;
                        p = end;
                        if (fv >= 0.1 && fv <= 10.0) g_gsSteps[n++] = fv;
                        else { bad = 1;
                               ff13logv("[FF13-SRP] config: multiplier %.2f is outside [0.1,10] -> "
                                        "that step is dropped\n", fv); }
                    }
                    while (*p == ' ' || *p == '\t' || *p == ',') p++;
                    if (*p) { bad = 1;
                              ff13logv("[FF13-SRP] config: multiplier takes %d steps at most -> \"%s\" "
                                       "is past the end and is not used\n", GS_STEPS_MAX, p); }
                    if (n) { g_gsStepN = n; g_gsFactor = g_gsSteps[0]; }
                    else { bad = 1;
                           ff13loga("[FF13-SRP] config: multiplier=%s has no usable value -> keeping "
                                    "%.2f\n", val, g_gsSteps[0]); }
                    if (bad) cfg_note_bad("multiplier");
                }
            } else if (cfg_ieq(section, "save")) {
                // Save anywhere (srp_save.c). Only these three keys are settings; savefix /
                // camera_fix / field_only / map_refresh / bak_cleanup are always on.
                if (cfg_ieq(key, "list_mark")) {
                    // The marker ends up in a UTF-16 line, so the UTF-8 ini text is widened here.
                    // A value that is not valid UTF-8 falls back to the local codepage.
                    g_listMark[0] = 0;
                    if (cfg_ieq(val, "off") || cfg_ieq(val, "none")) { /* leave the list alone */ }
                    else if (val[0]) {
                        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, val, -1,
                                                   g_listMark, (int)(sizeof(g_listMark)/sizeof(wchar_t)));
                        if (!n) n = MultiByteToWideChar(CP_ACP, 0, val, -1,
                                                       g_listMark, (int)(sizeof(g_listMark)/sizeof(wchar_t)));
                        if (!n) { g_listMark[0] = 0;
                            cfg_note_bad("list_mark");
                            ff13loga("[FF13-SRP] config: list_mark '%s' does not fit (max 8 chars) -> no marker\n", val); }
                        // A digit would be eaten by the renumbering: the mark lands right behind
                        // the save's number, and header_renumber (srp_save.c) takes the line's
                        // first run of digits as that number.
                        for (int i = 0; g_listMark[i]; i++)
                            if (g_listMark[i] >= L'0' && g_listMark[i] <= L'9') {
                                g_listMark[0] = 0;
                                cfg_note_bad("list_mark");
                                ff13loga("[FF13-SRP] config: list_mark '%s' has a digit in it -> no marker "
                                         "(digits would be read as part of the save number)\n", val);
                                break;
                            }
                    }
                } else if (cfg_ieq(key, "save_dir")) {
                    if (strlen(val) >= sizeof(g_saveDir)) {
                        cfg_note_bad("save_dir");
                        ff13loga("[FF13-SRP] config: save_dir too long (%u chars) -> truncated\n", (unsigned)strlen(val));
                    }
                    strncpy(g_saveDir, val, sizeof(g_saveDir)-1);
                    g_saveDir[sizeof(g_saveDir)-1] = 0;
                } else if (cfg_ieq(key, "keep_note")) {
                    if (cfg_ieq(val, "on") || cfg_ieq(val, "1") || cfg_ieq(val, "true")) g_keepNote = 1;
                    else if (!val[0] || cfg_ieq(val, "off") || cfg_ieq(val, "0") || cfg_ieq(val, "false"))
                        g_keepNote = 0;
                    else {
                        cfg_note_bad("keep_note");
                        ff13loga("[FF13-SRP] config: keep_note=%s is not on/off -> off\n", val);
                        g_keepNote = 0;
                    }
                }
            } else if (cfg_ieq(section, "log")) {
                if (cfg_ieq(key, "level")) {
                    if      (cfg_ieq(val, "quiet"))   g_logLevel = 0;
                    else if (cfg_ieq(val, "verbose")) g_logLevel = 2;
                    else {
                        g_logLevel = 1;
                        if (!cfg_ieq(val, "normal")) {
                            cfg_note_bad("log level");
                            ff13loga("[FF13-SRP] config: level=%s is not quiet/normal/verbose -> "
                                     "normal\n", val);
                        }
                    }
                }
            } else if (cfg_ieq(section, "battle_picker")) {
                if (cfg_ieq(key, "bgm_delay_ms")) {
                    // A matter of taste and of what the machine is doing; no measurement settles
                    // it. 0 restores the immediate hand-off this replaced.
                    int n = atoi(val);
                    if (n >= 0 && n <= BGM_HOLD_MAX_MS) g_bgmHoldMs = n;
                    else {
                        cfg_note_bad("bgm_delay_ms");
                        ff13loga("[FF13-SRP] config: battle_picker bgm_delay_ms=%s out of [0,%d] -> "
                                 "keeping %d\n", val, BGM_HOLD_MAX_MS, g_bgmHoldMs);
                    }
                }
            } else if (cfg_ieq(section, "overlay")) {
                if (cfg_ieq(key, "top_px")) {
                    char* end;
                    long n = strtol(val, &end, 10);
                    if (end != val && !*end && n >= 0 && n <= OVL_TOP_MAX) g_ovlTopPx = (int)n;
                    else {
                        cfg_note_bad("top_px");
                        ff13loga("[FF13-SRP] config: overlay top_px=%s out of [0,%d] -> keeping %d\n",
                                 val, OVL_TOP_MAX, g_ovlTopPx);
                    }
                }
            }
        }
        fclose(f);
    }
    if (hkGlobal >= 0) {
        g_hkGlobal = hkGlobal;
        if (hkFocus >= 0)
            ff13loga("[FF13-SRP] config: require_focus is ignored -- global_hotkeys=%d is set\n", hkGlobal);
    } else if (hkFocus >= 0) {
        g_hkGlobal = !hkFocus;
        ff13loga("[FF13-SRP] config: require_focus=%d read as global_hotkeys=%d\n", hkFocus, g_hkGlobal);
    }
    // menu_ok is the menus' only confirm, so it is never left empty, nor on the tidy key's PHYSICAL
    // key (by hk_key: a modifier does not tell the two apart once it is let up first), whose press
    // closes the menu.
    if (!g_menuOkVk || (g_ssVk && hk_key(g_menuOkVk) == hk_key(g_ssVk))) {
        // NOT cfg_bad_vk: that channel means "did not parse, the DEFAULT is in force", and neither
        // half is true here. Written at every log level on purpose.
        UINT was = g_menuOkVk;
        g_menuOkVk = (hk_key(g_ssVk) == VK_RETURN) ? VK_SPACE : VK_RETURN;
        char l[32];
        vk_label(g_menuOkVk, l, sizeof l);
        ff13loga("[FF13-SRP] config: menu_ok %s -> %s (the menus have no other confirm)\n",
                 was ? "is the tidy key itself" : "is empty", l);
    }
    for (int i = 0; i < hk_binding_count(); i++)
        for (int j = i + 1; j < hk_binding_count(); j++) {
            UINT a = *kHotkeys[i].vk;
            if (!a || a != *kHotkeys[j].vk) continue;
            char l[32];
            vk_label(a, l, sizeof l);
            ff13loga("[FF13-SRP] config: %s and %s are both bound to %s\n", kHotkeys[i].name,
                     kHotkeys[j].name, l);
        }
    char mark[32] = {0};   // back to narrow for the log line
    WideCharToMultiByte(CP_UTF8, 0, g_listMark, -1, mark, sizeof(mark), NULL, NULL);
    ff13loga("[FF13-SRP] config: help=0x%X kill_mode=0x%X enemy_suppress=0x%X game_speed=0x%X "
             "warp=0x%X battle_picker=0x%X multiplier=%s log=%d save=0x%X camera_unstick=0x%X "
             "list_mark='%s' saveDir='%s' keep_note=%d\n",
             g_helpVk, g_armVk, g_supVk, g_gsVk, g_warpVk, g_bsVk, gs_steps_str(), g_logLevel,
             g_saveVk, g_camUnstickVk, mark, g_saveDir, g_keepNote);
}

// ---- writing one setting back ----------------------------------------------------------------
static int cfg_write_fail(const char* what) {
    ff13loga("[FF13-SRP] config: cannot write %s\n", what);
    flash_say_c(ui_text(T_SET_INI_FAIL), OVL_MENU_OFF);
    return 0;
}

int config_write(const char* section, const char* key, const char* value) {
    char path[MAX_PATH], tmp[MAX_PATH], err[MAX_PATH + 64];
    if (!exe_sibling(path, sizeof path, "ff13-srpractice.ini")
        || !exe_sibling(tmp, sizeof tmp, "ff13-srpractice.ini.tmp"))
        return cfg_write_fail("ff13-srpractice.ini (the path is too long)");
    ff13_ini_notes nt;
    // The second line says the first in Japanese, UTF-8 escaped: only ovl_text.h holds Japanese.
    int ok = ff13_ini_write(path, tmp, section, key, value,
        "; Written by FF13-SpeedrunPractice's settings screen; the README explains every key.\n"
        "; \xE3\x83\x9B\xE3\x83\x83\xE3\x83\x88\xE3\x82\xAD\xE3\x83\xBC\xE3\x81\xA8"
        "\xE8\xA8\xAD\xE5\xAE\x9A\xE3\x81\xAE\xE7\x94\xBB\xE9\x9D\xA2\xE3\x81\x8B"
        "\xE3\x82\x89\xE6\x9B\xB8\xE3\x81\x8B\xE3\x82\x8C\xE3\x81\x9F\xE3\x83\x95"
        "\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB\xE3\x80\x82\xE5\x85\xA8\xE3\x82\xAD"
        "\xE3\x83\xBC\xE3\x81\xAE\xE8\xAA\xAC\xE6\x98\x8E\xE3\x81\xAF"
        "README\xE3\x81\xAB\xE3\x81\x82\xE3\x82\x8B\xE3\x80\x82\n",
        &nt, err, sizeof err);
    if (nt.commentDropped)
        ff13logv("[FF13-SRP] config: %s.%s's inline comment dropped with its value\n", section, key);
    return ok ? 1 : cfg_write_fail(err);
}

// ---- runtime toggle persistence (sidecar ff13-srpractice_state.dat) ----
// Written on every toggle; loaded once at startup AFTER config_load, so a saved runtime state
// overrides the ini defaults. Missing file = keep whatever config_load set.
void state_save(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice_state.dat")) return;
    FILE* f = fopen(path, "w"); if (!f) return;
    // Format is fixed by backward compatibility with sidecars older builds also read: "onehit"
    // stays the one-hit-kill bit (a 2 there would arm them without instant-win meaning anything to
    // them; the extra strength rides in "instantwin", which they skip), and "gamespeed" carries the
    // STEP -- 0 = off, n = the nth multiplier -- so an older sidecar's 1 restores as the first step.
    fprintf(f, "suppress %d\ngamespeed %d\nonehit %d\ninstantwin %d\nbattle_overlay %d\n"
               "filter %d\norder %d\n",
            g_suppressEnabled ? 1 : 0, g_gsStep + 1, g_armed ? 1 : 0,
            g_iwArmed ? 1 : 0, g_battleOverlay ? 1 : 0, g_filterChapter, g_sortOrder);
    fclose(f);
}
void state_load(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice_state.dat")) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    char key[32]; int val;
    // "sort" is the retired row: 0 asc / 1 desc / 2 by number, applied by re-stamping files. It is
    // read only to seed "order" once (below), and state_save never writes it again.
    int sawOrder = 0, oldSort = -1;
    // Every line is "<key> <int>". A key this build does not know is consumed by the same fscanf
    // and claimed by no branch, so a sidecar carrying a retired setting still parses.
    while (fscanf(f, "%31s %d", key, &val) == 2) {
        int on = (val != 0);
        if      (cfg_ieq(key, "suppress"))       g_suppressEnabled = on;
        else if (cfg_ieq(key, "gamespeed"))      gs_apply(val - 1);
        else if (cfg_ieq(key, "onehit"))         g_armed = on;
        else if (cfg_ieq(key, "instantwin"))     g_iwArmed = on;
        else if (cfg_ieq(key, "battle_overlay")) g_battleOverlay = on;
        // Recorded here, put into effect by filter_restore once the save headers have been
        // scanned and the list hook can be installed.
        else if (cfg_ieq(key, "filter"))         g_filterChapter = (val > 0 && val < 256) ? val : 0;
        else if (cfg_ieq(key, "order")) {
            g_sortOrder = (val >= 0 && val < SS_ORDER_MODES) ? val : 0;
            sawOrder = 1;
        }
        else if (cfg_ieq(key, "sort"))           oldSort = val;
    }
    fclose(f);
    // The retired sorter wrote its mode whether or not it stamped anything, so the real-date
    // sidecar is what says a sort had been applied; without it the list was the game's own order
    // and stays that way.
    if (!sawOrder) {
        g_sortOrder = (oldSort >= 0 && oldSort <= 2 && dates_sidecar_has_rows()) ? oldSort + 1 : 0;
        if (g_sortOrder)
            ff13loga("[FF13-SRP] state: the old sort %d carries over as list order %d\n",
                     oldSort, g_sortOrder);
    }
    // Rebuilt from the two bits rather than stored, so a sidecar from a build that never knew about
    // instant-win restores as plain one-hit-kill instead of as nothing.
    kill_mode_apply(g_iwArmed ? KILL_INSTANT : g_armed ? KILL_ONEHIT : KILL_OFF, 0);
    ff13loga("[FF13-SRP] state: restored suppress=%d gamespeed=%d kill=%s battle_overlay=%d "
             "filter=%d order=%d%s\n",
             g_suppressEnabled, g_gsEnabled, kill_mode_name_en(g_killMode), g_battleOverlay,
             g_filterChapter, g_sortOrder,
             g_killMode != KILL_OFF ? "  (WARNING: this is carried over from a previous session)" : "");
}

// Zone id + field-state gate.
#define OFF_ZONE_ROOT 0x242B230
#define OFF_ZONE_ID   0x1D68
uint32_t field_zone_id(void) {
    if (IsBadReadPtr((void*)(g_base + OFF_ZONE_ROOT), 4)) return (uint32_t)-1;
    uint32_t root = *(uint32_t*)(g_base + OFF_ZONE_ROOT);
    if (!root || IsBadReadPtr((void*)(uintptr_t)(root + OFF_ZONE_ID), 4)) return (uint32_t)-1;
    return *(uint32_t*)(uintptr_t)(root + OFF_ZONE_ID);
}
// ZoneStateMachine state index; 3 = FIELD_IDLE = normal explorable field, the only safe state to
// record or warp in. -1 if unreadable.
int field_zone_state(void) {
    uintptr_t p = g_base + 0x2411190;
    if (IsBadReadPtr((void*)p, 4)) return -1;
    uint32_t facade = *(uint32_t*)p;
    if (!facade || IsBadReadPtr((void*)(uintptr_t)(facade + 0x14), 4)) return -1;
    uint32_t zsm = *(uint32_t*)(uintptr_t)(facade + 0x14);
    if (!zsm || IsBadReadPtr((void*)(uintptr_t)(zsm + 0x50), 4)) return -1;
    uint32_t gf = *(uint32_t*)(uintptr_t)(zsm + 0x50);
    if (!gf || IsBadReadPtr((void*)(uintptr_t)(gf + 4), 4)) return -1;
    return (int)*(uint32_t*)(uintptr_t)(gf + 4);
}
int wait_for_bytes(uintptr_t addr, const uint8_t* expect, int len, int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 100) {
        if (memcmp((const void*)addr, expect, (size_t)len) == 0) return t;
        Sleep(100);
    }
    return -1;
}

#ifdef FF13_DIAG
// Where the address space goes, for telling the large-address-aware (4GB-patched) exe from the
// untouched one: committed and reserved bytes below and above 2 GB (by region base), the largest
// free run, and the highest pointer the AI hooks were handed.
static volatile uint32_t g_ptrMax, g_ptrHi;
void diag_ptr_seen(uint32_t p) {
    if (p > g_ptrMax) g_ptrMax = p;
    if (p >= 0x80000000u) g_ptrHi++;
}
void diag_vm_census(const char* when) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t top = (uintptr_t)si.lpMaximumApplicationAddress;
    unsigned long long comLo = 0, comHi = 0, resLo = 0, resHi = 0, freeMax = 0;
    uintptr_t freeAt = 0, hiFirst = 0;
    MEMORY_BASIC_INFORMATION m;
    while (a < top && VirtualQuery((void*)a, &m, sizeof m) == sizeof m) {
        uintptr_t b = (uintptr_t)m.BaseAddress, n = m.RegionSize;
        int hi = b >= 0x80000000u;
        if (m.State == MEM_COMMIT) {
            if (hi) { comHi += n; if (!hiFirst) hiFirst = b; } else comLo += n;
        } else if (m.State == MEM_RESERVE) {
            if (hi) resHi += n; else resLo += n;
        } else if (m.State == MEM_FREE && n > freeMax) { freeMax = n; freeAt = b; }
        if (b + n <= a) break;
        a = b + n;
    }
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    static unsigned long long lowAvail = ~0ull, lowFree = ~0ull;
    if (ms.ullAvailVirtual < lowAvail) lowAvail = ms.ullAvailVirtual;
    if (freeMax < lowFree) lowFree = freeMax;
    uint32_t amin = 0xFFFFFFFFu, amax = 0;
    uint32_t* tbl = actor_table();
    if (tbl) for (int i = 0; i < ACTOR_SLOTS; i++) if (tbl[i]) {
        if (tbl[i] < amin) amin = tbl[i];
        if (tbl[i] > amax) amax = tbl[i];
    }
    ff13logv("[FF13-SRP] vm census (%s): top %08X, OS total %llu MB avail %llu MB; committed %llu MB "
             "below 2GB + %llu MB above (first %08X); reserved %llu + %llu MB; largest free %llu MB @%08X; "
             "actors %08X..%08X; AI-hook pointers max %08X, %u at/above 2GB; lowest so far: avail %llu MB, "
             "largest free %llu MB\n",
             when, (unsigned)top, ms.ullTotalVirtual >> 20, ms.ullAvailVirtual >> 20,
             comLo >> 20, comHi >> 20, (unsigned)hiFirst, resLo >> 20, resHi >> 20,
             freeMax >> 20, (unsigned)freeAt, amax ? amin : 0, amax,
             (unsigned)g_ptrMax, (unsigned)g_ptrHi, lowAvail >> 20, lowFree >> 20);
}
#endif
