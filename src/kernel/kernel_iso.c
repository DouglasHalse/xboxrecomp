/* A disc image mounted read-only underneath the game directory.
 *
 * See kernel_iso.h for what this is for. Two things about the shape are worth
 * knowing before reading the code:
 *
 *   - The mount point is discovered, not passed in: <game_dir>/.disc.mount
 *     lists the payload's descriptors, because a bundled launcher can hand a
 *     descriptor to the process but cannot set the process environment before
 *     the library is loaded. The mount file's own directory is the prefix that
 *     maps "data/audio/music.big" onto a host path.
 *   - The image is split into parts. An entry of an APK is addressed with the
 *     zip format's 32-bit offsets, so the payload is cut below 2 GiB and reads
 *     are stitched across the seam here; nothing above this file knows.
 *
 * The filesystem is XDVDFS, read with the same B-tree walk as the port's
 * extractor (src/android/xiso.c) and flattened into a path index at mount time.
 */
#include "kernel_iso.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* The Windows build never mounts an image; this shim exists so tests/iso can
 * exercise the parser on a host that has no pread(). */
#include <io.h>
#include <direct.h>
#ifndef F_OK
#define F_OK 0            /* MSVC spells it in <io.h> without the flag */
#endif
#define iso_strcasecmp _stricmp
static long long iso_pread(int fd, void *buf, size_t n, long long off)
{
    if (_lseeki64(fd, off, SEEK_SET) != off)
        return -1;
    return (long long)_read(fd, buf, (unsigned)n);
}
#else
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#define iso_strcasecmp strcasecmp
#define iso_pread(fd, buf, n, off) pread((fd), (buf), (n), (off_t)(off))
#endif

#define ISO_SECTOR      2048u
#define ISO_MAX_PARTS   8
#define ISO_MAX_ENTRIES 16384
#define ISO_MAX_DEPTH   12
#define ISO_MAX_TABLE   (4u << 20)
#define ISO_MAX_HANDLES 64

static const unsigned char iso_magic[20] = {
    'M','I','C','R','O','S','O','F','T','*','X','B','O','X','*','M','E','D','I','A'
};

typedef struct {
    int      fd;
    uint64_t off;      /* offset of this part inside the host file */
    uint64_t len;
    uint64_t start;    /* offset of this part inside the image */
} iso_part;

typedef struct {
    char    *path;     /* "data/audio/music.big"; "" is the root */
    uint64_t off;      /* image offset of the file's first byte */
    uint32_t size;
    int      isdir;
} iso_entry;

typedef struct {
    int      used;
    int      entry;    /* index of the file's entry, or the directory's */
} iso_handle;

static struct {
    int        state;      /* 0 = not tried, 1 = mounted, 2 = no mount file */
    char       prefix[1024];
    iso_part   part[ISO_MAX_PARTS];
    int        npart;
    uint64_t   image_size;
    iso_entry *ent;
    int        nent;
    iso_handle handle[ISO_MAX_HANDLES];
} s_iso;

/* ---------------------------------------------------------------- reading */

static int img_read(void *dst, size_t n, uint64_t off)
{
    unsigned char *p = (unsigned char *)dst;

    if (n == 0)
        return 0;
    if (off > s_iso.image_size || n > s_iso.image_size - off)
        return -1;
    while (n > 0) {
        int i;
        uint64_t rel, avail;
        size_t take;

        for (i = 0; i < s_iso.npart; i++)
            if (off >= s_iso.part[i].start && off < s_iso.part[i].start + s_iso.part[i].len)
                break;
        if (i == s_iso.npart)
            return -1;
        rel = off - s_iso.part[i].start;
        avail = s_iso.part[i].len - rel;
        take = (uint64_t)n < avail ? n : (size_t)avail;
        if (iso_pread(s_iso.part[i].fd, p, take, (long long)(s_iso.part[i].off + rel)) !=
            (long long)take)
            return -1;
        p += take;
        off += take;
        n -= take;
    }
    return 0;
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* A name we are willing to put in the index. The image is untrusted input and
 * this index is what the guest's paths are matched against. */
static int name_ok(const char *name, size_t len)
{
    size_t i;

    if (len == 0 || len >= 250)
        return 0;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '/' || ch == '\\' || ch == ':' || ch < 0x20 || ch == 0x7f)
            return 0;
    }
    return 1;
}

