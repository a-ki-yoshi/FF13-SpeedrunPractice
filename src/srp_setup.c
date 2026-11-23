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

// The hotkeys-and-settings screen: every ini setting, changed in place and written back.
//
// Threads: everything but setup_visible/setup_draw runs on the hotkey worker; those two run on the
// render thread. Shared state is volatile with one writer each, except the lapse guard's close (as
// in the tidy menus' guard in overlay_draw).
#include "srp_internal.h"

// ---- the rows ---------------------------------------------------------------------------------
enum {
    R_H_KEYS, R_K_HELP, R_K_SPEED, R_K_KILL, R_K_WARP, R_K_SUP, R_K_SAVE, R_K_CAM, R_K_TIDY,
    R_K_BATTLE, R_K_PICKER, R_KEYS,
    R_H_SET, R_FOCUS, R_MULT, R_BGM, R_MARK, R_KEEPNOTE, R_SAVEDIR, R_TOP, R_LOG, R_N
};
enum { K_HEAD, K_KEY, K_KEYS, K_BOOL, K_MULT, K_INT, K_MARK, K_INFO, K_TOP, K_LOG };
static int row_kind(int r) {
    if (r == R_H_KEYS || r == R_H_SET) return K_HEAD;
    if (r >= R_K_HELP && r <= R_K_PICKER) return K_KEY;
    switch (r) {
    case R_KEYS:    return K_KEYS;
    case R_FOCUS:   return K_BOOL;
    case R_KEEPNOTE: return K_BOOL;
    case R_MULT:    return K_MULT;
    case R_BGM:     return K_INT;
    case R_MARK:    return K_MARK;
    case R_SAVEDIR: return K_INFO;
    case R_TOP:     return K_TOP;
    default:        return K_LOG;
    }
}
static const char* const kKeyRowName[R_KEYS] = {
    NULL, "help", "game_speed", "kill_mode", "warp", "enemy_suppress", "save", "camera_unstick",
    "save_tidy", "battle_overlay", "battle_picker",
};
#define SUB_N 8
static const char* const kSubName[SUB_N] = {
    "picker_up", "picker_down", "picker_left", "picker_right", "cancel", "menu_ok", "list_prev",
    "list_next",
};
// T_COUNT = no name: list_next rides on list_prev's.
static const ovl_msg kSubShort[SUB_N] = {
    T_SET_SUB_UP, T_SET_SUB_DOWN, T_SET_SUB_LEFT, T_SET_SUB_RIGHT, T_SET_SUB_CLOSE, T_SET_SUB_OK,
    T_SET_SUB_LIST, T_COUNT,
};

// What each binding is called when another one wants its key.
static const struct { const char* name; ovl_msg label; } kFn[] = {
    { "game_speed", T_SET_L_SPEED },        { "kill_mode", T_SET_L_KILL },
    { "warp", T_SET_L_WARP },               { "enemy_suppress", T_SET_L_SUPPRESS },
    { "save", T_SET_L_SAVE },               { "camera_unstick", T_SET_L_CAMERA },
    { "save_tidy", T_SET_L_TIDY },          { "battle_overlay", T_SET_L_BATTLE },
    { "battle_picker", T_SET_L_PICKER },    { "help", T_SET_L_HELP },
    { "picker_up", T_SET_FN_UP },           { "picker_down", T_SET_FN_DOWN },
    { "picker_left", T_SET_FN_LEFT },       { "picker_right", T_SET_FN_RIGHT },
    { "cancel", T_SET_FN_CLOSE },           { "menu_ok", T_SET_FN_OK },
    { "list_prev", T_SET_FN_LPREV },        { "list_next", T_SET_FN_LNEXT },
};
static const char* fn_label(const char* name) {
    for (int i = 0; i < (int)(sizeof kFn / sizeof kFn[0]); i++)
        if (!strcmp(kFn[i].name, name)) return ui_text(kFn[i].label);
    return name;
}

