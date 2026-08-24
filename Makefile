# Luckfox Pico Pro Max firmware helper Makefile

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDLIBS ?= -pthread

SRC_DIR := src
BUILD_DIR := bin

.PHONY: all clean test video web_config check

all: test video web_config

test: $(BUILD_DIR)/test
video: $(BUILD_DIR)/video
web_config: $(BUILD_DIR)/web_config

$(BUILD_DIR)/test: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/video: $(SRC_DIR)/video_stream_record.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/web_config: $(SRC_DIR)/web_config.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

check:
	$(CC) $(CFLAGS) -fsyntax-only $(SRC_DIR)/web_config.c

clean:
	rm -rf $(BUILD_DIR)