static int add_entry(const char *path, uint64_t off, uint32_t size, int isdir)
{
    char *copy;

    if (s_iso.nent >= ISO_MAX_ENTRIES)
        return -1;
    copy = (char *)malloc(strlen(path) + 1);
    if (!copy)
        return -1;
    strcpy(copy, path);
    s_iso.ent[s_iso.nent].path  = copy;
    s_iso.ent[s_iso.nent].off   = off;
    s_iso.ent[s_iso.nent].size  = size;
    s_iso.ent[s_iso.nent].isdir = isdir;
    s_iso.nent++;
    return 0;
}

static int walk_dir(uint32_t dir_start, uint32_t dir_size, const char *prefix,
                    uint64_t base, int depth);

/* One directory table: a B-tree of dirents, `off` being a byte offset inside
 * `table`. left/right are in 4-byte units from the start of the same table. */
static int walk_table(const unsigned char *table, uint32_t table_size, uint32_t off,
                      unsigned char *seen, const char *prefix, uint64_t base, int depth)
{
    uint16_t left, right;
    uint32_t start, size;
    unsigned char attrs, namelen;
    char name[256];
    char path[1200];
    int is_dir;

    if (off + 14 > table_size || seen[off / 4])
        return 0;
    seen[off / 4] = 1;

    left    = rd16(table + off);
    right   = rd16(table + off + 2);
    start   = rd32(table + off + 4);
    size    = rd32(table + off + 8);
    attrs   = table[off + 12];
    namelen = table[off + 13];

    if (namelen && off + 14u + namelen <= table_size &&
        name_ok((const char *)table + off + 14, namelen)) {
        memcpy(name, table + off + 14, namelen);
        name[namelen] = '\0';
        is_dir = (attrs & 0x10) != 0;
        if (*prefix)
            snprintf(path, sizeof path, "%s/%s", prefix, name);
        else
            snprintf(path, sizeof path, "%s", name);
        if (add_entry(path, base + (uint64_t)start * ISO_SECTOR, size, is_dir) != 0)
            return -1;
        if (is_dir && walk_dir(start, size, path, base, depth + 1) != 0)
            return -1;
    }

    if (left && walk_table(table, table_size, left * 4u, seen, prefix, base, depth) != 0)
        return -1;
    if (right && walk_table(table, table_size, right * 4u, seen, prefix, base, depth) != 0)
        return -1;
    return 0;
}

static int walk_dir(uint32_t dir_start, uint32_t dir_size, const char *prefix,
                    uint64_t base, int depth)
{
    unsigned char *table, *seen;
    int rc;

    if (depth > ISO_MAX_DEPTH)
        return 0;                      /* malformed image: stop descending */
    if (dir_size == 0)
        return 0;
    if (dir_size > ISO_MAX_TABLE)
        return -1;

    table = (unsigned char *)malloc(dir_size);
    seen  = (unsigned char *)calloc(dir_size / 4 + 1, 1);
    if (!table || !seen) {
        free(table);
        free(seen);
        return -1;
    }
    rc = 0;
    if (img_read(table, dir_size, base + (uint64_t)dir_start * ISO_SECTOR) != 0 ||
        walk_table(table, dir_size, 0, seen, prefix, base, depth) != 0)
        rc = -1;
    free(table);
    free(seen);
    return rc;
}

/* The volume descriptor: a trimmed image has it at 0x10000, a redump-style
 * image behind the 405 MiB video partition. Sector numbers in the dirents are
 * relative to the descriptor minus one sector. */