static int bind_idx(const char* name) {
    for (int i = 0; i < hk_binding_count(); i++)
        if (!strcmp(hk_binding_name(i), name)) return i;
    return -1;
}
// The binding that already holds b, other than `self`; -1 = none. Exact match: vk AND modifiers.
static int bind_owner(UINT b, int self) {
    if (!b) return -1;
    for (int j = 0; j < hk_binding_count(); j++)
        if (j != self && *hk_binding_vk(j) == b) return j;
    return -1;
}
// menu_ok on the tidy key's PHYSICAL key is the pair config_load moves off (srp_core.c).
static int ok_tidy_clash(const char* name, UINT b) {
    if (!b) return 0;
    if (!strcmp(name, "menu_ok"))   return g_ssVk && hk_key(g_ssVk) == hk_key(b);
    if (!strcmp(name, "save_tidy")) return g_menuOkVk && hk_key(g_menuOkVk) == hk_key(b);
    return 0;
}
static int bind_dup(int bi) {
    if (bi < 0) return 0;
    UINT v = *hk_binding_vk(bi);
    return bind_owner(v, bi) >= 0 || ok_tidy_clash(hk_binding_name(bi), v);
}

// ---- state ------------------------------------------------------------------------------------
static volatile int  g_open   = 0;
static volatile int  g_sel    = R_K_SPEED;
static volatile int  g_sub    = 0;          // the key row's item under the sub-cursor
static volatile int  g_msub   = 0;          // the multiplier candidate under the sub-cursor
static volatile int  g_cap    = -1;         // the binding being captured, -1 = none
static volatile UINT g_settle = 0;          // the key that ended the last capture
static unsigned char g_prev[256];           // worker only: the capture's key states last tick
static wchar_t g_markOwn[9];                // worker only: the ini's own mark, when not a preset

int setup_is_open(void) { return g_open; }
int setup_capturing(void) { return g_open && g_cap >= 0; }

// ---- the mark row -----------------------------------------------------------------------------
static const wchar_t* const kMarks[] = {
    L"", L"*", L"\uFF0A", L"\u2605", L"\u2606", L"\u25CF", L"\u25C6", L"\u266A",
};
#define MARK_PRESETS ((int)(sizeof kMarks / sizeof kMarks[0]))
static int mark_preset(const wchar_t* m) {
    for (int i = 0; i < MARK_PRESETS; i++) if (!wcscmp(kMarks[i], m)) return i;
    return -1;
}
static void mark_note_own(void) {
    wchar_t cur[9];
    memcpy(cur, g_listMark, sizeof cur);
    cur[8] = 0;
    if (mark_preset(cur) < 0) memcpy(g_markOwn, cur, sizeof cur);
}

void setup_open(void) {
    if (g_open) return;
    g_cap = -1;
    g_settle = 0;
    if (row_kind(g_sel) == K_HEAD) g_sel = R_K_SPEED;
    mark_note_own();
    di_gate_arm();                   // before the flag: the render thread's lapse guard reads both
    g_open = 1;
    ff13log("[FF13-SRP] settings: opened\n");
}
void setup_close(void) {
    if (!g_open) return;
    g_open = 0;
    g_cap = -1;
    ff13log("[FF13-SRP] settings: closed\n");
}

void setup_move(int dy) {
    if (!g_open || g_cap >= 0) return;
    int s = g_sel;
    do {
        s += dy > 0 ? 1 : -1;
        if (s < 0) s = R_N - 1;
        if (s >= R_N) s = 0;
    } while (row_kind(s) == K_HEAD);
    g_sel = s;
}

// ---- writing a setting ------------------------------------------------------------------------
static void setting_write(const char* sec, const char* key, const char* val) {
    int ok = config_write(sec, key, val);
    ff13log("[FF13-SRP] settings: %s.%s = %s (%s)\n", sec, key, val[0] ? val : "(empty)",
            ok ? "ini written" : "ini NOT written");
}

// ---- the multiplier list ----------------------------------------------------------------------
static const double kCand[] = { 0.5, 1.5, 2, 3, 4, 5, 6, 8, 10 };
#define CAND_MAX ((int)(sizeof kCand / sizeof kCand[0]) + GS_STEPS_MAX)
static int near_eq(double a, double b) { double d = a - b; return d < 0.005 && d > -0.005; }
static void sort_asc(double* v, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
}
static int mult_cands(double* out) {
    int n = 0;
    for (int i = 0; i < (int)(sizeof kCand / sizeof kCand[0]); i++) out[n++] = kCand[i];
    int sn = g_gsStepN;
    if (sn > GS_STEPS_MAX) sn = GS_STEPS_MAX;
    for (int i = 0; i < sn; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++) if (near_eq(out[j], g_gsSteps[i])) dup = 1;
        if (!dup) out[n++] = g_gsSteps[i];
    }
    sort_asc(out, n);
    return n;
}
static int mult_has(double v) {
    for (int i = 0; i < g_gsStepN && i < GS_STEPS_MAX; i++) if (near_eq(g_gsSteps[i], v)) return 1;
    return 0;
}
// "%.2f" with the trailing zeros and a bare point dropped: 3.00 -> 3, 1.50 -> 1.5.
static void fmt_factor(double v, char* out, size_t n) {
    snprintf(out, n, "%.2f", v);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '0') out[--l] = 0;
    if (l > 1 && out[l - 1] == '.') out[--l] = 0;
}

