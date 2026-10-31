/* Host test for the disc-image overlay (src/kernel/kernel_iso.c).
 *
 * Builds on Windows only because that is where the rip is; the parser and the
 * piece stitching are the same code the Android build compiles. It mounts a
 * real image split at a sector boundary (so reads cross the seam between
 * pieces), then walks the image and compares every file byte for byte with
 * the extracted tree it was made from.
 *
 *   iso_test <mount-dir> <tree> [/full] [image]
 *
 * <mount-dir> is a scratch directory this writes .disc.mount into. The image is
 * the fourth argument when given (then it is remembered in
 * <mount-dir>/image.txt), otherwise the first line of that file is used.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../../src/kernel/kernel_iso.h"

#define SEAM (1024ull * 1024ull * 1024ull)   /* 1 GiB, a multiple of 2048 */

static char s_mount[512];
static char s_tree[512];

/* kernel_iso.c asks the path layer where the game directory is; for this test
 * the mount point is that directory. */
const char *xbox_game_dir(void);
static long long s_files, s_bytes, s_bad;

const char *xbox_game_dir(void)
{
    return s_mount;
}
static int s_full;

static int slurp(const char *path, unsigned char **out, long long *len)
{
    FILE *f = fopen(path, "rb");
    long long n;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = _ftelli64(f);
    fseek(f, 0, SEEK_SET);
    *out = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
    if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(*out);
        return -1;
    }
    fclose(f);
    *len = n;
    return 0;
}

/* Compare one image file with its extracted counterpart by streaming both. */
static void check_file(const char *rel)
{
    char host[1024], real[1024];
    int h;
    unsigned char *want;
    long long want_len, off = 0;
    static unsigned char buf[1 << 20];   /* not on the stack: 1 MB overflows it */

    snprintf(host, sizeof host, "%s/%s", s_mount, rel);
    h = xbox_iso_open(host);
    if (h < 0) {
        printf("MISSING in image: %s\n", rel);
        s_bad++;
        return;
    }
    snprintf(real, sizeof real, "%s/%s", s_tree, rel);
    if (slurp(real, &want, &want_len) != 0) {
        printf("no extracted file for %s\n", rel);
        s_bad++;
        xbox_iso_close(h);
        return;
    }
    if ((long long)xbox_iso_handle_size(h) != want_len) {
        printf("SIZE %s: image %llu, tree %lld\n", rel,
               (unsigned long long)xbox_iso_handle_size(h), want_len);
        s_bad++;
        xbox_iso_close(h);
        free(want);
        return;
    }
    while (off < want_len) {
        size_t want_n = want_len - off < (long long)sizeof buf ? (size_t)(want_len - off)
                                                               : sizeof buf;
        int64_t got = xbox_iso_pread(h, buf, want_n, (uint64_t)off);
        if (got != (int64_t)want_n || memcmp(buf, want + off, want_n) != 0) {
            printf("BYTES %s at %lld\n", rel, off);
            s_bad++;
            break;
        }
        off += (long long)want_n;
    }
    xbox_iso_close(h);
    free(want);
    s_files++;
    s_bytes += want_len;
}

typedef struct {
    char  dir[1024];
    int   depth;
} walk_state;

static void walk(const char *rel);

static void ent_cb(void *user, const char *name, uint64_t size, int isdir)
{
    walk_state *st = (walk_state *)user;
    char rel[1024];

    if (*st->dir)
        snprintf(rel, sizeof rel, "%s/%s", st->dir, name);
    else
        snprintf(rel, sizeof rel, "%s", name);
    if (isdir)
        walk(rel);
    else
        check_file(rel);
    (void)size;
}

static void walk(const char *rel)
{
    walk_state st;
    char host[1024];

    if (strlen(rel) >= sizeof st.dir)
        return;
    snprintf(st.dir, sizeof st.dir, "%s", rel);
    st.depth = 0;
    snprintf(host, sizeof host, "%s/%s", s_mount, rel);
    if (xbox_iso_list(host, ent_cb, &st) < 0)
        printf("not a directory in the image: %s\n", rel);
}

