# Luckfox Pico Pro test Makefile

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS := -O2 -Wall -Wextra -std=c11
LDFLAGS := -lpthread

SRC_DIR := src
BUILD_DIR := bin

.PHONY: all clean test video web_config

# Targets
all: test video web_config

test: $(BUILD_DIR)/test
video: $(BUILD_DIR)/video
web_config: $(BUILD_DIR)/web_config

$(BUILD_DIR)/test: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/video: $(SRC_DIR)/video_stream_record.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/web_config: $(SRC_DIR)/web_config.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