static void mult_toggle(void) {
    double c[CAND_MAX];
    int n = mult_cands(c);
    int at = g_msub;
    if (at < 0 || at >= n) at = n - 1;
    double v = c[at];
    double list[GS_STEPS_MAX + 1];
    int m = 0, had = 0;
    for (int i = 0; i < g_gsStepN && i < GS_STEPS_MAX; i++) {
        if (near_eq(g_gsSteps[i], v)) had = 1;
        else list[m++] = g_gsSteps[i];
    }
    if (had && !m) { flash_say_c(ui_text(T_SET_MULT_MIN), OVL_MENU_OFF); return; }
    if (!had) {
        if (m >= GS_STEPS_MAX) { flash_say_c(ui_text(T_SET_MULT_MAX), OVL_MENU_OFF); return; }
        list[m++] = v;
    }
    sort_asc(list, m);
    gs_steps_set(list, m);
    cfg_bad_clear("multiplier");
    // The sub-cursor stays on the value; a factor of the player's own that left the list also left
    // the candidates, so the cursor takes the next one up.
    n = mult_cands(c);
    at = n - 1;
    for (int i = 0; i < n; i++) if (c[i] >= v - 0.005) { at = i; break; }
    g_msub = at;
    char val[GS_STEPS_MAX * 12], f[16];
    int o = 0;
    val[0] = 0;
    for (int i = 0; i < m; i++) {
        fmt_factor(list[i], f, sizeof f);
        o += snprintf(val + o, sizeof val - (size_t)o, "%s%s", i ? ", " : "", f);
    }
    setting_write("game_speed", "multiplier", val);
}

// ---- left/right and confirm -------------------------------------------------------------------
static const char* const kLogName[3] = { "quiet", "normal", "verbose" };

static void bool_toggle(int r) {
    if (r == R_KEEPNOTE) {
        g_keepNote = !g_keepNote;
        cfg_bad_clear("keep_note");
        setting_write("save", "keep_note", g_keepNote ? "on" : "off");
        return;
    }
    g_hkGlobal = !g_hkGlobal;
    setting_write("hotkeys", "global_hotkeys", g_hkGlobal ? "1" : "0");
}

static void mark_cycle(int dir) {
    const wchar_t* list[MARK_PRESETS + 1];
    int n = 0;
    for (int i = 0; i < MARK_PRESETS; i++) list[n++] = kMarks[i];
    if (g_markOwn[0]) list[n++] = g_markOwn;
    wchar_t cur[9];
    memcpy(cur, g_listMark, sizeof cur);
    cur[8] = 0;
    int at = 0;
    for (int i = 0; i < n; i++) if (!wcscmp(list[i], cur)) { at = i; break; }
    at = (at + (dir > 0 ? 1 : n - 1)) % n;
    const wchar_t* m = list[at];
    // [0] last: the render thread draws the mark row from the same buffer.
    g_listMark[0] = 0;
    size_t len = wcslen(m);
    for (size_t i = 1; i <= len && i < 9; i++) g_listMark[i] = m[i];
    g_listMark[0] = m[0];
    cfg_bad_clear("list_mark");
    char val[40] = "";
    if (m[0]) WideCharToMultiByte(CP_UTF8, 0, m, -1, val, sizeof val, NULL, NULL);
    setting_write("save", "list_mark", val);
}