static int find_root(uint64_t *root_start, uint32_t *root_size, uint64_t *base)
{
    static const uint64_t candidate[2] = { 0x10000ull, 0x18300000ull + 0x10000ull };
    unsigned char vd[ISO_SECTOR];
    int i;

    for (i = 0; i < 2; i++) {
        if (s_iso.image_size < candidate[i] + ISO_SECTOR)
            continue;
        if (img_read(vd, sizeof vd, candidate[i]) != 0)
            continue;
        if (memcmp(vd, iso_magic, sizeof iso_magic) != 0)
            continue;
        *root_start = rd32(vd + 20);
        *root_size  = rd32(vd + 24);
        *base       = candidate[i] - 0x10000u;
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------- mounting */

/* "fd off len" or "fd,off,len". */
static int parse_part(const char *value)
{
    char buf[128];
    char *field[3];
    int n = 0;
    char *p;

    if (strlen(value) >= sizeof buf)
        return -1;
    strcpy(buf, value);
    for (p = buf; *p && n < 3; ) {
        char *start;
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != ',' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
        field[n++] = start;
    }
    if (n != 3 || s_iso.npart >= ISO_MAX_PARTS)
        return -1;
    s_iso.part[s_iso.npart].fd    = atoi(field[0]);
    s_iso.part[s_iso.npart].off   = strtoull(field[1], NULL, 10);
    s_iso.part[s_iso.npart].len   = strtoull(field[2], NULL, 10);
    s_iso.part[s_iso.npart].start = s_iso.image_size;
    if (s_iso.part[s_iso.npart].fd < 0 || s_iso.part[s_iso.npart].len == 0)
        return -1;
    s_iso.image_size += s_iso.part[s_iso.npart].len;
    s_iso.npart++;
    return 0;
}

static int mount_file(const char *game_dir)
{
    char path[1200], line[1600];
    FILE *f;
    int mounted = 0;

    snprintf(path, sizeof path, "%s/.disc.mount", game_dir);
    f = fopen(path, "r");
    if (!f)
        return 0;
    snprintf(s_iso.prefix, sizeof s_iso.prefix, "%s", game_dir);
    while (fgets(line, sizeof line, f)) {
        char *p = line, *key, *eq, *val, *end;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = p;
        for (end = key + strlen(key); end > key &&
             (end[-1] == ' ' || end[-1] == '\t'); )
            *--end = '\0';
        val = eq + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                             end[-1] == '\t'))
            *--end = '\0';
        if (!iso_strcasecmp(key, "PREFIX")) {
            if (*val)
                snprintf(s_iso.prefix, sizeof s_iso.prefix, "%s", val);
        } else if (!iso_strcasecmp(key, "PART")) {
            if (parse_part(val) == 0)
                mounted = 1;
        }
    }
    fclose(f);
    return mounted ? 1 : 0;
}

static void free_index(void)
{
    int i;

    for (i = 0; i < s_iso.nent; i++)
        free(s_iso.ent[i].path);
    free(s_iso.ent);
    s_iso.ent  = NULL;
    s_iso.nent = 0;
}

int xbox_iso_ready(const char *game_dir)
{
    uint64_t root_start = 0, base = 0;
    uint32_t root_size = 0;
    size_t prefix_len;

    if (s_iso.state == 2)
        return 0;
    if (s_iso.state == 1)
        return 1;

    s_iso.state = 2;
    if (!game_dir || !game_dir[0])
        return 0;
    if (!mount_file(game_dir))
        return 0;                     /* the usual case: nothing bundled */
    if (find_root(&root_start, &root_size, &base) != 0) {
        fprintf(stderr, "[ISO] no Xbox filesystem in the %d part(s) mounted "
                        "from %s/.disc.mount\n", s_iso.npart, game_dir);
        return 0;
    }

    prefix_len = strlen(s_iso.prefix);
    while (prefix_len > 1 && s_iso.prefix[prefix_len - 1] == '/')
        s_iso.prefix[--prefix_len] = '\0';

    s_iso.ent = (iso_entry *)calloc(ISO_MAX_ENTRIES, sizeof(iso_entry));
    if (!s_iso.ent)
        return 0;
    if (add_entry("", base + (uint64_t)root_start * ISO_SECTOR, root_size, 1) != 0 ||
        walk_dir((uint32_t)root_start, root_size, "", base, 0) != 0) {
        fprintf(stderr, "[ISO] the directory tree could not be read\n");
        free_index();
        return 0;
    }

    s_iso.state = 1;
    fprintf(stderr, "[ISO] %d file(s) mounted from %d part(s), %llu MB, at %s\n",
            s_iso.nent - 1, s_iso.npart, (unsigned long long)(s_iso.image_size >> 20),
            s_iso.prefix);
    return 1;
}

