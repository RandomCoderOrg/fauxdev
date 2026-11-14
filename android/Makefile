NDK_CC ?= aarch64-linux-android31-clang
HOST_CC ?= cc
TERMUX_CC ?= cc
CFLAGS := -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror

.PHONY: all test clean

all: libfauxdev-camera.so

libfauxdev-camera.so: camera_broker.c yuv_rgb.c yuv_rgb.h ../src/transport.c ../src/transport.h ../include/fauxdev/protocol.h
	$(NDK_CC) $(CFLAGS) -fPIC -shared -I../include -I../src camera_broker.c yuv_rgb.c ../src/transport.c -o $@ -lcamera2ndk -lmediandk -landroid -pthread

test-yuv-rgb: test_yuv_rgb.c yuv_rgb.c yuv_rgb.h
	$(HOST_CC) $(CFLAGS) test_yuv_rgb.c yuv_rgb.c -o $@

test: test-yuv-rgb
	./test-yuv-rgb

# Build inside native Termux to exercise the app's authenticated FD handoff.
broker-client: tests/broker_client.c ../src/transport.c ../src/transport.h ../include/fauxdev/protocol.h
	$(TERMUX_CC) $(CFLAGS) -I../include -I../src tests/broker_client.c ../src/transport.c -o $@ -pthread

clean:
	rm -f libfauxdev-camera.so test-yuv-rgb broker-client