void setup_cycle(int dir) {
    if (!g_open || g_cap >= 0) return;
    int r = g_sel;
    switch (row_kind(r)) {
    case K_KEYS:
        g_sub = (g_sub + (dir > 0 ? 1 : SUB_N - 1)) % SUB_N;
        break;
    case K_MULT: {
        double c[CAND_MAX];
        int n = mult_cands(c);
        int s = g_msub + (dir > 0 ? 1 : -1);
        g_msub = s < 0 ? n - 1 : s >= n ? 0 : s;
        break;
    }
    case K_BOOL:
        bool_toggle(r);
        break;
    case K_INT: {
        int v = g_bgmHoldMs + (dir > 0 ? 100 : -100);
        if (v < 0) v = 0;
        if (v > BGM_HOLD_MAX_MS) v = BGM_HOLD_MAX_MS;
        if (v == g_bgmHoldMs) break;
        g_bgmHoldMs = v;
        cfg_bad_clear("bgm_delay_ms");
        char val[16];
        snprintf(val, sizeof val, "%d", v);
        setting_write("battle_picker", "bgm_delay_ms", val);
        break;
    }
    case K_MARK:
        mark_cycle(dir);
        break;
    case K_TOP: {
        int v = g_ovlTopPx + (dir > 0 ? OVL_TOP_STEP : -OVL_TOP_STEP);
        if (v < 0) v = 0;
        if (v > OVL_TOP_MAX) v = OVL_TOP_MAX;
        if (v == g_ovlTopPx) break;
        g_ovlTopPx = v;
        cfg_bad_clear("top_px");
        char val[16];
        snprintf(val, sizeof val, "%d", v);
        setting_write("overlay", "top_px", val);
        break;
    }
    case K_LOG: {
        int l = g_logLevel;
        if (l < 0 || l > 2) l = 1;
        l = (l + (dir > 0 ? 1 : 2)) % 3;
        g_logLevel = l;
        cfg_bad_clear("log level");
        int ok = config_write("log", "level", kLogName[l]);
        // At every level: a change to quiet would otherwise drop its own line.
        ff13loga("[FF13-SRP] settings: log.level = %s (%s)\n", kLogName[l],
                 ok ? "ini written" : "ini NOT written");
        break;
    }
    }
}

