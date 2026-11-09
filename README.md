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

`/dev/video0` is a separate V4L2 capture source with two MMAP
buffers. The current dev-only clarity experiment produces deterministic
320×240 RGB24 frames. Setting `FAUXDEV_VIDEO_SOCKET` connects it to a broker
that writes into per-request staging FDs. An
[Android Camera2 broker](android/README.md) runs
inside a camera-authorized foreground app; the test broker remains synthetic.
Queueing is asynchronous, and blocking dequeue waits for completion or
disconnect. A visible `/dev/video0` directory entry still needs a
deployment-created placeholder.

Run the acceptance suite on Linux with:

```sh
make test
```

`fauxdev` is Linux-specific; build it in a Linux container or VM when working
from macOS.

On a Pixel 6a, the earlier 64×48 build delivered 180 distinct FFmpeg frames
and a visible 90-second FFplay stream through uDroid Dev's PRoot. The new
320×240 experiment still needs device QA and is not general V4L2 support; see
[Pixel validation](docs/pixel-proot-validation.md) for the lower-buffering
FFplay command and its latency limits.
