-include config.mak

CC ?= cc
AR ?= ar
RANLIB ?= ranlib
INSTALL ?= install
PREFIX ?= /usr/local
CONFIG_CFLAGS ?= -O3 -std=gnu11
WITH_UBLIO ?= local
WITH_UTF8PROC ?= local
CFLAGS := $(CONFIG_CFLAGS) -D_FILE_OFFSET_BITS=64 $(CFLAGS)

FUSE_FLAGS = -DFUSE_USE_VERSION=28
FUSE_LIB = -lfuse
OS := $(shell uname)
ifeq ($(OS), Darwin)
	APP_FLAGS += -DHAVE_BIRTHTIME
	FUSE_FLAGS += -I/usr/local/include/osxfuse
	FUSE_LIB = -losxfuse
else ifeq ($(OS), Haiku)
	CFLAGS += -D_BSD_SOURCE
	APP_LIB += -lbsd
	FUSE_FLAGS += -I/system/develop/headers/userlandfs -I/system/develop/headers/bsd
	FUSE_LIB = -L/system/lib/ -luserlandfs_fuse
else ifeq ($(OS), FreeBSD)
	APP_FLAGS += -DHAVE_BIRTHTIME
	APP_FLAGS += -I/usr/local/include
	APP_LIB += -L/usr/local/lib
	FUSE_FLAGS += -I/usr/local/include
endif

LIBS = lib/libhfsuser/libhfsuser.a lib/libhfs/libhfs.a
LIBDIRS = $(abspath $(dir $(LIBS)))
INCLUDE = -I $(abspath lib)

ifneq ($(WITH_UBLIO), none)
	APP_FLAGS += -DHAVE_UBLIO
	ifeq ($(WITH_UBLIO), system)
		APP_LIB += -lublio
	else ifeq ($(WITH_UBLIO), local)
		LIBS += lib/ublio/libublio.a
	endif
endif
ifneq ($(WITH_UTF8PROC), none)
	APP_FLAGS += -DHAVE_UTF8PROC
	ifeq ($(WITH_UTF8PROC), system)
		APP_LIB += -lutf8proc
	else ifeq ($(WITH_UTF8PROC), local)
		LIBS += lib/utf8proc/libutf8proc.a
	endif
endif

GIT_HEAD_SHA=$(shell git rev-parse --short HEAD 2> /dev/null)
ifneq ($(GIT_HEAD_SHA), )
	VERSION = 0.$(shell git rev-list HEAD --count)-$(GIT_HEAD_SHA)
	CFLAGS += -DHFSFUSE_VERSION_STRING=\"$(VERSION)\"
endif

export PREFIX CC CFLAGS APP_FLAGS LIBDIRS AR RANLIB INCLUDE

.PHONY: all clean always_check config install uninstall install-lib uninstall-lib lib version

all: hfsfuse hfsdump

%.o: %.c
	$(CC) $(CFLAGS) $(FUSE_FLAGS) $(INCLUDE) -c -o $*.o $^

$(LIBS): always_check
	$(MAKE) -C $(dir $@)

lib: $(LIBS)

hfsfuse: src/hfsfuse.o $(LIBS)
	$(CC) $(CFLAGS) $(APP_LIB) -o $@ src/hfsfuse.o $(LIBS) $(FUSE_LIB) -lpthread

hfsdump: src/hfsdump.o $(LIBS)
	$(CC) $(CFLAGS) $(APP_LIB) -o $@ src/hfsdump.o $(LIBS) -lpthread

clean:
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir clean; done
	rm -f src/hfsfuse.o hfsfuse src/hfsdump.o hfsdump

distclean: clean
	rm -f config.mak src/version.h

install-lib: $(LIBS)
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir install; done

uninstall-lib: $(LIBS)
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir uninstall; done

install: hfsfuse hfsdump
	$(INSTALL) $< $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/hfsfuse $(PREFIX)/bin/hfsdump

src/version.h:
	echo \#define HFSFUSE_VERSION_STRING $(VERSION) > src/version.h

version: src/version.h

config:
	echo CC=$(CC) > config.mak
	echo CONFIG_CFLAGS=$(CFLAGS) >> config.mak
	echo WITH_UBLIO=$(WITH_UBLIO) >> config.mak
	echo WITH_UTF8PROC=$(WITH_UTF8PROC) >> config.mak
