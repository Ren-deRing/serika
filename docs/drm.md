# DRM and KMS

`/dev/dri/card0` (226:0) is the boot's framebuffer as a drm device: `drivers/builtin/drm/`. It calls itself `vkms`, the
virtual driver of linux, which it is, and the name mesa takes to need no gpu (`kms_swrast`, llvmpipe). There is no
render node.

- `drm.c`: the device, the ioctl switch, the capabilities, events (`read`, `poll`)
- `kms.c`: the display objects, framebuffers, the crtc, flips and the vblank
- `gem.c`: buffers, handles, `MAP_DUMB`, dma-bufs
- `drm_uapi.h`: the structs of libdrm's `drm.h` and `drm_mode.h` as they are, with the sizes asserted

## Per open file

`struct file` has a `priv`, and an inode's ops can have `open_file`, `release_file`, `ioctl_file` and `poll_file`,
which take the file. The drm device keeps its handles, its events and the universal planes bit in them. Handles are
numbers of a file, as in linux; a buffer exists until its last handle, framebuffer, dma-buf and the frames that
a mapping holds are gone.

## Objects

One of each: crtc 33, encoder 34, connector `Virtual-1` 35 (always connected) and the primary plane 36. The mode is the
boot's, the only one. The plane is listed only to a file that set `UNIVERSAL_PLANES`. The formats are `XRGB8888`
and `ARGB8888` (the alpha is not used), linear. Properties: the connector's `DPMS` and the plane's `type`. There is
no cursor plane (`CURSOR` is `ENXIO`: the compositor draws the cursor) and no overlay.

Atomic is not here: `CLIENT_CAP_ATOMIC` is refused and a program goes on with the legacy calls (`SETCRTC`,
`PAGE_FLIP`, `DIRTYFB`). wlroots does so by itself.

## Buffers

Dumb buffers only, `CREATE_DUMB` of 8, 16, 24 and 32 bits (a framebuffer needs 32), up to 256 MiB. They are pages that
are not contiguous. `MAP_DUMB` gives an offset in the device file, from a counter that starts at 4 GiB and never reuses
a range, and `mmap` of the file at it maps the pages (`drm_getpage()` finds the buffer by that offset). `PRIME` exports
a handle as a dma-buf, a file on an inode of its own that maps the same pages and answers `lseek(SEEK_END)` with the
size, and imports one of ours (not another driver's) as a handle: the file's own if it has one, else a new one.

## Showing

Nothing scans out, so showing is copying: `SETCRTC`, `PAGE_FLIP` and `DIRTYFB` copy the buffer, from `(x, y)` and as
much as the mode needs, into the framebuffer (xrgb8888 is required of it). `PAGE_FLIP` copies at once and the event
comes at the next vblank: a one shot timer set to the next multiple of 1/60 s, so a program that waits for the event
before it draws the next frame runs at 60. One flip may wait at a time (`EBUSY`). The event
(`DRM_EVENT_FLIP_COMPLETE`, a `drm_event_vblank` with the time on the monotonic clock) is queued on the file that
asked, which `read`s it or `poll`s for it. The timer's function takes no lock but the file's event lock; closing a
file takes its flip off with `timer_del()`.

A framebuffer taken off while shown leaves the crtc without one, as in linux; what was on the screen stays.

The framebuffer is write combining memory, slow to write and not to be read: a shadow in ram is what the screen has,
and a copy writes only the 64 byte blocks that differ from it. a mode set or dpms makes it stale, and that copy writes
everything.

## Not here

Atomic, cursor and overlay planes, `IN_FORMATS` and modifiers, other modes and outputs, `WAIT_VBLANK`, explicit sync
(syncobjs), importing a dma-buf of another driver, gamma, and any sysfs: libdrm's device listing
(`drmGetDevices2`) and mesa's loader look at `/sys/dev/char/226:0` (mesa says "failed to retrieve device information"
and goes on).

No sync of any kind, so a buffer is shown as it is when the flip comes. A compositor that draws with llvmpipe (gles2)
flips before llvmpipe's threads are done with the frame, which then shows torn until the next flip: llvmpipe waits for
them only when it is asked for a fence fd (explicit sync), and that needs atomic, syncobjs, `IN_FENCE_FD`, `/dev/udmabuf`
and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`. Compositors here draw with pixman, as wlroots and weston do on their own where
there is no gpu.

## Tests

`exec drm` (`user/glibc/drmtest.c`): libdrm against the device: version, capabilities, objects, properties, a buffer
shown (compared with `/dev/fb0`), flips and their events (30 in about half a second), dma-bufs and a second opening,
errors. `exec gbm` (`user/glibc/gbmtest.c`): gbm, EGL and OpenGL ES 2 on llvmpipe drawing the buffers of a gbm surface,
shown and flipped.
