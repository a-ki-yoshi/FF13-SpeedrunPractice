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
// Features: ABOUT.md (developer-facing) / README.md (user-facing). Why this ships as a dinput8
// proxy and which slots are usable: CLAUDE.md. What -DFF13_DIAG leaves out: build.sh.

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
// Hotkeys only while the game is the focused window. GetAsyncKeyState reads the keyboard, not the
// window, so without this every key here fires from inside a browser. ini: [hotkeys] require_focus
// -- a setting rather than a rule because a wrong focus test takes EVERY hotkey with it, and 0 is
// the only way back from inside the game.
volatile int g_hkFocusOnly = 1;
// ---- what the kill hotkey is set to -------------------------------------------------------
// User-facing cycle order, strongest first: OFF -> INSTANT WIN (+ one-hit-kill) -> ONE-HIT KILL ->
// OFF. The two are not redundant: instant-win only acts while a fight is being BUILT, so
// one-hit-kill is what covers the fight you are already in, and any fight the gate finds nothing
// to suppress in.
volatile int g_killMode = KILL_OFF;
volatile unsigned char g_battleOverlay = 1;  // battle timer + enemy HP shown in battle (default ON)
int  g_logLevel = 1;           // 0 quiet / 1 normal / 2 verbose

// ---- game-speed state (the game clock 0x420A80 wrapper reads these) ----
// [game_speed] multiplier is a LIST; the hotkey steps through it and then off, so a list of one
// behaves as a plain toggle. Which step is not an ini setting -- the sidecar restores it.
#define GS_STEPS_MAX 8
static double g_gsSteps[GS_STEPS_MAX] = { 3.0 };
int    g_gsStepN = 1;
volatile int g_gsStep = -1;               // -1 = off, else an index into g_gsSteps
volatile unsigned char g_gsEnabled = 0;   // g_gsStep >= 0, kept as its own byte for gs_clock
double g_gsFactor = 3.0;                  // ...and the multiplier of that step, likewise
// One place decides both, so the clock hook can never read a factor from a step it is not on.
void gs_apply(int step) {
    if (step >= g_gsStepN) step = -1;
    g_gsStep = step;
    g_gsEnabled = (step >= 0);
    if (step >= 0) g_gsFactor = g_gsSteps[step];
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

// ---- what language the mod speaks -------------------------------------------------------------
// The game resolves its text language once at startup into a .data global, long after this DLL is
// loaded -- so the value is read late and re-read until it answers (-1 = not yet). Policy:
// Japanese for a Japanese game, English for every other one.
#define RVA_LANGUAGE 0x22A19B8      // VA 0x026A19B8, int
#define OVL_LANG_AUTO 0
#define OVL_LANG_JP   1
#define OVL_LANG_EN   2
static volatile int g_uiLang   = OVL_LANG_AUTO;   // [overlay] language
static volatile int g_langSeen = -1;              // what the game last answered, -1 = not yet

// 1 = English.
int ui_english(void) {
    if (g_uiLang != OVL_LANG_AUTO) return g_uiLang == OVL_LANG_EN;
    if (!g_base) return 0;
    int idx = *(volatile int*)(uintptr_t)(g_base + RVA_LANGUAGE);
    if (idx < 0 || idx > 10) return g_langSeen > 0;     // not resolved yet -- keep the last answer
    if (idx != g_langSeen) {
        g_langSeen = idx;
        static const char* const kLang[11] =
            { "jp", "us", "uk", "it", "gr", "fr", "sp", "ru", "kr", "ck", "ch" };
        ff13loga("[FF13-SRP] the game is running in \"%s\" -- overlay and fight names in %s "
                 "([overlay] language = jp or en to override)\n",
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
    if (ui_english() || !g_fontJp) return kOvlEn[m];
    const char* jp = kOvlJp[m];
    return (jp && jp[0]) ? jp : kOvlEn[m];
}


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

// auto | jp | en. `where` names the ini line, so a typo is reported against that line.
static void cfg_language(const char* val, const char* where) {
    g_uiLang = cfg_ieq(val, "jp") || cfg_ieq(val, "ja") ? OVL_LANG_JP
             : cfg_ieq(val, "en") || cfg_ieq(val, "us") ? OVL_LANG_EN
             : OVL_LANG_AUTO;
    if (g_uiLang == OVL_LANG_AUTO && val[0] && !cfg_ieq(val, "auto")) {
        cfg_note_bad("language");
        ff13loga("[FF13-SRP] config: %s=%s is not one of auto/jp/en -> following the game\n",
                 where, val);
    }
    else
        ff13loga("[FF13-SRP] config: %s=%s -- the overlay's own text and the picker's fight names "
                 "both follow it\n", where, g_uiLang == OVL_LANG_JP ? "jp"
                                          : g_uiLang == OVL_LANG_EN ? "en" : "auto");
}
// Parses a binding into a VK plus modifier bits (the modifiers ride in the bits above the key
// code): a function-key name, "0xNN" hex, or a single letter or digit, each optionally behind
// Ctrl+ / Shift+ / Alt+ prefixes in any order and case; 0 = invalid.
// Fn cannot be supported -- the keyboard handles it and it never becomes a key code the OS sees.
static int cfg_ieqn(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}
static UINT cfg_parse_vk(const char* v) {
    UINT mods = 0;
    for (;;) {                                   // eat "<mod>+" prefixes, in whatever order
        while (*v == ' ' || *v == '\t') v++;
        const char* plus = strchr(v, '+');
        if (!plus) break;
        size_t len = (size_t)(plus - v);
        while (len && (v[len-1] == ' ' || v[len-1] == '\t')) len--;
        if      (len == 4 && cfg_ieqn(v, "ctrl", 4))    mods |= HK_CTRL;
        else if (len == 7 && cfg_ieqn(v, "control", 7)) mods |= HK_CTRL;
        else if (len == 5 && cfg_ieqn(v, "shift", 5))   mods |= HK_SHIFT;
        else if (len == 3 && cfg_ieqn(v, "alt", 3))     mods |= HK_ALT;
        else return 0;                           // an unknown prefix is a typo, not a key
        v = plus + 1;
    }
    while (*v == ' ' || *v == '\t') v++;
    size_t klen = strlen(v);                     // the key itself, without trailing blanks
    while (klen && (v[klen-1] == ' ' || v[klen-1] == '\t')) klen--;
    UINT vk = 0;
    if ((v[0]=='F'||v[0]=='f') && v[1]>='0' && v[1]<='9') {
        int n = atoi(v + 1);
        if (n >= 1 && n <= 24) vk = (UINT)(VK_F1 + (n - 1));
    } else if (v[0]=='0' && (v[1]=='x'||v[1]=='X')) {
        unsigned x = (unsigned)strtoul(v, NULL, 16);
        if (x && x <= 0xFF) vk = (UINT)x;
    } else if (klen == 1 && ((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z'))) {
        vk = (UINT)(v[0] >= 'a' ? v[0] - 'a' + 'A' : v[0]);   // VK for a letter IS its uppercase
    } else if (klen == 1 && v[0] >= '0' && v[0] <= '9') {
        vk = (UINT)v[0];                                      // ...and for a digit, its ASCII
    }
    return vk ? (vk | mods) : 0;   // unrecognized -> caller keeps the default and warns
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
};
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
                if (cfg_ieq(key, "require_focus")) {
                    g_hkFocusOnly = (cfg_ieq(val, "1") || cfg_ieq(val, "true") || cfg_ieq(val, "on")) ? 1 : 0;
                } else {
                    for (int i = 0; i < (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0])); i++) {
                        if (!cfg_ieq(key, kHotkeys[i].name)) continue;
                        // Any key can be turned off: 0 is never a real binding, and hk_edge /
                        // hk_down on 0 cannot fire, so an unbound key simply stops existing.
                        if (cfg_ieq(val, "off") || cfg_ieq(val, "none") || cfg_ieq(val, "0")) {
                            *kHotkeys[i].vk = 0;
                            break;
                        }
                        UINT vk = cfg_parse_vk(val);
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
                // Save anywhere (srp_save.c). Only these two keys are settings; savefix /
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
                    }
                } else if (cfg_ieq(key, "save_dir")) {
                    if (strlen(val) >= sizeof(g_saveDir)) {
                        cfg_note_bad("save_dir");
                        ff13loga("[FF13-SRP] config: save_dir too long (%u chars) -> truncated\n", (unsigned)strlen(val));
                    }
                    strncpy(g_saveDir, val, sizeof(g_saveDir)-1);
                    g_saveDir[sizeof(g_saveDir)-1] = 0;
                }
            } else if (cfg_ieq(section, "overlay")) {
                if (cfg_ieq(key, "language")) cfg_language(val, "overlay language");
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
            }
        }
        fclose(f);
    }
    char mark[32] = {0};   // back to narrow for the log line
    WideCharToMultiByte(CP_UTF8, 0, g_listMark, -1, mark, sizeof(mark), NULL, NULL);
    ff13loga("[FF13-SRP] config: help=0x%X kill_mode=0x%X enemy_suppress=0x%X game_speed=0x%X "
             "warp=0x%X battle_picker=0x%X multiplier=%s log=%d save=0x%X camera_unstick=0x%X "
             "list_mark='%s' saveDir='%s'\n",
             g_helpVk, g_armVk, g_supVk, g_gsVk, g_warpVk, g_bsVk, gs_steps_str(), g_logLevel,
             g_saveVk, g_camUnstickVk, mark, g_saveDir);
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
    fprintf(f, "suppress %d\ngamespeed %d\nonehit %d\ninstantwin %d\nbattle_overlay %d\n",
            g_suppressEnabled ? 1 : 0, g_gsStep + 1, g_armed ? 1 : 0,
            g_iwArmed ? 1 : 0, g_battleOverlay ? 1 : 0);
    fclose(f);
}
void state_load(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof(path), "ff13-srpractice_state.dat")) return;
    FILE* f = fopen(path, "r"); if (!f) return;
    char key[32]; int val;
    // Every line is "<key> <int>". A key this build does not know is consumed by the same fscanf
    // and claimed by no branch, so a sidecar carrying a retired setting still parses.
    while (fscanf(f, "%31s %d", key, &val) == 2) {
        int on = (val != 0);
        if      (cfg_ieq(key, "suppress"))       g_suppressEnabled = on;
        else if (cfg_ieq(key, "gamespeed"))      gs_apply(val - 1);
        else if (cfg_ieq(key, "onehit"))         g_armed = on;
        else if (cfg_ieq(key, "instantwin"))     g_iwArmed = on;
        else if (cfg_ieq(key, "battle_overlay")) g_battleOverlay = on;
    }
    fclose(f);
    // Rebuilt from the two bits rather than stored, so a sidecar from a build that never knew about
    // instant-win restores as plain one-hit-kill instead of as nothing.
    kill_mode_apply(g_iwArmed ? KILL_INSTANT : g_armed ? KILL_ONEHIT : KILL_OFF, 0);
    ff13loga("[FF13-SRP] state: restored suppress=%d gamespeed=%d kill=%s battle_overlay=%d%s\n",
             g_suppressEnabled, g_gsEnabled, kill_mode_name_en(g_killMode), g_battleOverlay,
             g_killMode != KILL_OFF ? "  (WARNING: this is carried over from a previous session)" : "");
}

// Zone id + field-state gate (duplicated from AnywhereSave on purpose; not shared).
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
