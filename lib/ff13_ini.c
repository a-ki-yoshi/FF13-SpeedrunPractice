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
#include "ff13_ini.h"

// Line-based on purpose: the player's comments, order and line endings are what make the file
// theirs. Lines are byte ranges, so a reader's line-buffer size is not a limit here.
#define INI_WRITE_MAX (1024 * 1024)
typedef struct { size_t a, e, t; } ini_line;   // [a, e) = the text, [e, t) = its terminator

static ini_line next_line(const char* b, size_t n, size_t a) {
    ini_line l = { a, a, a };
    size_t i = a;
    while (i < n && b[i] != '\n') i++;
    l.e = (i > a && b[i - 1] == '\r') ? i - 1 : i;
    l.t = i < n ? i + 1 : i;
    return l;
}
// The whitespace the mods' ini readers trim, off both ends.
static void span_trim(const char* b, size_t* a, size_t* e) {
    while (*a < *e && (b[*a] == ' ' || b[*a] == '\t' || b[*a] == '\r')) (*a)++;
    while (*e > *a && (b[*e - 1] == ' ' || b[*e - 1] == '\t' || b[*e - 1] == '\r')) (*e)--;
}
static int span_ieq(const char* b, size_t a, size_t e, const char* s) {
    size_t n = strlen(s);
    if (e - a != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)b[a + i]) != tolower((unsigned char)s[i])) return 0;
    return 1;
}

static int fail(char* err, size_t errN, const char* file, const char* why) {
    if (err && errN) snprintf(err, errN, "%s (%s)", file, why);
    return 0;
}

int ff13_ini_write(const char* path, const char* tmp, const char* section, const char* key, const char* value,
                   const char* header, ff13_ini_notes* notes, char* err, size_t errN) {
    ff13_ini_notes dummy;
    if (!notes) notes = &dummy;
    memset(notes, 0, sizeof *notes);
    if (err && errN) err[0] = 0;
    char* in = NULL;
    size_t n = 0;
    // Only a file that is really not there starts fresh: any other failure would replace the
    // player's whole ini with one line.
    int exists = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    if (!exists) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            return fail(err, errN, path, "cannot see it");
    }
    if (exists) {
        FILE* f = fopen(path, "rb");
        if (!f) return fail(err, errN, path, "cannot open it for reading");
        long sz = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
        if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return fail(err, errN, path, "cannot size it"); }
        if (sz > INI_WRITE_MAX) { fclose(f); return fail(err, errN, path, "larger than 1 MB"); }
        n = (size_t)sz;
        in = (char*)malloc(n + 1);
        if (!in) { fclose(f); return fail(err, errN, path, "out of memory"); }
        if (n && fread(in, 1, n, f) != n) { fclose(f); free(in); return fail(err, errN, path, "read failed"); }
        fclose(f);
    }
    size_t cap = n + strlen(section) + strlen(key) + strlen(value) + (header ? strlen(header) : 0)
               + 1024;
    char* out = (char*)malloc(cap);
    if (!out) { free(in); return fail(err, errN, path, "out of memory"); }
    size_t o = 0;
#define EMIT(p, len) do { memcpy(out + o, (p), (len)); o += (len); } while (0)
#define EMITS(str) EMIT((str), strlen(str))
    if (!exists) {
        if (header) EMITS(header);
        EMITS("\n[");  EMITS(section); EMITS("]\n");
        EMITS(key); EMITS(value[0] ? " = " : " ="); EMITS(value); EMITS("\n");
    } else {
        // The file's own line ending, for any line this adds.
        int crlf = 0, lf = 0;
        for (size_t i = 0; i < n; i++)
            if (in[i] == '\n') { if (i && in[i - 1] == '\r') crlf++; else lf++; }
        const char* term = crlf > lf ? "\r\n" : "\n";
        // The readers let a later line win, so the LAST matching line (in the last copy of the
        // section) is the one that decides the setting, and the one rewritten.
        int inSec = 0;
        size_t hit = (size_t)-1, secEnd = (size_t)-1;
        for (size_t a = 0; a < n; ) {
            ini_line l = next_line(in, n, a);
            size_t ta = l.a, te = l.e;
            span_trim(in, &ta, &te);
            if (ta < te && in[ta] == '[') {
                size_t ce = ta + 1;
                while (ce < te && in[ce] != ']') ce++;
                if (ce < te) {
                    size_t sa = ta + 1, se = ce;
                    span_trim(in, &sa, &se);
                    inSec = span_ieq(in, sa, se, section);
                    if (inSec) secEnd = l.t;
                }
            } else if (inSec && ta < te) {
                secEnd = l.t;
                if (in[ta] != ';' && in[ta] != '#') {
                    size_t eq = ta;
                    while (eq < te && in[eq] != '=') eq++;
                    if (eq < te) {
                        size_t ka = ta, ke = eq;
                        span_trim(in, &ka, &ke);
                        if (span_ieq(in, ka, ke, key)) hit = a;
                    }
                }
            }
            a = l.t;
        }
        if (hit != (size_t)-1) {
            ini_line l = next_line(in, n, hit);
            size_t eq = l.a;
            while (in[eq] != '=') eq++;
            // The readers' inline-comment rule: ';' or '#' after a blank, and not the value's
            // first character.
            size_t va = eq + 1, com = (size_t)-1;
            while (va < l.e && (in[va] == ' ' || in[va] == '\t')) va++;
            for (size_t c = va + 1; c < l.e; c++)
                if ((in[c] == ';' || in[c] == '#') && (in[c - 1] == ' ' || in[c - 1] == '\t')) {
                    com = c;
                    break;
                }
            EMIT(in, eq + 1);
            if (value[0]) { EMITS(" "); EMITS(value); }
            // An empty value cannot keep its comment: the comment would become the value.
            if (com != (size_t)-1 && value[0]) {
                size_t ce = l.e;
                while (ce > com && (in[ce - 1] == ' ' || in[ce - 1] == '\t')) ce--;
                EMITS("  "); EMIT(in + com, ce - com);
            } else if (com != (size_t)-1) {
                notes->commentDropped = 1;
            }
            EMIT(in + l.e, l.t - l.e);
            EMIT(in + l.t, n - l.t);
        } else if (secEnd != (size_t)-1) {
            EMIT(in, secEnd);
            if (secEnd && in[secEnd - 1] != '\n') EMITS(term);
            EMITS(key); EMITS(value[0] ? " = " : " ="); EMITS(value); EMITS(term);
            EMIT(in + secEnd, n - secEnd);
        } else {
            EMIT(in, n);
            if (n && in[n - 1] != '\n') EMITS(term);
            if (n) EMITS(term);
            EMITS("["); EMITS(section); EMITS("]"); EMITS(term);
            EMITS(key); EMITS(value[0] ? " = " : " ="); EMITS(value); EMITS(term);
        }
    }
#undef EMITS
#undef EMIT
    free(in);
    FILE* f = fopen(tmp, "wb");
    if (!f) { free(out); return fail(err, errN, tmp, "cannot create the temporary file"); }
    size_t w = fwrite(out, 1, o, f);
    int closeBad = fclose(f) != 0;
    free(out);
    if (w != o || closeBad) { DeleteFileA(tmp); return fail(err, errN, tmp, "write failed"); }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        char why[64];
        snprintf(why, sizeof why, "replace failed, err %lu", (unsigned long)GetLastError());
        DeleteFileA(tmp);
        return fail(err, errN, path, why);
    }
    return 1;
}
