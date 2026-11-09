#ifndef FAUXDEV_PROBE_H
#define FAUXDEV_PROBE_H

#include <stdint.h>
#include <sys/ioctl.h>

#define FAUXDEV_PROBE_PATH "/dev/fauxdev-probe"
#define FAUXDEV_PROBE_VERSION 1u

#define FAUXDEV_PROBE_GET_VERSION _IOR('F', 0x00, uint32_t)
#define FAUXDEV_PROBE_SIGNAL _IOW('F', 0x01, uint64_t)
#define FAUXDEV_PROBE_CONSUME _IOR('F', 0x02, uint64_t)

#endif
