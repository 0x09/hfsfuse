-include config.mak

CC ?= cc
PREFIX ?= /usr/local
CONFIG_CFLAGS ?= -O3 -std=gnu11
WITH_UBLIO ?= local
WITH_UTF8PROC ?= local
CFLAGS := $(CONFIG_CFLAGS) $(CFLAGS)

export CC
export CFLAGS

FUSE_FLAGS = -DFUSE_USE_VERSION=28 -D_FILE_OFFSET_BITS=64
FUSE_LIB = -lfuse
OS := $(shell uname)
ifeq ($(OS), Darwin)
	FUSE_FLAGS += -I/usr/local/include/osxfuse -DHAVE_BIRTHTIME
	FUSE_LIB = -losxfuse
else ifeq ($(OS), FreeBSD)
	FUSE_FLAGS += -DHAVE_BIRTHTIME
endif

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:%.c=%.o)
LIBS = lib/libhfs/libhfs.a
LIBDIRS = $(dir $(LIBS))
INCLUDE = $(foreach dir, $(LIBDIRS), -I$(dir))
TARGET = hfsfuse

ifneq ($(WITH_UBLIO), none)
	FUSE_FLAGS += -DHAVE_UBLIO
	ifeq ($(WITH_UBLIO), system)
		FUSE_LIB += -lublio
	else ifeq ($(WITH_UBLIO), local)
		LIBS += lib/ublio/ublio.o
	endif
endif
ifneq ($(WITH_UTF8PROC), none)
	FUSE_FLAGS += -DHAVE_UTF8PROC
	ifeq ($(WITH_UTF8PROC), system)
		FUSE_LIB += -lutf8proc
	else ifeq ($(WITH_UTF8PROC), local)
		LIBS += lib/utf8proc/utf8proc.o
	endif
endif

.PHONY: all amalgamation clean always_check config install uninstall

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) $(FUSE_FLAGS) $(INCLUDE) -c -o $*.o $^

$(LIBS): always_check
	$(MAKE) -C $(dir $@)

$(TARGET): $(LIBS) $(OBJS)
	$(CC) $(CFLAGS) $(FUSE_LIB) -o $@ lib/libhfs/*.o $^ -lpthread

amalgamation: $(foreach dir, $(LIBDIRS), $(dir)*.c) $(SRCS)
	cat $^ | $(CC) $(CFLAGS) $(FUSE_FLAGS) $(FUSE_LIB) $(INCLUDE) -o $(TARGET) -x c -

clean:
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir clean; done
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install $< $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

config:
	echo CC=$(CC) > config.mak
	echo CONFIG_CFLAGS=$(CFLAGS) >> config.mak
	echo WITH_UBLIO=$(WITH_UBLIO) >> config.mak
	echo WITH_UTF8PROC=$(WITH_UTF8PROC) >> config.mak
