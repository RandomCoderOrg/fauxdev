# Android camera broker (development)

`libfauxdev-camera.so` is an Android AArch64 JNI library loaded into the app
process, which already runs Binder transactions. It opens a supported
`YUV_420_888` preview stream (preferring the rear camera and 640×480), drains
its image queue continuously, converts the latest image to a dev-only 320×240
RGB24 buffer, and replies only after writing it. It does not need a kernel
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
directory must already exist and be private. The broker accepts only same-UID clients,
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

The host app must grant `CAMERA` permission and start a camera foreground
service from a visible activity before starting the broker. Camera use
must stop when that service stops. Android's [camera foreground-service
restrictions](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)
apply. Starting the preview and waiting for its first twelve frames can take up to fifteen seconds; the guest
transport's `VIDEO_FRAME` receive timeout must exceed that bound.
`STREAMOFF` releases the active camera while retaining its authenticated
connection, so the same guest descriptor can select another input and restart.
It may wait up to 20 seconds for an in-flight `VIDEO_FRAME` request to finish.
Closing the guest descriptor cancels the transport; the broker checks for peer
disconnect while waiting for a frame so another client can connect promptly.
For older protocol-v1 clients, the first valid `VIDEO_FRAME` implicitly opens
the default camera. This compatibility path is disabled once that client sends
`CAMERA_START` or `CAMERA_STOP`.

This is a low-resolution camera proof path, not a full V4L2 camera implementation. It uses a
nearest-neighbor downscale and approximate BT.601 full-range YUV conversion;
orientation and precise color metadata are not yet handled. The Android NDK
[camera](https://developer.android.com/ndk/reference/group/camera) and
[image-reader](https://developer.android.com/ndk/reference/group/media) APIs
provide the source frames and per-plane strides.
