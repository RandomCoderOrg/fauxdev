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
INVENTORY_TOOL := $(BUILD)/fauxdev-camera-info
INVENTORY_TEST := $(BUILD)/inventory-client-test
CAMERA_SELECT_TEST := $(BUILD)/camera-selection-client
CAMERA_SELECT_BROKER_TEST := $(BUILD)/camera-selection-broker-test
LEGACY_BROKER_TEST := $(BUILD)/legacy-broker-client
MULTIOPEN_TEST := $(BUILD)/video-multiopen-client
CAMERA_MODES_TEST := $(BUILD)/camera-modes-broker-test
CAMERA_GLOBAL_TEST := $(BUILD)/camera-global-broker-test

.PHONY: all test clean

all: $(LIB) $(TEST) $(TRANSPORT_TEST) $(VIDEO_TEST) $(VIDEO_BROKER_TEST) $(INVENTORY_TOOL) $(INVENTORY_TEST) $(CAMERA_SELECT_TEST) $(CAMERA_SELECT_BROKER_TEST) $(LEGACY_BROKER_TEST) $(MULTIOPEN_TEST) $(CAMERA_MODES_TEST) $(CAMERA_GLOBAL_TEST)

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

$(INVENTORY_TOOL): tools/camera_inventory.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tools/camera_inventory.c src/transport.c -o $@ $(LDFLAGS)

$(INVENTORY_TEST): tests/inventory_client_test.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/inventory_client_test.c src/transport.c -o $@ $(LDFLAGS)

$(CAMERA_SELECT_TEST): tests/camera_selection_client.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/camera_selection_client.c -o $@ $(LDFLAGS)

$(CAMERA_SELECT_BROKER_TEST): tests/camera_selection_broker_test.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/camera_selection_broker_test.c src/transport.c -o $@ $(LDFLAGS)

$(LEGACY_BROKER_TEST): tests/legacy_broker_client.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/legacy_broker_client.c src/transport.c -o $@ $(LDFLAGS)

$(MULTIOPEN_TEST): tests/video_multiopen_client.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/video_multiopen_client.c src/transport.c -o $@ $(LDFLAGS)

$(CAMERA_MODES_TEST): tests/camera_modes_broker_test.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/camera_modes_broker_test.c src/transport.c -o $@ $(LDFLAGS)

$(CAMERA_GLOBAL_TEST): tests/camera_global_broker_test.c src/transport.c src/transport.h include/fauxdev/protocol.h | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) tests/camera_global_broker_test.c src/transport.c -o $@ $(LDFLAGS)

test: all
	LD_PRELOAD=$(abspath $(LIB)) $(TEST)
	$(TRANSPORT_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(VIDEO_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(VIDEO_BROKER_TEST)
	$(INVENTORY_TEST) $(abspath $(INVENTORY_TOOL))
	LD_PRELOAD=$(abspath $(LIB)) $(CAMERA_SELECT_BROKER_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(MULTIOPEN_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(CAMERA_MODES_TEST)
	LD_PRELOAD=$(abspath $(LIB)) $(CAMERA_GLOBAL_TEST)

clean:
	rm -rf $(BUILD)
