# cleanlinux-boot: dinit services and a helper in C for the boot of CleanLinux
#
#   make                 cleanlinux-boot and cleanlinux-init
#   make check           unit tests (no root needed)
#   make install         DESTDIR=... PREFIX=/usr
#   make dist            cleanlinux-boot-VERSION.tar.gz from git HEAD

VERSION := 0.5.1
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/lib/cleanlinux-boot
DINITDIR ?= $(PREFIX)/lib/dinit.d
DATADIR ?= $(PREFIX)/lib

CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
WARN = -std=c17 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
# the clock floor: the build date, reproducible with SOURCE_DATE_EPOCH
ifndef BUILD_EPOCH
BUILD_EPOCH := $(or $(SOURCE_DATE_EPOCH),$(shell date +%s))
endif
DEFS = -DVERSION='"$(VERSION)"' -DBUILD_EPOCH=$(BUILD_EPOCH) -DLIBEXECDIR='"$(LIBEXECDIR)"'

KMOD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libkmod 2>/dev/null)
KMOD_LIBS := $(or $(shell $(PKG_CONFIG) --libs libkmod 2>/dev/null),-lkmod)
BLKID_CFLAGS := $(shell $(PKG_CONFIG) --cflags blkid 2>/dev/null)
BLKID_LIBS := $(or $(shell $(PKG_CONFIG) --libs blkid 2>/dev/null),-lblkid)
ZSTD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libzstd 2>/dev/null)
ZSTD_LIBS := $(or $(shell $(PKG_CONFIG) --libs libzstd 2>/dev/null),-lzstd)

ALL_CFLAGS = $(WARN) $(DEFS) $(KMOD_CFLAGS) $(BLKID_CFLAGS) $(ZSTD_CFLAGS) $(CPPFLAGS) $(CFLAGS)

BOOT_OBJ = src/main.o src/util.o src/mounts.o src/sysctl.o src/modules.o src/misc.o src/tmpfiles.o \
	src/tty.o src/kernel.o src/bootimg.o src/initramfs.o src/sysusers.o src/power.o src/vconsole.o
INIT_OBJ = src/init.o src/util.o src/mounts.o src/modules.o

all: cleanlinux-boot cleanlinux-init

src/%.o: src/%.c src/util.h src/boot.h
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

cleanlinux-boot: $(BOOT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BOOT_OBJ) $(KMOD_LIBS) $(ZSTD_LIBS)

cleanlinux-init: $(INIT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(INIT_OBJ) $(KMOD_LIBS) $(BLKID_LIBS)

check: cleanlinux-boot
	./tests/run.sh ./cleanlinux-boot

install: all
	install -Dm755 cleanlinux-boot $(DESTDIR)$(BINDIR)/cleanlinux-boot
	install -Dm755 cleanlinux-init $(DESTDIR)$(LIBEXECDIR)/init
	install -d $(DESTDIR)$(DINITDIR) $(DESTDIR)/etc/dinit.d/boot.d $(DESTDIR)$(DATADIR)/dinit
	for f in services/*; do install -m644 $$f $(DESTDIR)$(DINITDIR)/; done
	ln -sf ../../bin/cleanlinux-boot $(DESTDIR)$(DATADIR)/dinit/shutdown-hook
	install -Dm644 data/tmpfiles.conf $(DESTDIR)$(DATADIR)/tmpfiles.d/cleanlinux-boot.conf
	install -Dm644 data/burn.trigger $(DESTDIR)$(PREFIX)/share/burn/triggers/cleanlinux-boot.trigger
	install -Dm644 data/sysusers.trigger $(DESTDIR)$(PREFIX)/share/burn/triggers/cleanlinux-sysusers.trigger
	ln -sf dinit $(DESTDIR)$(BINDIR)/init

dist:
	git archive --format=tar --prefix=cleanlinux-boot-$(VERSION)/ HEAD | gzip -n -9 >cleanlinux-boot-$(VERSION).tar.gz

clean:
	rm -f src/*.o cleanlinux-boot cleanlinux-init cleanlinux-boot-*.tar.gz

.PHONY: all check install dist clean