/* ---------------------------------------------------------------- lookups */

/* Index of the entry whose path is `rel` (relative to the mount point, no
 * leading slash). 0 is the root. */
static int find_rel(const char *rel)
{
    int i;

    for (i = 0; i < s_iso.nent; i++)
        if (!iso_strcasecmp(s_iso.ent[i].path, rel))
            return i;
    return -1;
}

/* The path the guest asked for, minus the mount prefix. NULL when it is
 * outside the prefix. */
static const char *strip_prefix(const char *host_path)
{
    size_t n = strlen(s_iso.prefix);

    if (strncmp(host_path, s_iso.prefix, n) != 0)
        return NULL;
    if (host_path[n] == '\0')
        return "";
    if (host_path[n] != '/')
        return NULL;
    return host_path + n + 1;
}

/* Resolve a relative host path against the working directory: the ports probe
 * their own files as "game/default.xbe", not as an absolute path. */
static const char *absolute(const char *path, char *buf, size_t cap)
{
    char cwd[1024];

    if (path[0] == '/' || path[0] == '\\')
        return path;
#if defined(_WIN32)
    if (path[0] && path[1] == ':')
        return path;
#endif
    if (!getcwd(cwd, sizeof cwd))
        return NULL;
    /* getcwd answers with the host's separators (backslashes on Windows); the
     * mount point is spelled the way the app gives it, always with slashes. */
    for (char *p = cwd; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (snprintf(buf, cap, "%s/%s", cwd, path) >= (int)cap)
        return NULL;
    return buf;
}

static int find_host(const char *host_path)
{
    const char *rel;
    char clean[1200], abs[1200];
    size_t i, n = 0;

    if (!host_path || !s_iso.state)
        return -1;
    host_path = absolute(host_path, abs, sizeof abs);
    if (!host_path)
        return -1;
    rel = strip_prefix(host_path);
    if (!rel)
        return -1;
    /* Backslashes cannot appear in an index path, and a doubled slash would
     * miss it: normalise the guest's spelling first. */
    if (strlen(rel) >= sizeof clean)
        return -1;
    for (i = 0; rel[i]; i++) {
        if (rel[i] == '\\') {
            if (n + 1 < sizeof clean)
                clean[n++] = '/';
        } else if (rel[i] == '/' && n > 0 && clean[n - 1] == '/') {
            continue;
        } else if (n + 1 < sizeof clean) {
            clean[n++] = rel[i];
        }
    }
    clean[n] = '\0';
    return find_rel(clean);
}

int xbox_iso_find(const char *host_path)
{
    return find_host(host_path);
}

int xbox_iso_is_dir(const char *host_path)
{
    int i = find_host(host_path);
    return i >= 0 ? s_iso.ent[i].isdir : 0;
}

uint64_t xbox_iso_size(const char *host_path)
{
    int i = find_host(host_path);
    return i >= 0 ? (uint64_t)s_iso.ent[i].size : 0;
}

int xbox_iso_open(const char *host_path)
{
    int i = find_host(host_path), h;

    if (i < 0 || s_iso.ent[i].isdir)
        return -1;
    for (h = 0; h < ISO_MAX_HANDLES; h++) {
        if (!s_iso.handle[h].used) {
            s_iso.handle[h].used  = 1;
            s_iso.handle[h].entry = i;
            return h;
        }
    }
    fprintf(stderr, "[ISO] out of file handles (%d in use)\n", ISO_MAX_HANDLES);
    return -1;
}

int64_t xbox_iso_pread(int handle, void *buf, size_t n, uint64_t off)
{
    iso_entry *e;

    if (handle < 0 || handle >= ISO_MAX_HANDLES || !s_iso.handle[handle].used)
        return -1;
    e = &s_iso.ent[s_iso.handle[handle].entry];
    if (off >= e->size)
        return 0;
    if (n > e->size - off)
        n = (size_t)(e->size - off);
    if (img_read(buf, n, e->off + off) != 0)
        return -1;
    return (int64_t)n;
}

uint64_t xbox_iso_handle_size(int handle)
{
    if (handle < 0 || handle >= ISO_MAX_HANDLES || !s_iso.handle[handle].used)
        return 0;
    return s_iso.ent[s_iso.handle[handle].entry].size;
}

void xbox_iso_close(int handle)
{
    if (handle >= 0 && handle < ISO_MAX_HANDLES)
        s_iso.handle[handle].used = 0;
}

/* --------------------------------------------------------- whole-file read */

/* The kernel path layer owns the mount point; kernel_iso.c only needs to ask
 * it where the game directory is. Declared here rather than by including
 * kernel.h, which this file compiles without on the host test. */
extern const char *xbox_game_dir(void);

int xbox_game_file_exists(const char *host_path)
{
    if (!host_path || !host_path[0])
        return 0;
    if (access(host_path, F_OK) == 0)
        return 1;
    return xbox_iso_ready(xbox_game_dir()) && xbox_iso_find(host_path) >= 0;
}

int xbox_read_game_file(const char *host_path, void **data, size_t *size)
{
    FILE *f;
    long n;
    unsigned char *buf;

    if (!host_path || !data || !size)
        return 0;
    f = fopen(host_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) {
            buf = (unsigned char *)malloc((size_t)n);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                fclose(f);
                *data = buf;
                *size = (size_t)n;
                return 1;
            }
            free(buf);
        }
        fclose(f);
    }

