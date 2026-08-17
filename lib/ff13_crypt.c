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

// FF13 ff13-NN.dat save crypto — faithful C port of dat_crypt.py.
// Only the low 32 bits of each half survive to the output, so all arithmetic is
// done with wrapping uint32_t (verified equivalent to the Python big-int version).
//
// Attribution (GPLv3): the White-engine XOR-table block cipher used here — the
// 264-byte table generator (gen_xor_table), the SpecialKey constant 0xA1652347,
// and the 8-byte block encrypt/decrypt — is derived from WhiteCryptTool by Surihix
// (https://github.com/Surihix/WhiteCryptTool, Copyright (C) Surihix, GNU GPL v3.0).
// Changes vs. WhiteCryptTool: ported from C# to C; WhiteCryptTool handles the
// trilogy's clb/filelist files, whereas this file targets FF13-1 save files — it
// uses the exe's hardcoded 8-byte save seed (data VA 0x281b968: 8e 40 91 d1 5e 8f
// aa 56, passed WITHOUT the byte-reversal WhiteCryptTool applies to per-file seeds)
// and leaves the 0x208 plaintext header and trailing 8-byte footer unencrypted.
// Both works are GPLv3, so this derivation is license-compatible; this notice
// preserves attribution and records the modifications as GPLv3 sec. 5 requires.
#include "ff13_crypt.h"
#include <string.h>

// Hardcoded save seed (exe data VA 0x281b968), no byte-reversal. 8e4091d15e8faa56
static const uint8_t SAVE_SEED[8] = {0x8e,0x40,0x91,0xd1,0x5e,0x8f,0xaa,0x56};

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static uint32_t rotl32(uint32_t v, int n) { return (v<<n) | (v>>(32-n)); }

// gen_xor_table: 264-byte table from the 8-byte seed.
static void gen_xor_table(uint8_t table[264]) {
    uint32_t A = rd32(SAVE_SEED), B = rd32(SAVE_SEED+4);
    A = rotl32(A, 8);
    B = (B>>16) | (B<<16);
    uint8_t xb[8];
    wr32(xb, B); wr32(xb+4, A);
    xb[0] = (uint8_t)(xb[0] + 0x45);
    for (int i=1; i<8; i++) {
        unsigned tmp = xb[i] + 0xD4u + xb[i-1];
        tmp ^= (unsigned)(xb[i-1] << 2);
        tmp ^= 0x45u;
        xb[i] = (uint8_t)tmp;
    }
    memcpy(table, xb, 8);
    int ci = 8;
    uint64_t prev = (uint64_t)rd32(xb) | ((uint64_t)rd32(xb+4) << 32);
    for (int k=1; k<0x21; k++) {
        uint32_t bA = (uint32_t)prev;
        uint32_t bB = (uint32_t)(prev >> 32);
        uint64_t a = 5ull * prev;
        a ^= ((uint64_t)bB << 32);
        uint32_t tmpA = (uint32_t)((uint32_t)bA ^ (uint32_t)a);
        uint32_t tmpB = (uint32_t)(a >> 32);
        a = (uint64_t)bA | (a & 0xFFFFFFFF00000000ull);
        uint32_t xA = (uint32_t)((uint32_t)a ^ tmpA);
        tmpB ^= bB;
        uint32_t xB = tmpB;
        wr32(xb, xA); wr32(xb+4, xB);
        memcpy(table+ci, xb, 8); ci += 8;
        prev = (uint64_t)rd32(xb) | ((uint64_t)rd32(xb+4) << 32);
    }
}

static void bc_setup(uint32_t bc, uint32_t* toff, uint32_t* Eval, uint32_t* Fval) {
    *toff = bc & 0xF8u;
    uint64_t val = (uint64_t)bc | ((uint64_t)bc<<10) | ((uint64_t)bc<<20) | ((uint64_t)bc<<30);
    *Eval = (uint32_t)val;
    *Fval = (uint32_t)(val >> 32);
}

// special key: k1 = (Eval+0xA1652347) low32; carry -> k2 = Fval+carry.
static void special_key(uint32_t Eval, uint32_t Fval, uint32_t* k1, uint32_t* k2) {
    uint32_t carry = (Eval > (~0xA1652347u)) ? 1u : 0u;
    *k1 = Eval + 0xA1652347u;
    *k2 = Fval + carry;
}

static uint8_t loop_fwd(int db, const uint8_t* table, uint32_t toff) {
    for (int i=0; i<8; i++) {
        int cv = ((db + 120) & 0xff) - table[toff + i];
        db = cv & 0xff;
    }
    return (uint8_t)(db & 0xff);
}
static uint8_t loop_rev(int b, const uint8_t* table, uint32_t toff) {
    for (int i=7; i>=0; i--)
        b = ((table[toff + i] + b) - 120) & 0xff;
    return (uint8_t)(b & 0xff);
}

