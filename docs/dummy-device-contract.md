# Dummy device contract

## Scope

The foundation provides process-local device emulation for dynamically linked
Linux applications. A registered path is opened as a synthetic file descriptor,
while device operations are dispatched to a userspace backend.

The foundation does not define V4L2, Android camera access, frame formats, or
hardware-buffer transport. Those belong to later backends.

## Kernel objects

| Object | Application visibility | Responsibility |
| --- | --- | --- |
| Placeholder device path | Visible | Makes directory scans discover the device |
| `eventfd` | Returned by `open` | Blocking and readiness notification |
| `SOCK_SEQPACKET` control connection | Hidden | Broker commands, responses, errors, and file-descriptor passing |
| `memfd` mappings | Exposed through `mmap` | Shared backend data |
| Session registry | Hidden | Associates descriptor aliases with one backend session |

`eventfd` is a readiness primitive only. It never carries device payloads,
metadata, or control messages.

## Broker transport

The broker uses a pathname-based `AF_UNIX` `SOCK_SEQPACKET` socket. Each packet
contains one fixed-width little-endian header, at most 64 KiB of payload, and at
most four descriptors carried with `SCM_RIGHTS`.

- Request IDs bind every response to exactly one request.
- Received descriptors are close-on-exec and become the receiver's property.
- Malformed, truncated, mismatched, or out-of-version packets terminate the
  connection.
- Backend error responses fail only that request; transport errors are sticky.
- A broker that does not reply fails a control request after at most two seconds
  of receive inactivity, or a `VIDEO_FRAME` request after twenty seconds; the
  connection becomes terminal.
- Broker disconnect writes one token to the device readiness `eventfd`. The
  token remains readable while later operations return `EPIPE`.
- Device data uses `memfd` descriptors; writable video staging buffers are
  distinct from client-mapped buffers. Device-specific `mmap` translation
  remains a backend responsibility.

The optional `/dev/video0` broker path sends `VIDEO_FRAME` with a per-request
staging FD and a fixed-width little-endian description (offset, dimensions,
stride, RGB24 format, byte count). After the broker replies, the producer
copies the frame into the client-mapped buffer only if that stream is still
active, then marks it ready. A late write to a cancelled request cannot
overwrite a restarted stream's mapped buffer. `QBUF` and `STREAMON` only queue
work; `STREAMOFF` cancels an in-flight request, and a later stream reconnects.

The broker socket must live in a private directory and use mode `0600`. The
broker must validate clients with `SO_PEERCRED`; abstract sockets are not used
because they do not provide filesystem permission checks.

## Interception boundary

### Foundation

| Operation | Required behavior |
| --- | --- |
| `open`, `open64`, `openat`, `openat64` and fortified aliases | Match registered paths and create a session |
| `stat`, `lstat`, `fstat`, `fstatat`, `statx` | Report the configured character-device identity |
| `close`, `close_range`, `closefrom` | Remove aliases and tear down the final reference once |
| `dup`, `dup2`, `dup3`, `fcntl` duplication commands | Attach aliases to the same session |
| `fcntl` status commands | Preserve shared `O_NONBLOCK` behavior and close-on-exec policy |
| `ioctl` | Dispatch dummy descriptors; forward all other descriptors unchanged |
| `read`, `readv`, `pread*`, `write`, `writev`, `pwrite*` | Reject unsupported dummy-device I/O without consuming readiness tokens |
| `pthread_atfork` child handler | Close inherited dummy sessions in the child |

### Backend-dependent

| Operation | Required when used |
| --- | --- |
| `mmap`, `mmap64` | Translate backend offsets to shared `memfd` mappings |
| `munmap` | Release backend mapping ownership, when mappings are tracked |
| `mremap` | Reject or correctly update tracked mappings, when mappings are tracked |
| `v4l2_*` entry points | Cover applications that use libv4l rather than libc calls |

### Kernel-native

`poll`, `ppoll`, `select`, `pselect`, and `epoll` operate directly on the
synthetic descriptor's `eventfd` and are not interposed.

### Explicit limits

- Direct `syscall` and `io_uring` submissions bypass `LD_PRELOAD`.
- Secure execution may discard `LD_PRELOAD`.
- Automatic sysfs and udev discovery are separate compatibility layers.
- Dummy descriptors are close-on-exec and are not inherited across `exec`.
- Forked children discard inherited dummy-device sessions and may reopen them.

## Descriptor invariants

- Every successful open owns one backend-private session state object.
- Every descriptor alias references exactly one session.
- Duplicates share device state and file-status flags.
- Closing one alias does not stop a session while another alias remains.
- Closing the final alias releases backend state exactly once, after in-flight
  operations finish.
- Bulk close operations cannot leave stale registry entries.
- Operations on unrelated descriptors are forwarded without modification.
- Broker disconnect wakes blocked clients before operations begin returning a
  stable terminal error.

## Readiness invariants

- A backend publishes one token only when one operation becomes consumable.
- Consuming that operation removes exactly one token.
- Dropped or replaced backend data does not create a token.
- Reset drains outstanding readiness.
- A terminal backend error is sticky and leaves the descriptor readable until
  the application closes it.

## Foundation acceptance tests

1. Real files and descriptors remain unaffected.
2. A registered probe path opens without a kernel device.
3. Path and descriptor metadata report the configured character device.
4. `poll`, `select`, and `epoll` block and wake correctly.
5. All descriptor duplication paths share one session.
6. The final close performs one teardown.
7. `close_range` cannot leave stale registrations.
8. A forked child cannot corrupt its parent's session.
9. Blocking and nonblocking operations return stable Linux errors.
10. Concurrent opens create independent sessions.
11. Broker disconnect wakes a blocked client and produces a terminal error.

The first implementation backend is `/dev/fauxdev-probe`. It only validates
this contract; it does not implement a camera or V4L2.

The `/dev/video0` backend exercises the single-planar V4L2 MMAP sequence with
either a fixed RGB24 test pattern or broker-supplied frames. It currently
supports blocking and nonblocking dequeue, but frame pacing, device controls,
and formats beyond 64×48 RGB24 remain outside this backend. The Android
Camera2 broker is an opt-in companion, not part of the Linux preload library.

## Transport acceptance tests

1. Header and ancillary descriptor counts must agree.
2. Descriptors pass in both directions with close-on-exec set.
3. A sealed `memfd` can be mapped read-only with unchanged contents.
4. A backend error response leaves the connection usable.
5. Peer disconnect wakes readiness and produces a stable `EPIPE`.

## References

- [`unix(7)`](https://man7.org/linux/man-pages/man7/unix.7.html)
- [`recvmsg(2)`](https://man7.org/linux/man-pages/man2/recvmsg.2.html)
- [`memfd_create(2)`](https://man7.org/linux/man-pages/man2/memfd_create.2.html)
- [`fcntl(2)` file seals](https://man7.org/linux/man-pages/man2/fcntl.2.html)
- [`eventfd(2)`](https://man7.org/linux/man-pages/man2/eventfd.2.html)
