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

// Whether Steam Cloud syncs a game's saves, read out of Steam's own config files (never through
// steam_api.dll: the cloud switches are vtable slots on a version-specific interface, and a wrong
// slot called in a live game is a crash).
#ifndef FF13_STEAMCLOUD_H
#define FF13_STEAMCLOUD_H
#include <stddef.h>

// appid: the game's Steam app id as text ("292120"). 1 = synced, 0 = not, -1 = Steam would not
// say. why gets one line for the caller's log.
int ff13_steam_cloud_query(const char* appid, char* why, size_t n);

#endif