static void decrypt_block(const uint8_t* cur, uint32_t bc, const uint8_t* table, uint8_t* out) {
    uint32_t blockId = bc >> 3, toff, Eval, Fval;
    bc_setup(bc, &toff, &Eval, &Fval);
    uint8_t d1 = loop_fwd((((blockId ^ 69) & 255) ^ cur[0]), table, toff);
    uint8_t d2 = loop_fwd((cur[0]^cur[1]), table, toff);
    uint8_t d3 = loop_fwd((cur[1]^cur[2]), table, toff);
    uint8_t d4 = loop_fwd((cur[2]^cur[3]), table, toff);
    uint8_t d5 = loop_fwd((cur[3]^cur[4]), table, toff);
    uint8_t d6 = loop_fwd((cur[4]^cur[5]), table, toff);
    uint8_t d7 = loop_fwd((cur[5]^cur[6]), table, toff);
    uint8_t d8 = loop_fwd((cur[6]^cur[7]), table, toff);
    uint8_t arr[8] = {d5,d6,d7,d8,d1,d2,d3,d4};
    uint32_t higher = rd32(arr), lower = rd32(arr+4);
    uint32_t xlo = rd32(table+toff), xhi = rd32(table+toff+4);
    uint32_t k1, k2; special_key(Eval, Fval, &k1, &k2);
    uint32_t carry = (lower < xlo) ? 1u : 0u;
    uint32_t Llo = lower - xlo;
    uint32_t Lhi = higher - xhi - carry;
    Llo ^= k1; Lhi ^= k2; Llo ^= xlo; Lhi ^= xhi;
    wr32(out, Lhi); wr32(out+4, Llo);
}

static void encrypt_block(const uint8_t* pt, uint32_t bc, const uint8_t* table, uint8_t* out) {
    uint32_t blockId = bc >> 3, toff, Eval, Fval;
    bc_setup(bc, &toff, &Eval, &Fval);
    uint32_t loVal = rd32(pt+4);   // pt[4..7]
    uint32_t hiVal = rd32(pt);     // pt[0..3]
    uint32_t xlo = rd32(table+toff), xhi = rd32(table+toff+4);
    uint32_t k1, k2; special_key(Eval, Fval, &k1, &k2);
    loVal ^= xlo; hiVal ^= xhi;
    loVal ^= k1;  hiVal ^= k2;
    loVal += xlo; hiVal += xhi;
    if (loVal < xlo) hiVal += 1;
    uint8_t comp[8]; wr32(comp, loVal); wr32(comp+4, hiVal);
    uint8_t e1 = (uint8_t)(((blockId ^ 69) & 255) ^ loop_rev(comp[0], table, toff));
    uint8_t e2 = (uint8_t)(e1 ^ loop_rev(comp[1], table, toff));
    uint8_t e3 = (uint8_t)(e2 ^ loop_rev(comp[2], table, toff));
    uint8_t e4 = (uint8_t)(e3 ^ loop_rev(comp[3], table, toff));
    uint8_t e5 = (uint8_t)(e4 ^ loop_rev(comp[4], table, toff));
    uint8_t e6 = (uint8_t)(e5 ^ loop_rev(comp[5], table, toff));
    uint8_t e7 = (uint8_t)(e6 ^ loop_rev(comp[6], table, toff));
    uint8_t e8 = (uint8_t)(e7 ^ loop_rev(comp[7], table, toff));
    out[0]=e1; out[1]=e2; out[2]=e3; out[3]=e4; out[4]=e5; out[5]=e6; out[6]=e7; out[7]=e8;
}

static void cipher_stream(uint8_t* data, size_t n,
                          void (*fn)(const uint8_t*, uint32_t, const uint8_t*, uint8_t*)) {
    uint8_t table[264]; gen_xor_table(table);
    uint8_t out[8];
    uint32_t bc = 0;
    for (size_t i=0; i+8<=n; i+=8) {
        fn(data + i, bc, table, out);
        memcpy(data + i, out, 8);
        bc += 8;
    }
}

void ff13_decrypt(uint8_t* img, size_t len) {
    if (len <= FF13_HEADER_LEN + FF13_TRAILER_LEN) return;
    cipher_stream(img + FF13_HEADER_LEN, len - FF13_HEADER_LEN - FF13_TRAILER_LEN, decrypt_block);
}
void ff13_encrypt(uint8_t* img, size_t len) {
    if (len <= FF13_HEADER_LEN + FF13_TRAILER_LEN) return;
    cipher_stream(img + FF13_HEADER_LEN, len - FF13_HEADER_LEN - FF13_TRAILER_LEN, encrypt_block);
}