static void cap_start(int bi) {
    if (bi < 0) return;
    for (int vk = 0; vk < 256; vk++) g_prev[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    g_cap = bi;
    ff13logv("[FF13-SRP] settings: waiting for a key for %s\n", hk_binding_name(bi));
}

void setup_ok(void) {
    if (!g_open || g_cap >= 0) return;
    int r = g_sel;
    switch (row_kind(r)) {
    case K_KEY:  cap_start(bind_idx(kKeyRowName[r])); break;
    case K_KEYS: cap_start(bind_idx(kSubName[g_sub])); break;
    case K_MULT: mult_toggle(); break;
    case K_BOOL: bool_toggle(r); break;
    }
}

// ---- the key capture --------------------------------------------------------------------------
// Keys that cannot be a binding: modifiers (they ride on another key), Esc (the game's quit
// dialog), the Windows keys, and the IME / packet codes a layout sends on its own.
static int cap_excluded(int vk) {
    if (vk < 0x08 || vk > 0xFE) return 1;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return 1;
    if (vk >= 0xA0 && vk <= 0xA5) return 1;
    if (vk == VK_ESCAPE || vk == VK_LWIN || vk == VK_RWIN) return 1;
    if (vk >= 0x15 && vk <= 0x19) return 1;
    if (vk >= 0x1C && vk <= 0x1F) return 1;
    if (vk == 0xE5 || vk == 0xE7 || vk == 0xE8) return 1;
    if (vk >= 0xF6) return 1;
    return 0;
}

static void cap_end(UINT vk) {
    g_cap = -1;
    g_settle = vk;
}

static void cap_assign(int bi, UINT b, UINT vk) {
    const char* name = hk_binding_name(bi);
    *hk_binding_vk(bi) = b;
    cfg_vk_clear_bad(hk_binding_vk(bi));
    char lab[32] = "";
    if (b) vk_label(b, lab, sizeof lab);
    cap_end(vk);
    int ok = config_write("hotkeys", name, lab);
    ff13log("[FF13-SRP] settings: hotkey %s = %s (%s)\n", name, b ? lab : "(unbound)",
            ok ? "ini written" : "ini NOT written");
}

void setup_capture_tick(void) {
    int bi = g_cap;
    if (!g_open || bi < 0) return;
    int act = hk_active();
    UINT got = 0;
    for (int vk = 0x08; vk <= 0xFE; vk++) {
        unsigned char d = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
        if (d && !g_prev[vk] && !got && act && !cap_excluded(vk)) got = (UINT)vk;
        g_prev[vk] = d;
    }
    if (!got) return;
    UINT b = got | hk_mods_now();
    UINT mine = *hk_binding_vk(bi);
    const char* name = hk_binding_name(bi);
    char lab[32], msg[288];
    vk_label(b, lab, sizeof lab);
    if ((b == g_helpVk || b == g_warpCancelVk) && b != mine) {
        cap_end(got);
        ff13logv("[FF13-SRP] settings: key for %s not changed\n", name);
        return;
    }
    if (b == mine) {
        if (!strcmp(name, "help")) {
            flash_say_c(ui_text(T_SET_CANT_UNBIND_HELP), OVL_MENU_OFF);
            return;
        }
        if (!strcmp(name, "menu_ok")) {
            flash_say_c(ui_text(T_SET_CANT_UNBIND_OK), OVL_MENU_OFF);
            return;
        }
        cap_assign(bi, 0, got);
        return;
    }
    int other = bind_owner(b, bi);
    if (other >= 0) {
        snprintf(msg, sizeof msg, ui_text(T_SET_USED), lab, fn_label(hk_binding_name(other)));
        flash_say_c(msg, OVL_MENU_OFF);
        ff13log("[FF13-SRP] settings: refused %s: used by %s\n", lab, hk_binding_name(other));
        return;
    }
    if (ok_tidy_clash(name, b)) {
        int isOk = !strcmp(name, "menu_ok");
        flash_say_c(ui_text(isOk ? T_SET_OK_IS_TIDY : T_SET_TIDY_IS_OK), OVL_MENU_OFF);
        ff13log("[FF13-SRP] settings: refused %s for %s: the same physical key as %s\n", lab, name,
                isOk ? "save_tidy" : "menu_ok");
        return;
    }
    cap_assign(bi, b, got);
}

int setup_settling(void) {
    UINT k = g_settle;
    if (!k) return 0;
    if (GetAsyncKeyState((int)k) & 0x8000) return 1;
    g_settle = 0;
    return 0;
}

// ---- drawing ----------------------------------------------------------------------------------
// Render thread. The same lapse guard as the tidy menus': a screen nobody can dismiss must not
// survive the gate that was keeping its keys from the game.
int setup_visible(void) {
    static DWORD staleSince = 0;
    if (!g_open) { staleSince = 0; return 0; }
    if (di_gate_left_ms() > 0) { staleSince = 0; return 1; }
    DWORD now = GetTickCount();
    if (!staleSince) { staleSince = now; return 1; }
    if (now - staleSince <= 2000) return 1;
    staleSince = 0;
    g_open = 0;
    g_cap = -1;
    ff13loga("[FF13-SRP] settings: the input gate lapsed under the screen -- closed\n");
    return 0;
}

static void key_col(UINT v, int capturing, char* out, size_t n) {
    char l[32];
    if (capturing) { snprintf(out, n, "[???]"); return; }
    if (!v) { snprintf(out, n, "-"); return; }
    vk_label(v, l, sizeof l);
    snprintf(out, n, "[%s]", l);
}
static const char* steps_arrow(void) {
    return gs_steps_arrow(ui_ascii_only() ? "->" : "\xE2\x86\x92");
}

// A settings row is "label:" ROW_TAB "value"; the draw puts every value at one column, past the
// widest label, since the Japanese labels are measured rather than counted.
#define ROW_TAB "\t"
// ui_arrows around `value`, centred in the width of the widest value ANY arrow row can show, so the
// right arrows stand in one column. The gap is split in px and each half laid from measured spaces.
// Render thread (it measures with the overlay's font).
static void arrows_fit(char* out, size_t n, const char* value) {
    char ms[32], px[32], pre[MARK_PRESETS + 1][40];
    snprintf(ms, sizeof ms, ui_text(T_SET_V_MS), BGM_HOLD_MAX_MS);
    snprintf(px, sizeof px, ui_text(T_SET_V_PX), OVL_TOP_MAX);
    const char* pool[16 + MARK_PRESETS];
    int pn = 0;
    pool[pn++] = ui_text(T_SET_V_ON);  pool[pn++] = ui_text(T_SET_V_OFF);
    pool[pn++] = ui_text(T_SET_V_NONE); pool[pn++] = ms; pool[pn++] = px;
    for (int i = 0; i < 3; i++) pool[pn++] = kLogName[i];
    wchar_t own[9];
    memcpy(own, g_markOwn, sizeof own);
    own[8] = 0;
    for (int i = 0; i <= MARK_PRESETS; i++) {
        const wchar_t* m = i < MARK_PRESETS ? kMarks[i] : own;
        if (m[0] && WideCharToMultiByte(CP_UTF8, 0, m, -1, pre[i], sizeof pre[i], NULL, NULL))
            pool[pn++] = pre[i];
    }
    int mx = 0;
    for (int i = 0; i < pn; i++) { int w = ovl_text_w(pool[i]); if (w > mx) mx = w; }
    int gap = mx - ovl_text_w(value);
    if (gap < 0) gap = 0;
    char lp[64], rp[64], v[160];
    int carry = ovl_space(lp, sizeof lp, gap / 2);
    ovl_space(rp, sizeof rp, gap - gap / 2 + carry);
    snprintf(v, sizeof v, "%s%s%s", lp, value, rp);
    ui_arrows(out, n, v);
}
static unsigned long row_text(int r, int sel, int cap, char* out, size_t n) {
    char k[MAX_PATH + 40];
    const char* mark = (r == sel) ? ">" : " ";
    int dim = 0;
    switch (row_kind(r)) {
    case K_HEAD:
        snprintf(out, n, "%s", ui_text(r == R_H_KEYS ? T_SET_HEAD_KEYS : T_SET_HEAD_SET));
        return OVL_BATTLE_HEAD;
    case K_KEY: {
        int bi = bind_idx(kKeyRowName[r]);
        UINT* vp = bi >= 0 ? hk_binding_vk(bi) : NULL;
        key_col(vp ? *vp : 0, cap >= 0 && cap == bi, k, sizeof k);
        snprintf(out, n, "%s %-16s %s%s", mark, k, fn_label(kKeyRowName[r]),
                 (vp && cfg_vk_bad(vp)) ? ui_text(T_HELP_BADKEY) : "");
        dim = bind_dup(bi);
        break;
    }
    case K_KEYS: {
        int o = snprintf(out, n, "%s %s", mark, ui_text(T_SET_KEYS_LABEL));
        int sub = g_sub, bad = 0;
        for (int i = 0; i < SUB_N && o < (int)n; i++) {
            int bi = bind_idx(kSubName[i]);
            UINT v = bi >= 0 ? *hk_binding_vk(bi) : 0;
            char l[32];
            if (cap >= 0 && cap == bi) snprintf(l, sizeof l, "???");
            else vk_label(v, l, sizeof l);
            o += snprintf(out + o, n - (size_t)o, "%s%s%s[%s]", i < SUB_N - 1 ? " " : "",
                          (r == sel && i == sub) ? ">" : "",
                          kSubShort[i] == T_COUNT ? "" : ui_text(kSubShort[i]), l);
            if (bind_dup(bi)) dim = 1;
            if (bi >= 0 && cfg_vk_bad(hk_binding_vk(bi))) bad = 1;
        }
        if (bad && o < (int)n) snprintf(out + o, n - (size_t)o, "%s", ui_text(T_HELP_BADKEY));
        break;
    }
    case K_BOOL: {
        int on = r == R_KEEPNOTE ? g_keepNote : g_hkGlobal;
        const char* const onOff[2] = { ui_text(T_SET_V_ON), ui_text(T_SET_V_OFF) };
        arrows_fit(k, sizeof k, onOff[on ? 0 : 1]);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark,
                 ui_text(r == R_KEEPNOTE ? T_SET_L_KEEPNOTE : T_SET_L_FOCUS), k);
        break;
    }
    case K_MULT: {
        double c[CAND_MAX];
        int cn = mult_cands(c), ms = g_msub;
        int o = snprintf(out, n, "%s %s:" ROW_TAB, mark, ui_text(T_SET_L_MULT));
        for (int i = 0; i < cn && o < (int)n; i++) {
            char f[16], v[32];
            fmt_factor(c[i], f, sizeof f);
            snprintf(v, sizeof v, ui_text(mult_has(c[i]) ? T_SET_V_MULT_IN : T_SET_V_MULT_OUT), f);
            o += snprintf(out + o, n - (size_t)o, "%s%s%s", i ? " " : "",
                          (r == sel && i == ms) ? ">" : "", v);
        }
        break;
    }
    case K_INT: {
        char v[32];
        snprintf(v, sizeof v, ui_text(T_SET_V_MS), (int)g_bgmHoldMs);
        arrows_fit(k, sizeof k, v);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_BGM), k);
        break;
    }
    case K_MARK: {
        wchar_t cur[9];
        char v[64];
        memcpy(cur, g_listMark, sizeof cur);
        cur[8] = 0;
        if (!cur[0] || !WideCharToMultiByte(CP_UTF8, 0, cur, -1, v, sizeof v, NULL, NULL))
            snprintf(v, sizeof v, "%s", ui_text(T_SET_V_NONE));
        arrows_fit(k, sizeof k, v);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_MARK), k);
        break;
    }
    case K_INFO:
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_SAVEDIR),
                 g_saveDir[0] ? g_saveDir : ui_text(T_SET_V_AUTODIR));
        dim = 1;
        break;
    case K_TOP: {
        char v[32];
        snprintf(v, sizeof v, ui_text(T_SET_V_PX), (int)g_ovlTopPx);
        arrows_fit(k, sizeof k, v);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_TOP), k);
        break;
    }
    case K_LOG: {
        int l = g_logLevel;
        arrows_fit(k, sizeof k, kLogName[(l >= 0 && l <= 2) ? l : 1]);
        snprintf(out, n, "%s %s:" ROW_TAB "%s", mark, ui_text(T_SET_L_LOG), k);
        break;
    }
    }
    if (r == sel) return OVL_MENU_SEL;
    return dim ? OVL_MENU_OFF : OVL_MENU;
}

