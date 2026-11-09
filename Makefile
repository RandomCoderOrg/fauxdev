CC ?= cc
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -fPIC -Wall -Wextra -Werror
LDFLAGS += -pthread

BUILD := build
LIB := $(BUILD)/libfauxdev.so
TEST := $(BUILD)/probe-client
TRANSPORT_TEST := $(BUILD)/transport-client
VIDEO_TEST := $(BUILD)/video-client
VIDEO_BROKER_TEST := $(BUILD)/video-broker-client

.PHONY: all test clean

all: $(LIB) $(TEST) $(TRANSPORT_TEST) $(VIDEO_TEST) $(VIDEO_BROKER_TEST)

$(BUILD):
	mkdir -p $@

$(LIB): src/fauxdev.c src/probe_backend.c src/video_backend.c src/transport.c src/internal.h src/transport.h include/fauxdev/probe.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared src/fauxdev.c src/probe_backend.c src/video_backend.c src/transport.c -o $@ $(LDFLAGS)

$(TEST): tests/probe_client.c include/fauxdev/probe.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/probe_client.c -o $@ $(LDFLAGS)

$(TRANSPORT_TEST): tests/transport_client.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/transport_client.c src/transport.c -o $@ $(LDFLAGS)

$(VIDEO_TEST): tests/video_client.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/video_client.c -o $@ $(LDFLAGS)

$(VIDEO_BROKER_TEST): tests/video_broker_client.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/video_broker_client.c src/transport.c -o $@ $(LDFLAGS)

test: all
	LD_PRELOAD=$(abspath $(LIB)) $(TEST)
	$(TRANSPORT_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(VIDEO_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(VIDEO_BROKER_TEST)

clean:
	rm -rf $(BUILD)
