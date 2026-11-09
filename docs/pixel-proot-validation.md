# Pixel 6a / uDroid Dev PRoot validation

On 2026-11-09, `libfauxdev.so` and `video-client` built for Linux AArch64 ran
inside uDroid Dev's Noble rootfs through its packaged PRoot. The unmodified
`video-client` passed its V4L2 query, MMAP, queue, stream, poll, dequeue,
pixel-content, duplicate-FD, and cleanup assertions.

The test used `LD_PRELOAD=/tmp/fauxdev-f3/libfauxdev.so` and a temporary PRoot
bind from the app-private test directory to `/tmp/fauxdev-f3`. This did not
modify the app's launcher or installed rootfs.

Without a placeholder, `ls -l /dev/video0` reported the intercepted character
device, but `ls -1 /dev` did not list `video0`. Binding an empty app-private
file to `/dev/video0` made directory enumeration include it; `video-client`
still passed with that bind. The placeholder supplies discoverability only;
the preload library implements the device behavior.

The initial synthetic fixture let FFmpeg 6.1 open the device and decode one
64×48 RGB24 frame (exit 0). It requires `VIDIOC_G_INPUT` and
`VIDIOC_ENUMINPUT`; both report the backend's single input. `VIDIOC_G_PARM`
remains unsupported, so FFmpeg reports an unknown frame interval. That initial
test did not establish a usable live frame rate. A separate `-f framecrc -` run
emitted one 9,216-byte frame with checksum `0x6ec36aa5` (exit 0).

With `FAUXDEV_VIDEO_SOCKET` set, a separate broker fixture received the frame
buffer FD, wrote 9,216 bytes into it, and the Pixel guest read the broker's
pixels through `/dev/video0`. The test also verified that broker disconnect
wakes the device and makes dequeue return `EPIPE` (exit 0). This is still a
synthetic broker, not Android camera capture.

The generic `probe-client` did not pass in this PRoot: its `/dev/null` passthrough
assertion expected a character device, while this guest's `/dev/null` metadata
did not meet that expectation. The V4L2 client completed independently.

With the asynchronous queue/cancellation path and opt-in Android Camera2 broker
in uDroid Dev's camera foreground service, Pixel QA captured 180 FFmpeg frames
in about 19.4 seconds; all 180 `framemd5` hashes differed. FFplay displayed a
live feed in XFCE for 90 seconds (about 1,300 frames, exit 0). The broker
service then stopped and its socket was removed.

In the default FFplay live-input run, its `vq` reached about 180 KB (roughly
20 frames at 9 KiB each). With the same guest preload and binds, the following
invocation held `vq` around 9–18 KB, and the user reported that the feed felt
faster:

```sh
ffplay -f video4linux2 -noinfbuf -fflags nobuffer -flags low_delay \
  -probesize 32 -analyzeduration 0 -max_delay 0 -sync ext -framedrop \
  -i /dev/video0
```

This isolates player buffering as a major contributor to perceived delay; it
does not measure the complete sensor-to-screen latency.

In a separate dev-only 320×240 RGB24 run, FFmpeg detected that format and
reported `14.92 tbr`. It captured 60 distinct `framemd5` hashes in 6.04
seconds of wall time, including startup. The low-buffer FFplay viewer visibly
rendered readable keyboard labels in a 640×480 window. The planned 120-second
`timeout` ended it with exit 124; the broker then stopped and its socket was
removed. `vq` was mostly 0–225 KB (about 0–1 frame), briefly reaching
450–675 KB (2–3 frames) near the end.
The reported `tbr` and queue depth do not measure sensor or display FPS.

Both resolutions still need manual `LD_PRELOAD`, socket and `/dev/video0`
placeholder binds. These results do not establish general V4L2 application
compatibility, other formats, or other devices.