static void footer_text(int sel, int cap, char* out, size_t n) {
    char a[32], b[32], ok[48];
    if (cap >= 0) {
        vk_label(g_helpVk, a, sizeof a);
        vk_label(g_warpCancelVk, b, sizeof b);
        snprintf(out, n, ui_text(T_SET_F_CAPTURE), a, b);
        return;
    }
    vk_label(g_bsLeftVk, a, sizeof a);
    vk_label(g_bsRightVk, b, sizeof b);
    tidy_ok_phrase(ok, sizeof ok);
    char body[512];
    int dup = 0;
    switch (sel) {
    case R_K_SPEED:  snprintf(body, sizeof body, ui_text(T_SET_F_SPEED), steps_arrow()); break;
    case R_K_KILL:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_KILL));     break;
    case R_K_WARP:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_WARP));     break;
    case R_K_SUP:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_SUPPRESS)); break;
    case R_K_SAVE:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_SAVE));     break;
    case R_K_CAM:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_CAMERA));   break;
    case R_K_TIDY:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_TIDY));     break;
    case R_K_BATTLE: snprintf(body, sizeof body, "%s", ui_text(T_SET_F_BATTLE));   break;
    case R_K_PICKER: {
        char u[32], d[32];
        vk_label(g_bsUpVk, u, sizeof u);
        vk_label(g_bsDownVk, d, sizeof d);
        snprintf(body, sizeof body, ui_text(T_SET_F_PICKER), u, d, a, b);
        break;
    }
    case R_K_HELP:   snprintf(body, sizeof body, "%s", ui_text(T_SET_F_HELP));     break;
    case R_KEYS:
        snprintf(body, sizeof body, ui_text(T_SET_F_KEYS), a, b, ok);
        dup = bind_dup(bind_idx(kSubName[g_sub]));
        break;
    case R_FOCUS:    snprintf(body, sizeof body, "%s", ui_text(T_SET_F_FOCUS));    break;
    case R_MULT:
        snprintf(body, sizeof body, ui_text(T_SET_F_MULT), a, b, ok, steps_arrow());
        break;
    case R_BGM:      snprintf(body, sizeof body, "%s", ui_text(T_SET_F_BGM));      break;
    case R_MARK:     snprintf(body, sizeof body, "%s", ui_text(T_SET_F_MARK));     break;
    case R_KEEPNOTE: snprintf(body, sizeof body, "%s", ui_text(T_SET_F_KEEPNOTE)); break;
    case R_SAVEDIR:  snprintf(body, sizeof body, "%s", ui_text(T_SET_F_SAVEDIR));  break;
    case R_TOP:      snprintf(body, sizeof body, "%s", ui_text(T_SET_F_TOP));      break;
    case R_LOG:      snprintf(body, sizeof body, "%s", ui_text(T_SET_F_LOG));      break;
    default:         body[0] = 0; break;
    }
    if (row_kind(sel) == K_KEY) dup = bind_dup(bind_idx(kKeyRowName[sel]));
    snprintf(out, n, "%s%s", dup ? ui_text(T_SET_F_DUP) : "", body);
}

