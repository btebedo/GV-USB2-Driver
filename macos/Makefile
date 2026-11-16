# gvcap - macOS user-space capture for the I-O DATA GV-USB2
#
# Needs libusb 1.0 (brew install libusb pkg-config).
# Works on Intel and Apple Silicon; Homebrew's prefix is found via pkg-config.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter
# Oldest macOS we target (Mac Pro 6,1 tops out at Monterey). Note that a
# Homebrew libusb is built for the machine it was installed on; to ship a
# binary to an older macOS, build libusb there or link a static one built
# with the same -mmacosx-version-min.
CFLAGS  += -mmacosx-version-min=12.3
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS   := $(shell pkg-config --libs libusb-1.0)

BUILD := build
BINS  := $(BUILD)/gvcap $(BUILD)/gvdump

all: $(BINS)

$(BUILD)/%.o: src/%.c src/gvusb2.h | $(BUILD)
	$(CC) $(CFLAGS) $(USB_CFLAGS) -c $< -o $@

$(BUILD)/gvcap: $(BUILD)/gvcap.o $(BUILD)/gvusb2.o
	$(CC) $(CFLAGS) $^ $(USB_LIBS) -lpthread -o $@

$(BUILD)/gvdump: $(BUILD)/gvdump.o $(BUILD)/gvusb2.o
	$(CC) $(CFLAGS) $^ $(USB_LIBS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