#if !defined(_WIN32)
    {
        int h;
        uint64_t len;

        if (!xbox_iso_ready(xbox_game_dir()))
            return 0;
        h = xbox_iso_open(host_path);
        if (h < 0)
            return 0;
        len = xbox_iso_handle_size(h);
        buf = (unsigned char *)malloc((size_t)len ? (size_t)len : 1);
        if (!buf || (len && xbox_iso_pread(h, buf, (size_t)len, 0) != (int64_t)len)) {
            free(buf);
            xbox_iso_close(h);
            return 0;
        }
        xbox_iso_close(h);
        *data = buf;
        *size = (size_t)len;
        return 1;
    }
#else
    return 0;
#endif
}

/* ------------------------------------------------------------- enumerate */

int xbox_iso_list(const char *host_dir, xbox_iso_ent_fn cb, void *user)
{
    const char *rel;
    char clean[1200], abs[1200];
    size_t i, n = 0, plen;
    int count = 0;

    if (!host_dir || !s_iso.state || !cb)
        return -1;
    host_dir = absolute(host_dir, abs, sizeof abs);
    if (!host_dir)
        return -1;
    rel = strip_prefix(host_dir);
    if (!rel)
        return -1;
    if (strlen(rel) >= sizeof clean)
        return -1;
    for (i = 0; rel[i]; i++) {
        if (rel[i] == '\\') {
            clean[n++] = '/';
        } else if (rel[i] == '/' && n > 0 && clean[n - 1] == '/') {
            continue;
        } else {
            clean[n++] = rel[i];
        }
    }
    clean[n] = '\0';

    plen = strlen(clean);
    for (i = 1; i < (size_t)s_iso.nent; i++) {      /* 0 is the root itself */
        const char *p = s_iso.ent[i].path;
        if (plen) {
            if (strncmp(p, clean, plen) != 0 || p[plen] != '/')
                continue;
            p += plen + 1;
        }
        if (!*p || strchr(p, '/'))
            continue;                               /* a child directory's file */
        cb(user, p, s_iso.ent[i].size, s_iso.ent[i].isdir);
        count++;
    }
    return count;
}
