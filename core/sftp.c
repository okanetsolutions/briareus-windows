// sftp's command line and its long listing: see sftp.h.
#include "sftp.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_space(char c) { return c == ' ' || c == '\t'; }
static const char *skip_space(const char *p) { while (*p && is_space(*p)) p++; return p; }
/// The next whitespace-separated field: its start in `*start`, its length returned; 0 at the end of the line.
static size_t next_field(const char **p, const char **start) {
    const char *s = skip_space(*p), *e = s;
    while (*e && !is_space(*e) && *e != '\r' && *e != '\n') e++;
    *start = s; *p = e;
    return (size_t)(e - s);
}

bool sftp_parse_entry(const char *line, SftpEntry *out) {
    memset(out, 0, sizeof *out);
    if (!line) return false;
    const char *p = line, *f[8]; size_t n[8];
    for (int i = 0; i < 8; i++) if (!(n[i] = next_field(&p, &f[i]))) return false;
    // The mode: ten characters, the first the type.
    if (n[0] < 10 || !strchr("-dlcbps", f[0][0])) return false;
    for (size_t i = 1; i < 10; i++) if (!strchr("-rwxsStTl", f[0][i])) return false;
    for (size_t i = 0; i < n[4]; i++) if (f[4][i] < '0' || f[4][i] > '9') return false;
    // The name is the rest of the line after one run of spaces, kept as it is (names may hold spaces).
    const char *name = p;
    if (!is_space(*name)) return false;
    while (is_space(*name)) name++;
    size_t len = strlen(name);
    while (len && (name[len - 1] == '\r' || name[len - 1] == '\n')) len--;
    if (!len) return false;
    char *nm = xstrndup(name, len);
    if (f[0][0] == 'l') { char *arrow = strstr(nm, " -> "); if (arrow) *arrow = 0; }
    // Listing a path given in full prints each entry under it in full: the entry is the last part.
    char *slash = strrchr(nm, '/');
    if (slash) memmove(nm, slash + 1, strlen(slash + 1) + 1);
    if (!*nm || str_eq(nm, ".") || str_eq(nm, "..")) { free(nm); return false; }
    out->name = nm;
    memcpy(out->perms, f[0], 10); out->perms[10] = 0;
    out->dir = f[0][0] == 'd';
    out->link = f[0][0] == 'l';
    out->size = strtoll(f[4], NULL, 10);
    out->when = xstrfmt("%.*s %.*s %.*s", (int)n[5], f[5], (int)n[6], f[6], (int)n[7], f[7]);
    return true;
}
void sftp_entry_free(SftpEntry *e) {
    if (!e) return;
    free(e->name); free(e->when);
    memset(e, 0, sizeof *e);
}

char *sftp_quote(const char *path) {
    if (!path || !*path) return NULL;
    Str s; str_init(&s);
    if (path[0] == '-') str_appendz(&s, "./");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) { str_free(&s); return NULL; }
        bool plain = *p >= 0x80 || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/._-+,=@%:", *p);
        if (!plain) str_appendc(&s, '\\');
        str_appendc(&s, (char)*p);
    }
    return str_detach(&s);
}

char *sftp_join(const char *dir, const char *name) {
    if (!dir || !*dir) return xstrdup(name ? name : "");
    size_t n = strlen(dir);
    return xstrfmt("%s%s%s", dir, dir[n - 1] == '/' ? "" : "/", name ? name : "");
}
char *sftp_parent(const char *path) {
    if (!path || !*path) return NULL;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    if (n == 1 && path[0] == '/') return NULL;
    while (n && path[n - 1] != '/') n--;
    if (!n) return xstrdup(".");
    while (n > 1 && path[n - 1] == '/') n--;
    return xstrndup(path, n);
}
const char *sftp_basename(const char *path) {
    if (!path) return "";
    if (str_eq(path, "/")) return path;
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}
bool sftp_path_within(const char *path, const char *folder) {
    if (!path || !folder) return false;
    if (str_eq(folder, "/")) return path[0] == '/';
    size_t n = strlen(folder);
    return strncmp(path, folder, n) == 0 && (path[n] == 0 || path[n] == '/');
}

char *sftp_format_size(long long bytes) {
    if (bytes < 1024) return xstrfmt("%lld byte%s", bytes, bytes == 1 ? "" : "s");
    static const char *const units[] = { "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = -1;
    do { v /= 1024; u++; } while (v >= 1024 && u < 3);
    return v < 10 ? xstrfmt("%.1f %s", v, units[u]) : xstrfmt("%.0f %s", v, units[u]);
}