int setup_draw(int x, int y) {
    static char rows[R_N][MAX_PATH + 128];
    static unsigned long cols[R_N];
    char title[384], foot[640], bad[288], hint[96];
    char hp[32], up[32], dn[32], lf[32], rt[32], ok[48];
    int sel = g_sel, cap = g_cap;
    int contX = x + 17 * ovl_char_w();
    vk_label(g_helpVk, hp, sizeof hp);
    vk_label(g_bsUpVk, up, sizeof up);     vk_label(g_bsDownVk, dn, sizeof dn);
    vk_label(g_bsLeftVk, lf, sizeof lf);   vk_label(g_bsRightVk, rt, sizeof rt);
    tidy_ok_phrase(ok, sizeof ok);
    snprintf(title, sizeof title, ui_text(T_SET_TITLE), hp, up, dn, lf, rt, ok);
    for (int r = 0; r < R_N; r++) cols[r] = row_text(r, sel, cap, rows[r], sizeof rows[0]);
    footer_text(sel, cap, foot, sizeof foot);
    bad[0] = 0;
    if (cfg_bad_list()[0]) snprintf(bad, sizeof bad, ui_text(T_HELP_BADCFG), cfg_bad_list());
    hint[0] = 0;
    if (cap < 0 && sel >= 0 && sel < R_N && (row_kind(sel) == K_KEY || row_kind(sel) == K_KEYS))
        snprintf(hint, sizeof hint, ui_text(T_SET_HINT_CHANGE), ok);
    char* vals[R_N];
    int valX = x;
    for (int r = 0; r < R_N; r++) {
        char* t = strchr(rows[r], '\t');
        vals[r] = t;
        if (!t) continue;
        *t = 0;
        int w = x + ovl_text_w(rows[r]) + ovl_char_w();
        if (w > valX) valX = w;
    }
    int titleRows = ovl_wrap_count(x, x, title);
    int lines = titleRows + ovl_wrap_count(x, x, foot) + (bad[0] ? ovl_wrap_count(x, x, bad) : 0);
    int hintX = x, hintRow = 0, selY = y + titleRows * OVL_LINE_H, selRows = 1;
    for (int r = 0; r < R_N; r++) {
        int n = vals[r] ? ovl_wrap_count(valX, contX, vals[r] + 1)
              : (r == sel && hint[0]) ? ovl_wrap_tail(x, contX, rows[r], hint, &hintX, &hintRow)
                                      : ovl_wrap_count(x, contX, rows[r]);
        if (r < sel) selY += n * OVL_LINE_H;
        if (r == sel) selRows = n;
        lines += n;
    }
    ovl_backdrop(0, y - 4, 4096, y + lines * OVL_LINE_H + 8, 0x99000000);
    if (sel >= 0 && sel < R_N) ovl_menu_band(selY, selRows);
    y += OVL_LINE_H * ovl_say_wrap(x, x, y, title, OVL_BATTLE, OVL_LINE_H);
    for (int r = 0; r < R_N; r++) {
        int n;
        if (vals[r]) {
            ovl_say(x, y, rows[r], cols[r]);
            n = ovl_say_wrap(valX, contX, y, vals[r] + 1, cols[r], OVL_LINE_H);
        } else {
            n = ovl_say_wrap(x, contX, y, rows[r], cols[r], OVL_LINE_H);
        }
        if (!vals[r] && r == sel && hint[0]) {
            ovl_say(hintX, y + hintRow * OVL_LINE_H, hint, OVL_MENU_OFF);
            n = selRows;
        }
        y += OVL_LINE_H * n;
    }
    y += OVL_LINE_H * ovl_say_wrap(x, x, y, foot, OVL_TEXT, OVL_LINE_H);
    if (bad[0]) y += OVL_LINE_H * ovl_say_wrap(x, x, y, bad, OVL_SWAP, OVL_LINE_H);
    return y + 12;
}
