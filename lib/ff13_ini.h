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

// Rewrites one setting of an ini file in place, leaving every other byte as it was.
#ifndef FF13_INI_H
#define FF13_INI_H
#include <stddef.h>

// What happened besides the write itself, for the caller's log.
typedef struct {
    int commentDropped;      // the line's inline comment went with an empty value
} ff13_ini_notes;

// [section] key = value in `path` (the last such line in the last copy of the section, as a
// later-line-wins reader sees it; appended when missing). The file is written to tmpPath and moved
// over path. A missing file is created with newFileHeader (verbatim, may be NULL) at its top.
// 1 = written. 0 = not; err holds "<file> (<reason>)". notes may be NULL. Not thread-safe: one
// writer thread per file.
int ff13_ini_write(const char* path, const char* tmpPath, const char* section, const char* key, const char* value,
                   const char* newFileHeader, ff13_ini_notes* notes, char* err, size_t errN);

#endif // FF13_INI_H
