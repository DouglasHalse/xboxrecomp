# Disc-image overlay: byte-for-byte check against the extracted tree

Exercises `src/kernel/kernel_iso.c` — the layer a bundled APK uses to read its
game files out of the disc image it carries — without a device.

It mounts the real image **in two pieces split at 1 GiB**, so every read that
crosses the seam exercises the stitching, then walks the image and compares
each file with its counterpart in the tree the image was extracted to. It also
checks that a lookup ignores case, that a relative path (`game/default.xbe`,
which is how the ports probe their own files) resolves against the working
directory, and that a path outside the mount point does not resolve.

```
tests/iso_image/run.bat "C:\path\SSX 3 (Europe).xiso.iso" C:\path\to\game
```

Needs MSVC (this is where the rip is; the same file is compiled by the Android
build) and writes its scratch directory under `%TEMP%`. An image path with
spaces and parentheses trips cmd's own argument parsing, so quote it or, from a
shell that quotes properly (`bash`, PowerShell), call the built exe directly:

```
iso_test <scratch>/mount <tree> /full "<image>"
``` Expected tail:

```
[ISO] 139 file(s) mounted from 2 part(s), 2180 MB, at .../mount
121 file(s), 2285258804 byte(s) compared, 0 bad
```

`121` is the 120 files under `data/` plus `default.xbe`; the byte count is the
tree's total, so a short read anywhere shows up as a mismatch rather than as a
plausible-looking success.
