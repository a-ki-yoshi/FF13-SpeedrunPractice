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
#include "ff13_steamcloud.h"

#define VDF_MAX   (4u << 20)   // sharedconfig.vdf runs to tens of KB; anything huge is not it
#define VDF_DEPTH 16
#define VDF_TOK   64

static int reg_str(HKEY root, const char* sub, const char* val, char* out, DWORD n) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, sz = n;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE*)out, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ || !sz) return 0;
    out[(sz < n) ? sz : n - 1] = 0;             // the value can come back without its terminator
    return out[0] != 0;
}
static DWORD reg_dword(HKEY root, const char* sub, const char* val) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, v = 0, sz = sizeof v;
    LONG r = RegQueryValueExA(k, val, NULL, &type, (BYTE*)&v, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_DWORD) ? v : 0;
}

// Every "key" "value" pair, lower-cased, with the block names above it (stack[0..depth-1]).
// Depth is counted past the stack's end so braces still pair when nesting exceeds VDF_DEPTH.
typedef void (*vdf_pair_fn)(char stack[][VDF_TOK], int depth, const char* key, const char* val,
                            void* ctx);
static void vdf_walk(const char* buf, vdf_pair_fn fn, void* ctx) {
    char stack[VDF_DEPTH][VDF_TOK], pend[VDF_TOK] = "";
    int depth = 0, havePend = 0;
    for (const char* p = buf; *p; ) {
        if (*p == '"') {
            char tok[VDF_TOK]; size_t n = 0;
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1]) p++;
                if (n + 1 < sizeof tok) tok[n++] = (char)tolower((unsigned char)*p);
            }
            tok[n] = 0;
            if (*p == '"') p++;
            if (!havePend) { memcpy(pend, tok, n + 1); havePend = 1; continue; }
            if (depth <= VDF_DEPTH) fn(stack, depth, pend, tok, ctx);
            havePend = 0;
        } else if (*p == '{') {
            if (depth < VDF_DEPTH) snprintf(stack[depth], VDF_TOK, "%s", havePend ? pend : "");
            depth++; havePend = 0; p++;
        } else if (*p == '}') {
            if (depth) depth--;
            havePend = 0; p++;
        } else if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
        } else p++;
    }
}
static char* read_text(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    char* buf = (sz != INVALID_FILE_SIZE && sz < VDF_MAX) ? (char*)malloc((size_t)sz + 1) : NULL;
    int ok = buf && ReadFile(h, buf, sz, &got, NULL) && got == sz;
    CloseHandle(h);
    if (!ok) { free(buf); return NULL; }
    buf[sz] = 0;
    return buf;
}

typedef struct { const char* appid; int acct, app; } cloud_ctx;
static void cloud_pair(char stack[][VDF_TOK], int depth, const char* key, const char* val, void* v) {
    cloud_ctx* c = (cloud_ctx*)v;
    if (strcmp(key, "cloudenabled") || depth < 1) return;
    int on = strcmp(val, "0") ? 1 : 0;
    if (!strcmp(stack[depth - 1], "steam")) c->acct = on;
    else if (depth > 1 && !strcmp(stack[depth - 1], c->appid) && !strcmp(stack[depth - 2], "apps"))
        c->app = on;
}

// userdata\<id> with the only account there, or 0 when there are none or several.
static int only_user_dir(const char* root, char* out, size_t n) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\userdata\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0; char only[64] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] < '1' || fd.cFileName[0] > '9') continue;   // "." ".." and "0"
        if (++found == 1) snprintf(only, sizeof only, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (found != 1) return 0;
    snprintf(out, n, "%s\\userdata\\%s", root, only);
    return 1;
}

// ---- Steam on Linux, seen from inside Proton ---------------------------------------------------
// Proton's steam.exe stand-in registers a Steam folder inside the prefix with no userdata and no
// ActiveUser, so the Windows lookup always answers "unknown" there. Steam hands every Proton game
// the real client folder (a Linux path) in STEAM_COMPAT_CLIENT_INSTALL_PATH, and Proton maps Z: to /.
static int proton_steam_root(char* out, size_t n) {
    WCHAR wunix[MAX_PATH];
    DWORD k = GetEnvironmentVariableW(L"STEAM_COMPAT_CLIENT_INSTALL_PATH", wunix, MAX_PATH);
    if (!k || k >= MAX_PATH) return 0;
    char unix8[MAX_PATH * 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, wunix, -1, unix8, sizeof unix8, NULL, NULL)) return 0;
    typedef WCHAR* (__cdecl *dos_name_fn)(const char*);
    dos_name_fn dos = (dos_name_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                        "wine_get_dos_file_name");
    if (dos) {
        WCHAR* w = dos(unix8);
        if (w) {
            BOOL lossy = FALSE;
            int ok = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, (int)n, NULL, &lossy);
            HeapFree(GetProcessHeap(), 0, w);
            if (ok && !lossy) return 1;
        }
    }
    if (unix8[0] != '/') return 0;
    for (const char* s = unix8; *s; s++) if ((unsigned char)*s >= 0x80) return 0;   // not ANSI-safe
    if (snprintf(out, n, "Z:%s", unix8) >= (int)n) return 0;
    for (char* s = out; *s; s++) if (*s == '/') *s = '\\';
    return 1;
}

