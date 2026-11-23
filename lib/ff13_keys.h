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

// Hotkey bindings as ini text and as on-screen labels. A binding is a virtual-key code with its
// modifier bits packed above it, so it travels as one UINT.
#ifndef FF13_KEYS_H
#define FF13_KEYS_H
#include <windows.h>
#include <stddef.h>

#ifndef HK_CTRL
#define HK_CTRL   0x0100u
#define HK_SHIFT  0x0200u
#define HK_ALT    0x0400u
#endif

// "F5", "Ctrl+Shift+T", "Num+", "0x2E", "@" ... -> binding; 0 = not a key this parser knows.
// Esc and Fn have no name here.
UINT ff13_vk_parse(const char* text);
// The binding as the label ff13_vk_parse reads back to the same binding; 0 -> "-".
void ff13_vk_label(UINT binding, char* out, size_t n);

#endif // FF13_KEYS_H