int main(int argc, char **argv)
{
    char path[1024], line[1024];
    FILE *f;
    int fd, fd2;
    long long image_size;

    if (argc < 3) {
        printf("usage: iso_test <mount-dir> <tree> [/full]\n");
        return 2;
    }
    snprintf(s_mount, sizeof s_mount, "%s", argv[1]);
    snprintf(s_tree, sizeof s_tree, "%s", argv[2]);
    s_full = argc > 3 && !strcmp(argv[3], "/full");

    snprintf(path, sizeof path, "%s/image.txt", s_mount);
    if (argc > 4 && argv[4][0]) {
        f = fopen(path, "w");
        if (!f) {
            printf("cannot write %s\n", path);
            return 2;
        }
        fprintf(f, "%s\n", argv[4]);
        fclose(f);
        snprintf(line, sizeof line, "%s", argv[4]);
    } else {
        f = fopen(path, "r");
        if (!f) {
            printf("cannot read %s (pass the image as the fourth argument)\n", path);
            return 2;
        }
        if (!fgets(line, sizeof line, f)) {
            fclose(f);
            return 2;
        }
        fclose(f);
        line[strcspn(line, "\r\n")] = '\0';
    }

    fd = _open(line, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        printf("cannot open the image: %s\n", line);
        return 2;
    }
    image_size = _lseeki64(fd, 0, SEEK_END);
    fd2 = _open(line, _O_RDONLY | _O_BINARY);
    if (fd2 < 0 || image_size <= (long long)SEAM) {
        printf("image too small to split\n");
        return 2;
    }

    /* Two pieces of the same file: everything below the seam comes from one
     * descriptor, the rest from the other, so every read that crosses 1 GiB
     * exercises the stitching. */
    snprintf(path, sizeof path, "%s/.disc.mount", s_mount);
    f = fopen(path, "w");
    if (!f) {
        printf("cannot write %s\n", path);
        return 2;
    }
    fprintf(f, "# written by tests/iso_image\n");
    fprintf(f, "PREFIX=%s\n", s_mount);
    fprintf(f, "PART=%d 0 %llu\n", fd, (unsigned long long)SEAM);
    fprintf(f, "PART=%d %llu %llu\n", fd2, (unsigned long long)SEAM,
            (unsigned long long)(image_size - (long long)SEAM));
    fclose(f);

    if (!xbox_iso_ready(s_mount)) {
        printf("mount failed\n");
        return 1;
    }

    /* A port probes its own files relatively ("game/default.xbe"); the lookup
     * has to resolve that against the working directory, not the mount point. */
    {
        char parent[1024], leaf[512], rel[1200];
        const char *slash;
        snprintf(parent, sizeof parent, "%s", s_mount);
        slash = strrchr(parent, '/');
        if (slash) {
            snprintf(leaf, sizeof leaf, "%s", slash + 1);
            *((char *)slash) = '\0';
            _chdir(parent);
            snprintf(rel, sizeof rel, "%s/default.xbe", leaf);
            if (xbox_iso_find(rel) < 0)
                printf("FAIL: a relative path did not resolve\n");
            if (!xbox_game_file_exists(rel))
                printf("FAIL: the image's file was not reported as existing\n");
            _chdir(s_mount);
        }
    }

    /* Case does not survive the guest's FATX habits, so lookup ignores it. */
    {
        char probe[1024];
        snprintf(probe, sizeof probe, "%s/DEFAULT.XBE", s_mount);
        if (xbox_iso_find(probe) < 0)
            printf("FAIL: lookup is case-sensitive\n");
        snprintf(probe, sizeof probe, "%s/../outside", s_mount);
        if (xbox_iso_find(probe) >= 0)
            printf("FAIL: a path outside the mount point resolved\n");
    }

    if (s_full)
        walk("");
    else
        check_file("default.xbe");

    printf("%lld file(s), %lld byte(s) compared, %lld bad\n",
           s_files, s_bytes, s_bad);
    return s_bad ? 1 : 0;
}