static int env_a(const char* name, char* out, DWORD n) {
    DWORD k = GetEnvironmentVariableA(name, out, n);
    return k && k < n;
}

// config\loginusers.vdf: "users" { "<SteamID64>" { "AccountName" .. "MostRecent" "1" } }.
typedef struct {
    char want[VDF_TOK];                 // the launching account's name, lower-cased, or ""
    char cur[VDF_TOK];                  // the SteamID64 block being read
    unsigned long long named, recent;   // the SteamID64 picked each way
} login_ctx;
static void login_pair(char stack[][VDF_TOK], int depth, const char* key, const char* val, void* v) {
    login_ctx* c = (login_ctx*)v;
    if (depth != 2 || strcmp(stack[0], "users")) return;
    unsigned long long id = strtoull(stack[1], NULL, 10);
    if (!id) return;
    if (!strcmp(key, "accountname") && c->want[0] && !strcmp(val, c->want)) c->named = id;
    if (!strcmp(key, "mostrecent") && !strcmp(val, "1")) c->recent = id;
}
static int linux_user_dir(const char* root, char* out, size_t n) {
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\config\\loginusers.vdf", root);
    char* buf = read_text(p);
    if (buf) {
        login_ctx c;
        memset(&c, 0, sizeof c);
        char name[VDF_TOK] = "";
        if (env_a("SteamAppUser", name, sizeof name) || env_a("SteamUser", name, sizeof name))
            for (size_t i = 0; name[i]; i++) c.want[i] = (char)tolower((unsigned char)name[i]);
        vdf_walk(buf, login_pair, &c);
        free(buf);
        unsigned long long id = c.named ? c.named : c.recent;
        if (id) {   // userdata is keyed by the account id, the SteamID64's low 32 bits
            snprintf(out, n, "%s\\userdata\\%lu", root, (unsigned long)(id & 0xFFFFFFFFull));
            DWORD a = GetFileAttributesA(out);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
        }
    }
    return only_user_dir(root, out, n);
}

// ---- Steam on Windows ------------------------------------------------------------------------
// userdata\<id> is per Steam account: the wrong one answers about somebody else's switches.
static int windows_user_dir(const char* root, char* out, size_t n) {
    DWORD id = reg_dword(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "ActiveUser");
    if (id) { snprintf(out, n, "%s\\userdata\\%lu", root, (unsigned long)id); return 1; }
    return only_user_dir(root, out, n);
}

int ff13_steam_cloud_query(const char* appid, char* why, size_t n) {
    char root[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    int proton = proton_steam_root(root, sizeof root);
    if (!proton
     && !reg_str(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", root, sizeof root)
     && !reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath", root, sizeof root)) {
        snprintf(why, n, "Steam is not in the registry -- its cloud switch is unknown");
        return -1;
    }
    for (char* s = root; *s; s++) if (*s == '/') *s = '\\';   // SteamPath is written with slashes
    if (!(proton ? linux_user_dir(root, dir, sizeof dir) : windows_user_dir(root, dir, sizeof dir))) {
        snprintf(why, n, "no single Steam account under %s\\userdata%s -- the cloud switch is "
                 "unknown", root, proton ? " (Steam on Linux)" : "");
        return -1;
    }
    snprintf(path, sizeof path, "%s\\7\\remote\\sharedconfig.vdf", dir);
    char* buf = read_text(path);
    if (!buf) {
        snprintf(why, n, "%s could not be read -- the cloud switch is unknown", path);
        return -1;
    }
    cloud_ctx c = { appid, -1, -1 };
    vdf_walk(buf, cloud_pair, &c);
    free(buf);
    int on = !(c.acct == 0 || c.app == 0);         // unset counts as on; either 0 means not synced
    snprintf(why, n, "sync is %s for this game (account switch %s, game switch %s, from %s)",
             on ? "ON" : "off",
             c.acct < 0 ? "unset" : c.acct ? "on" : "off",
             c.app  < 0 ? "unset" : c.app  ? "on" : "off", path);
    return on;
}
