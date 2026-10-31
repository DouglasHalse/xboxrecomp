/* A disc image mounted read-only underneath the game directory.
 *
 * A bundled APK carries the game's files inside itself, so nothing is
 * extracted to the device: the launcher opens the payload's descriptors and
 * leaves them in <game>/.disc.mount, and every path the guest opens that is
 * not on the host filesystem is served from the image instead.
 *
 * The image is one logical XDVDFS disc split into pieces (an APK entry is
 * addressed with the zip format's 32-bit offsets). It is parsed once, into a
 * flat path index, so a lookup costs a string compare rather than a B-tree
 * walk.
 */
#ifndef XBOX_KERNEL_ISO_H
#define XBOX_KERNEL_ISO_H

#include <stddef.h>
#include <stdint.h>

/* Load <game_dir>/.disc.mount if it exists. Idempotent and cheap after the
 * first call, so every file entry point may call it. Returns 1 when an image
 * is mounted. */
int xbox_iso_ready(const char *game_dir);

/* Resolve a host path (as produced by kernel_path.c, i.e. <game_dir>/...) to
 * an entry in the image. -1 when it is not there. */
int xbox_iso_find(const char *host_path);

int      xbox_iso_is_dir(const char *host_path);
uint64_t xbox_iso_size(const char *host_path);

/* Open for reading. Returns a handle >= 0, or -1 when the path is not in the
 * image (or names a directory). */
int  xbox_iso_open(const char *host_path);
int64_t xbox_iso_pread(int handle, void *buf, size_t n, uint64_t off);
void xbox_iso_close(int handle);
uint64_t xbox_iso_handle_size(int handle);

/* 1 when the file is on the host filesystem or in the mounted image. */
int xbox_game_file_exists(const char *host_path);

/* Read a file that belongs to the game into memory: the host's copy first,
 * the image second, so a title can load its own XBE (or any other asset) the
 * same way in an imported tree and in a bundled build. Returns 1 with a
 * malloc'd buffer the caller frees, 0 when neither has the file. */
int xbox_read_game_file(const char *host_path, void **data, size_t *size);

/* Directory listing, for NtQueryDirectoryFile. Calls back once per entry in
 * the image directory named by host_path, with the bare name. Returns the
 * count, or -1 when that directory is not in the image. */
typedef void (*xbox_iso_ent_fn)(void *user, const char *name, uint64_t size, int isdir);
int xbox_iso_list(const char *host_dir, xbox_iso_ent_fn cb, void *user);

#endif /* XBOX_KERNEL_ISO_H */
