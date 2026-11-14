# fauxdev

`fauxdev` provides reusable userspace foundations for exposing Linux device-like
interfaces without a kernel driver.

The first target is a rootless video capture device backed by an external
camera broker. The core remains independent of cameras, Android, V4L2, and any
specific device vendor.

Development starts with the lifecycle and interception contract in
[`docs/dummy-device-contract.md`](docs/dummy-device-contract.md).

## Status

The synthetic `/dev/fauxdev-probe` backend now validates path interception,
device metadata, descriptor lifetime, duplication, readiness, bulk close, fork
isolation, and concurrent sessions. It is a test interface, not a camera or
V4L2 device.

An isolated broker-transport microprobe validates framed `SOCK_SEQPACKET`
requests, file descriptors in both directions, sealed `memfd` mappings,
backend error replies, and disconnect notification through a test `eventfd`.
The probe device is not yet wired to this transport.

`/dev/video0` is a separate V4L2 RGB24 capture source with MMAP buffers
(up to 64 slots within a 128 MiB aggregate budget). Without a broker it
produces deterministic 320×240 test frames. Setting `FAUXDEV_VIDEO_SOCKET` connects it to a broker
that writes into per-request staging FDs. An
[Android Camera2 broker](android/README.md) runs
inside a camera-authorized foreground app; the test broker remains synthetic.
Queueing is asynchronous, and blocking dequeue waits for completion or
disconnect. A visible `/dev/video0` directory entry still needs a
deployment-created placeholder.

The read-only `fauxdev-camera-info` command reports Camera2 metadata through
the same broker connection. Build it with `make build/fauxdev-camera-info`
(use `CC=clang` in native Termux), then run it through the app's FD launcher,
for example `camera2termux-run /path/to/fauxdev-camera-info`. It accepts the
inherited `FAUXDEV_VIDEO_FD`; the older same-UID socket path also works through
`FAUXDEV_VIDEO_SOCKET`. The report describes device metadata, not implemented
V4L2 modes or controls. It does not open another camera; the broker must be
running, but the camera stays closed until streaming starts.

With a current broker, `/dev/video0` enumerates compatible Camera2 inputs and
their ordinary `YUV_420_888` output sizes that fit a 64 MiB RGB24 frame.
For each size, fixed AE target rates that fit its actual YUV source's minimum
frame duration appear through `ENUM_FRAMEINTERVALS` and global `G/S_PARM`.
These are camera targets, not a promise that RGB conversion and Termux delivery
will match the sensor cadence.
The broker owns input and format configuration across processes; allocating
buffers leases the queue until they are released or the process disconnects.
`VIDIOC_S_INPUT` selects one before buffers are allocated; `VIDIOC_STREAMON`
opens it and reports an open failure to the caller. `VIDIOC_STREAMOFF` releases
the camera while retaining the connection for another selection/stream. It may
wait up to 20 seconds for an in-flight frame request to finish. The
output is the selected RGB24 size (or a converted 320×240 fallback).
`build/camera-selection-client` exercises each
input on one descriptor and prints a frame hash; use it through
`camera2termux-run` for a device check. Pass an optional output prefix (for
example `/sdcard/Download/camera`) to save `camera-0.ppm`, `camera-1.ppm`, etc.
for visual inspection. Without the prefix, it writes no files. A listed input
can become unavailable before it is opened.

Older protocol-v1 clients that send `HELLO` followed directly by a valid
`VIDEO_FRAME` still open the default camera on that first frame. Once a client
sends `CAMERA_START` or `CAMERA_STOP`, it uses the explicit lifecycle; frames
after a stop require another start. `build/legacy-broker-client` checks the old
request sequence against a running broker through `camera2termux-run`.

Run the acceptance suite on Linux with:

```sh
make test
```

`fauxdev` is Linux-specific; build it in a Linux container or VM when working
from macOS.

On a Pixel 6a, all 58 advertised rear/front modes produced a real frame,
and the pinned live capture test completed 84/84 checks with no warnings.
This does not establish full V4L2 support or compatibility on other devices.
The older [Pixel validation](docs/pixel-proot-validation.md) records the
earlier FFplay/PRoot experiment and its latency limits.
