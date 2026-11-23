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

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff13_keys.h"

static int ieqn(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

// One name table for the parser and the label: the label takes the FIRST name listed for a code,
// the parser takes every one. Esc stays out on purpose -- it is the game's own quit-dialog key --
// so 0x1B labels as its code and every label still parses back to its own binding.
static const struct { const char* n; UINT k; } kKeyNames[] = {
    { "Enter", VK_RETURN },   { "Return", VK_RETURN },  { "Space", VK_SPACE },
    { "Tab", VK_TAB },        { "Backspace", VK_BACK },
    { "Up", VK_UP },          { "Down", VK_DOWN },
    { "Left", VK_LEFT },      { "Right", VK_RIGHT },
    { "Home", VK_HOME },      { "End", VK_END },
    { "PageUp", VK_PRIOR },   { "PageDown", VK_NEXT },
    { "Insert", VK_INSERT },  { "Delete", VK_DELETE },
    { "Num0", 0x60 }, { "Num1", 0x61 }, { "Num2", 0x62 }, { "Num3", 0x63 },
    { "Num4", 0x64 }, { "Num5", 0x65 }, { "Num6", 0x66 }, { "Num7", 0x67 },
    { "Num8", 0x68 }, { "Num9", 0x69 },
    { "Num*", 0x6A }, { "Num+", 0x6B }, { "Num-", 0x6D }, { "Num.", 0x6E },
    { "Num/", 0x6F },
    { "Pause", 0x13 },        { "CapsLock", 0x14 },     { "NumLock", 0x90 },
    { "ScrollLock", 0x91 },   { "PrintScreen", 0x2C },  { "Apps", 0x5D },
};
#define KEY_NAMES_N ((int)(sizeof kKeyNames / sizeof kKeyNames[0]))

// The punctuation keys, whose character depends on the keyboard layout.
static int vk_is_oem(UINT vk) {
    return (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2;
}
static int ch_is_punct(int c) {
    return c >= 0x21 && c <= 0x7E && !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z')
        && !(c >= 'a' && c <= 'z');
}
// The punctuation character that parses back to vk on this layout, or 0. Two keys can type the same
// character (a Japanese layout's two backslash keys), and only the one VkKeyScan answers with may
// use it -- the other is labelled by its code, so every label parses back to its own key.
static int vk_oem_char(UINT vk) {
    if (!vk_is_oem(vk)) return 0;
    int c = (int)(MapVirtualKeyA(vk, 2 /*MAPVK_VK_TO_CHAR*/) & 0xFF);
    if (!ch_is_punct(c)) return 0;
    SHORT r = VkKeyScanA((CHAR)c);
    return (r != -1 && (UINT)(r & 0xFF) == vk) ? c : 0;
}

UINT ff13_vk_parse(const char* v) {
    UINT mods = 0;
    for (;;) {
        while (*v == ' ' || *v == '\t') v++;
        const char* plus = strchr(v, '+');
        if (!plus) break;
        size_t len = (size_t)(plus - v);
        while (len && (v[len-1] == ' ' || v[len-1] == '\t')) len--;
        UINT m = 0;
        if      (len == 4 && ieqn(v, "ctrl", 4))    m = HK_CTRL;
        else if (len == 7 && ieqn(v, "control", 7)) m = HK_CTRL;
        else if (len == 5 && ieqn(v, "shift", 5))   m = HK_SHIFT;
        else if (len == 3 && ieqn(v, "alt", 3))     m = HK_ALT;
        // Not a modifier: the '+' belongs to the key ("Num+", a bare "+"). A typo in a modifier
        // still fails below, as a key name nobody knows.
        if (!m) break;
        mods |= m;
        v = plus + 1;
    }
    while (*v == ' ' || *v == '\t') v++;
    size_t klen = strlen(v);
    while (klen && (v[klen-1] == ' ' || v[klen-1] == '\t')) klen--;
    UINT vk = 0;
    if ((v[0]=='F'||v[0]=='f') && v[1]>='0' && v[1]<='9') {
        int n = atoi(v + 1);
        if (n >= 1 && n <= 24) vk = (UINT)(VK_F1 + (n - 1));
    } else if (v[0]=='0' && (v[1]=='x'||v[1]=='X')) {
        unsigned x = (unsigned)strtoul(v, NULL, 16);
        if (x && x <= 0xFF) vk = (UINT)x;
    } else if (klen == 1 && ((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z'))) {
        vk = (UINT)(v[0] >= 'a' ? v[0] - 'a' + 'A' : v[0]);
    } else if (klen == 1 && v[0] >= '0' && v[0] <= '9') {
        vk = (UINT)v[0];
    } else if (klen == 1 && ch_is_punct((unsigned char)v[0])) {
        // Only a punctuation KEY: a character typed with Shift ("*" = Shift+8 on a US layout)
        // would otherwise bind the digit under it.
        SHORT r = VkKeyScanA(v[0]);
        if (r != -1 && !(r & 0xFF00) && vk_is_oem((UINT)(r & 0xFF))) vk = (UINT)(r & 0xFF);
    } else {
        for (int i = 0; i < KEY_NAMES_N; i++)
            if (klen == strlen(kKeyNames[i].n) && ieqn(v, kKeyNames[i].n, klen)) {
                vk = kKeyNames[i].k;
                break;
            }
    }
    return vk ? (vk | mods) : 0;
}

void ff13_vk_label(UINT b, char* out, size_t n) {
    char pre[24];
    snprintf(pre, sizeof pre, "%s%s%s", (b & HK_CTRL) ? "Ctrl+" : "",
             (b & HK_SHIFT) ? "Shift+" : "", (b & HK_ALT) ? "Alt+" : "");
    UINT vk = b & 0xFFu;
    if (!vk) { snprintf(out, n, "-"); return; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, n, "%sF%u", pre, (unsigned)(vk - VK_F1 + 1));
        return;
    }
    for (int i = 0; i < KEY_NAMES_N; i++)
        if (kKeyNames[i].k == vk) { snprintf(out, n, "%s%s", pre, kKeyNames[i].n); return; }
    int c = vk_oem_char(vk);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) c = (int)vk;
    if (c) snprintf(out, n, "%s%c", pre, (char)c);
    else   snprintf(out, n, "%s0x%X", pre, (unsigned)vk);
}
