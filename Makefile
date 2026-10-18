# alfa-driver — build for macOS Apple Silicon (arm64), userspace via libusb.
# Chips: RTL8812AU (rtl_chip.c) and RTL8821AU (rtl8821au.c), USB IDs in rtl_ids.c.

CC      ?= clang

PKGCONF := $(shell command -v pkg-config 2>/dev/null)
ifeq ($(PKGCONF),)
  LIBUSB_CFLAGS := -I/opt/homebrew/include/libusb-1.0
  LIBUSB_LIBS   := -L/opt/homebrew/lib -lusb-1.0
else
  LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
  LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)
endif

CFLAGS  += -Wall -Wextra -O2 -arch arm64 $(LIBUSB_CFLAGS) -Isrc
LDFLAGS += $(LIBUSB_LIBS)

# All headers as prerequisites: otherwise make does not rebuild after a
# header-only change (there is no intermediate .o stage).
HDRS := $(wildcard src/*.h)

# Recipe: pass only the .c files to the compiler, filter out headers.
BUILD = $(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDFLAGS)

# Shared chip modules (bring-up through monitor, incl. ops table and TX).
CORE := src/rtl_usb.c src/rtl_ids.c src/rtl_init.c src/rtl_fw.c \
        src/fw_8812a_nic.c src/fw_8821a_nic.c \
        src/rtl_efuse.c src/rtl_led.c src/rtl_mac.c src/rtl_bb.c \
        src/rtl_rf.c src/rtl_cal.c src/rtl_txpwr.c \
        src/rtl_tx.c src/rtl_chip.c src/rtl8821au.c \
        src/rtl8821au_phy.c src/rtl8821au_tables.c src/rtl8821au_cal.c \
        src/rtl8821au_txpwr.c

# Bring-up + receive path (everything rtl_hal_full_init uses).
HAL := src/rtl_hal.c src/rtl_rx.c $(CORE)

BINS := usbprobe chipinfo initchip efuseinfo fwload ledtest stagetest \
        monitor inject ledscan alfa-extcap scan associate connect \
        ccmptest alfa-netd deauth txtest wpatest

all: $(BINS)

# --- Standalone tools ---

usbprobe: src/usbprobe.c src/rtl_ids.c $(HDRS)
	$(BUILD)

chipinfo: src/chipinfo.c src/rtl_usb.c src/rtl_ids.c $(HDRS)
	$(BUILD)

initchip: src/initchip.c $(CORE) $(HDRS)
	$(BUILD)

efuseinfo: src/efuseinfo.c src/rtl_efuse.c src/rtl_init.c src/rtl_usb.c src/rtl_ids.c $(HDRS)
	$(BUILD)

fwload: src/fwload.c $(CORE) $(HDRS)
	$(BUILD)

ledtest: src/ledtest.c src/rtl_led.c src/rtl_fw.c src/fw_8812a_nic.c src/rtl_init.c src/rtl_usb.c src/rtl_ids.c $(HDRS)
	$(BUILD)

ccmptest: src/ccmptest.c src/rtl_ccmp.c $(HDRS)
	$(BUILD)

stagetest: src/stagetest.c $(CORE) $(HDRS)
	$(BUILD)

# --- Tools with full bring-up + RX path (rtl_tx.c is in CORE!) ---

monitor: src/monitor.c $(HAL) $(HDRS)
	$(BUILD)

inject: src/inject.c $(HAL) $(HDRS)
	$(BUILD)

ledscan: src/ledscan.c $(HAL) $(HDRS)
	$(BUILD)

alfa-extcap: src/alfa_extcap.c $(HAL) $(HDRS)
	$(BUILD)

scan: src/scan.c $(HAL) $(HDRS)
	$(BUILD)

associate: src/associate.c $(HAL) $(HDRS)
	$(BUILD)

connect: src/connect.c src/rtl_ccmp.c $(HAL) $(HDRS)
	$(BUILD)

alfa-netd: src/alfa_netd.c src/rtl_wpa.c src/rtl_ccmp.c $(HAL) $(HDRS)
	$(BUILD)

deauth: src/deauth.c $(HAL) $(HDRS)
	$(BUILD)

txtest: src/txtest.c $(HAL) $(HDRS)
	$(BUILD)

wpatest: src/wpatest.c src/rtl_wpa.c src/rtl_ccmp.c $(HAL) $(HDRS)
	$(BUILD)

# Self-tests that need no adapter (AES-CCM vectors, PBKDF2 vectors).
check: ccmptest connect
	./ccmptest
	./connect --selftest

help:
	@echo "make          build all tools (arm64, libusb)"
	@echo "make check    run the hardware-free self-tests"
	@echo "make clean    remove the built tools"
	@echo "See README.md for the tools and docs/ for details."

clean:
	rm -f $(BINS)

.PHONY: all check help clean
