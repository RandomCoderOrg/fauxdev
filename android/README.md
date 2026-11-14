# Android camera broker (development)

`libfauxdev-camera.so` is an Android AArch64 JNI library loaded into the app
process, which already runs Binder transactions. It opens a selected
`YUV_420_888` preview stream (preferring the rear camera and 640×480 for the
legacy default), drains its image queue continuously, converts the latest
image to RGB24, and replies only after writing it. It does not need a kernel
`/dev/video0`; the guest-facing path remains the Linux preload library.

Build on macOS with an installed Android NDK:

```sh
NDK_CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android31-clang" make -C android all test
```

Load the library from the Android app, call
`org.randomcoder.udroid.camera.CameraBrokerBridge.prepare()` synchronously
before dispatching a worker, then call `run(socketPath)` on that worker.
Call `stop()` when the camera service stops and join the worker. Preparation
lets stop cancel a pending run before the worker reaches JNI. The absolute
socket path must be in an app-private directory,
for example `/data/user/0/<package>/files/tmp/fauxdev-f3/camera.sock`. The
directory must already exist and be private. The broker accepts up to sixteen
same-UID clients at once. Only one may own the camera; other clients can query
inputs or inventory, while a second `CAMERA_START` returns `EBUSY`. It
checks the frame description and passed FD bounds, and reports camera failures
through protocol error replies. It owns the socket until `run()` stops.

The optional inventory request (protocol v1 opcode 3) takes one little-endian
32-bit byte offset and no FDs. After the normal HELLO, the broker builds one
read-only diagnostic snapshot per client connection and returns it in chunks:
little-endian total length and offset, followed by up to 60 KiB of text.
The snapshot is capped at 128 KiB, remains unchanged while that client is
connected, and is freed on disconnect. A malformed request gets an error;
the inventory does not change camera or V4L2 settings. Live camera availability
callbacks are not part of this snapshot.

The guest enumerates ordinary Camera2 `YUV_420_888` output sizes whose RGB24
frame fits the 64 MiB per-frame and 128 MiB aggregate MMAP limits. Modes come
from paged `CAMERA_MODES` requests (wire-v1 opcode 7); `CAMERA_START_V2`
(opcode 8) validates the selected input and dimensions before opening a
same-size image reader. A converted 320×240 mode remains available if the
camera does not advertise it natively. Older brokers return `ENOTTY` and the
guest keeps only 320×240 using the original `CAMERA_START` request. Android's
separate maximum-resolution pixel-mode configurations are not enumerated.
The broker snapshots the camera catalog when its service starts, so input
indexes stay stable for all connected processes; a camera that disappears
afterward fails at start rather than being reindexed.

Current brokers own the selected input, RGB24 size, and a generation number
globally. `G_INPUT`/`G_FMT` fetch that state; `S_INPUT`/`S_FMT` update it only
while no process owns the buffer queue. The first nonzero `REQBUFS` or
`CREATE_BUFS` acquires the queue lease, which survives `STREAMOFF` and ends on
`REQBUFS(0)`, close, or disconnect. A stale generation returns `EAGAIN` so
the caller can read the new setting and retry. Higher-priority connections
block lower-priority mutations. Older brokers still use local 320×240 state;
older clients connecting to a new broker cannot announce their queue until
stream start, so they lack cross-process pre-stream exclusion. Timed-out
requests disconnect the transport: the broker may have committed the request,
but a new authenticated connection can read its current state safely.
Optional rate queries expose only fixed Camera2 AE ranges whose target period
fits the selected YUV source's minimum frame duration. `ENUM_FRAMEINTERVALS`
lists these as discrete 1/fps values; `G/S_PARM` reads or selects the
broker-global target and resets to nominal on a zero numerator or denominator.
Changing it requires no allocated buffers. The target is applied to both
session parameters and the repeating request before capture starts; a rejected
setting fails `STREAMON`. Older brokers return `ENOTTY`. Actual delivery can
be slower than the sensor's target, and V4L2 timestamps are still assigned on
delivery rather than at sensor capture. Android's variable AE ranges are not
advertised as fixed intervals.

The host app must grant `CAMERA` permission and start a camera foreground
service from a visible activity before starting the broker. Camera use
must stop when that service stops. Android's [camera foreground-service
restrictions](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)
apply. Starting the preview and waiting for its first twelve frames can take up to fifteen seconds; the guest
transport's `VIDEO_FRAME` receive timeout must exceed that bound.
`STREAMOFF` releases the active camera while retaining its authenticated
connection, so the same guest descriptor can select another input and restart.
It may wait up to 20 seconds for an in-flight `VIDEO_FRAME` request to finish.
Closing a guest descriptor stops its stream and releases its queue lease; other
handles in the same process retain the shared transport. If that connection
disconnects, the broker releases the camera after closing it.
For older protocol-v1 clients, the first valid `VIDEO_FRAME` implicitly opens
the default camera. This compatibility path is disabled once that client sends
`CAMERA_START` or `CAMERA_STOP`.

For a two-client regression on a Pixel, install the matching debug APK, start
the camera service in the app, and build `broker-client` inside native Termux
with `make -C fauxdev/android broker-client`. In two Termux tabs, run:

```sh
# Tab A: leave an idle authenticated client connected.
camera2termux-run ./fauxdev/android/broker-client idle 15000
# Tab B while A is idle: must print QUERY_OK.
camera2termux-run ./fauxdev/android/broker-client query

# Tab A: hold the camera; must print ACTIVE, then STOPPED.
camera2termux-run ./fauxdev/android/broker-client active 20000
# Tab B while A is active: query succeeds; START reports BUSY_OK.
camera2termux-run ./fauxdev/android/broker-client query
camera2termux-run ./fauxdev/android/broker-client busy
# After A stops, B can run `active 1000` and receive ACTIVE/STOPPED.
```

Run `broker-client abrupt` followed by `active 1000` to check disconnect
release. Run `broker-client wait-stop`, tap **Stop camera** in the app while it
prints ACTIVE, and check for DISCONNECTED; start the service again and run
`broker-client query` to check shutdown and restart. These are live protocol
checks, not part of the host `make test` target.

For a cross-process V4L2 configuration check in Termux, compile
`tests/camera_global_live_client.c` with `cc` and launch each command through
`camera2termux-run`. While `hold` prints `LEASED`, a second process running
`set 640` must fail with `EBUSY`; after `RELEASED`, `set 640` succeeds and a
third process running `query` reports `640x480`. `set 320` restores the
default. While `priority` prints `PRIORITY`, another process at default
priority cannot run `set 640`; it succeeds after the priority holder exits.
The synthetic two-handle/lease/generation test runs under `make test`.

This is a capture proof path, not a full V4L2 camera implementation. It uses
nearest-neighbor scaling for converted 320×240 and approximate BT.601 full-range YUV conversion;
orientation and precise color metadata are not yet handled. The Android NDK
[camera](https://developer.android.com/ndk/reference/group/camera) and
[image-reader](https://developer.android.com/ndk/reference/group/media) APIs
provide the source frames and per-plane strides.
