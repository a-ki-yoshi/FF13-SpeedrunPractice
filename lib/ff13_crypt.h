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

// FF13 ff13-NN.dat save crypto. Platform-independent.
// The XOR-table block cipher is derived from WhiteCryptTool by Surihix (GPLv3,
// https://github.com/Surihix/WhiteCryptTool); see ff13_crypt.c for the full
// attribution and the list of changes. Both works are GPLv3 (license-compatible).
#ifndef FF13_CRYPT_H
#define FF13_CRYPT_H
#include <stddef.h>
#include <stdint.h>

#define FF13_HEADER_LEN  0x208      // 520 plaintext header (not encrypted)
#define FF13_FILE_LEN    300600
#define FF13_TRAILER_LEN 8          // last 8 bytes excluded from the cipher

// In-place: decrypt/encrypt the body [FF13_HEADER_LEN .. len-FF13_TRAILER_LEN).
// `img` is the full FF13_FILE_LEN image; header and trailer are left untouched.
void ff13_decrypt(uint8_t* img, size_t len);
void ff13_encrypt(uint8_t* img, size_t len);

// One block-aligned window of the body, decrypted where it lies: `buf` holds the bytes read from
// file offset `off` (>= FF13_HEADER_LEN, 8-byte aligned with `len`, ending before the trailer).
void ff13_decrypt_window(uint8_t* buf, size_t off, size_t len);

// The same cipher with any 8-byte seed. ff13_crypt_table builds the 264-byte table from `seed`
// exactly as given (no byte reversal; formats that reverse their seed do it before the call).
// ff13_decrypt_window_with decrypts `len` bytes in place; `base` is the offset of buf[0] from the
// start of its cipher stream (the block counter). Misaligned base or len leaves `buf` untouched.
void ff13_crypt_table(const uint8_t seed[8], uint8_t table[264]);
void ff13_decrypt_window_with(const uint8_t table[264], uint8_t* buf, uint32_t base, size_t len);

#endif
