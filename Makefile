ifeq ($(filter config, $(MAKECMDGOALS)),)
-include config.mak
endif

CC ?= cc
AR ?= ar
RANLIB ?= ranlib
INSTALL ?= install
TAR ?= tar
CONFIG_CFLAGS ?= -O3 -std=gnu11
#CONFIG_CFLAGS ?= -O3 -std=c11 -D_POSIX_C_SOURCE=200809L #-D_XOPEN_SOURCE=700
CONFIG_CFLAGS := $(CONFIG_CFLAGS) $(CFLAGS)

CFLAGS := -D_FILE_OFFSET_BITS=64 $(CONFIG_CFLAGS)

# extra flags we don't want to forward to the "external" libs like libhfs/ublio/utf8proc
LOCAL_CFLAGS+=-Wall -Wextra -pedantic -Wno-gnu-zero-variadic-macro-arguments -Wno-unused-parameter
# older versions of gcc/clang need these as well
LOCAL_CFLAGS+=-Wno-missing-field-initializers -Wno-missing-braces

TARGETS = hfsfuse hfsdump
FUSE_FLAGS = -DFUSE_USE_VERSION=28
FUSE_LIB = -lfuse
OS := $(shell uname)
ifeq ($(OS), Darwin)
	APP_FLAGS += -I/usr/local/include
	APP_LIB += -L/usr/local/lib
	ifeq ($(shell [ -e /usr/local/lib/libosxfuse.dylib ] && echo 1), 1)
		FUSE_FLAGS += -I/usr/local/include/osxfuse
		FUSE_LIB = -losxfuse
	else
		FUSE_FLAGS += -I/usr/local/include
	endif
else ifeq ($(OS), Haiku)
	CFLAGS += -D_BSD_SOURCE
	APP_LIB += -lbsd
	FUSE_FLAGS += -I/system/develop/headers/userlandfs/fuse -I/system/develop/headers/bsd
	FUSE_LIB = -L/system/lib/ -luserlandfs_fuse
	PREFIX ?= /boot/home/config/non-packaged
else ifeq ($(OS), FreeBSD)
	APP_FLAGS += -I/usr/local/include
	APP_LIB += -L/usr/local/lib
	FUSE_FLAGS += -I/usr/local/include
else ifeq ($(OS), DragonFly)
	APP_FLAGS += -I/usr/local/include
	APP_LIB += -L/usr/local/lib
	FUSE_FLAGS += -I/usr/local/include
else ifeq ($(OS), OpenBSD)
	APP_FLAGS += -I/usr/local/include -I/usr/local/include/libutf8proc
	APP_LIB += -L/usr/local/lib
else ifeq ($(OS), NetBSD)
$(info NetBSD detected, only hfsdump will be built by default)
	TARGETS=hfsdump
else ifeq ($(OS), SunOS)
	FUSE_FLAGS += -I/usr/include/fuse
else ifeq (MSYS, $(findstring MSYS, $(OS)))
$(info MSYS2 detected, only hfsdump will be built by default)
	WITH_UBLIO ?= none
	ifneq (none, $(WITH_UBLIO))
$(warn building with ublio is not supported under MSYS2)
	endif
	TARGETS = hfsdump
else ifeq (MINGW, $(findstring MINGW, $(OS)))
$(info MinGW detected, only hfsdump will be built by default)
	WITH_UBLIO ?= none
	ifneq ($(WITH_UBLIO), none)
$(warn building with ublio is not supported under MinGW)
	endif
	TARGETS = hfsdump
endif

PREFIX ?= /usr/local
WITH_UBLIO ?= local
WITH_UTF8PROC ?= local

CEXPR_TEST_CFLAGS = -Werror-implicit-function-declaration -Wno-unused-value -Wno-missing-braces\
 -Wno-missing-field-initializers -Wno-format-security -Wno-format-nonliteral

ccshellcmd = printf "%s\n" "int main(void){$(1);}" | $(CC) $(if $(CEXPR_CFLAGS),$(CEXPR_CFLAGS),$(CFLAGS)) -xc -fsyntax-only $(CEXPR_TEST_CFLAGS) $(foreach inc,$(2),-include $(inc)) -
parsecexpr = $(shell ! $(call ccshellcmd, $(1), $(2)) > $(if $(VERBOSE),/dev/stdout,/dev/null) 2> $(if $(VERBOSE),/dev/stderr,/dev/null); echo $$?)

define cccheck
ifndef $(1)
    $$(if $$(VERBOSE),$$(info Checking $(1) with `$(call ccshellcmd, $(2), $(3))`))
    $(1) := $$(call parsecexpr, $(2), $(3))
    $$(info $(1): $$(if $$(filter $$($(1)),1),yes,no))
endif
FEATURES+=$(1)
endef

define testcccheck
ifneq ($$(call parsecexpr),1)
    tmp:=$$(VERBOSE)
    VERBOSE=true
    $$(info Unable to use C compiler "$(CC)" for platform-dependent feature detection. Specify these directly to make or by running `make config` and editing $\
           config.mak, otherwise fallbacks will be used. Command:)
    $$(info $$(call ccshellcmd))
    _:=$$(call parsecexpr)
    VERBOSE:=$$(tmp)
endif
endef

CEXPR_CFLAGS=$(CFLAGS) $(LIBHFS_CFLAGS)
$(eval $(call testcccheck))
$(eval $(call cccheck,HAVE_BEXXTOH_ENDIAN_H,{ be16toh(0); be32toh(0); be64toh(0); },endian.h))
$(eval $(call cccheck,HAVE_BEXXTOH_SYS_ENDIAN_H,{ be16toh(0); be32toh(0); be64toh(0); },sys/endian.h))
$(eval $(call cccheck,HAVE_OSBYTEORDER_H,{ OSSwapBigToHostInt16(0); OSSwapBigToHostInt32(0); OSSwapBigToHostInt64(0); },libkern/OSByteOrder.h))

CEXPR_CFLAGS=$(CFLAGS) $(LOCAL_CFLAGS)
$(eval $(call testcccheck))
$(eval $(call cccheck,HAVE_BIRTHTIME,{ (struct stat){0}.st_birthtime; },sys/stat.h))
$(eval $(call cccheck,HAVE_STAT_FLAGS,{ (struct stat){0}.st_flags; },sys/stat.h))
$(eval $(call cccheck,HAVE_STAT_BLKSIZE,{ (struct stat){0}.st_blksize; },sys/stat.h))
$(eval $(call cccheck,HAVE_STAT_BLOCKS,{ (struct stat){0}.st_blocks; },sys/stat.h))
$(eval $(call cccheck,HAVE_VSYSLOG,{ vsyslog(0,(const char*){0},(va_list){0}); },syslog.h stdarg.h))
$(eval $(call cccheck,HAVE_PREAD,{ pread(0,(void*){0},0,0); },unistd.h))

$(foreach cfg,CC AR RANLIB INSTALL PREFIX WITH_UBLIO WITH_UTF8PROC CONFIG_CFLAGS $(FEATURES),$(eval CONFIG:=$(CONFIG)$(cfg)=$$($(cfg))\n))
$(foreach feature,$(FEATURES),$(if $(filter $($(feature)),1),$(eval CFLAGS+=-D$(feature))))

LIBS = lib/libhfsuser/libhfsuser.a lib/libhfs/libhfs.a
LIBDIRS = $(abspath $(dir $(LIBS)))
INCLUDE = $(foreach lib,$(LIBDIRS),-iquote $(lib))

ifneq ($(WITH_UBLIO), none)
	APP_FLAGS += -DHAVE_UBLIO
	ifeq ($(WITH_UBLIO), system)
		APP_LIB += -lublio
	else ifeq ($(WITH_UBLIO), local)
		LIBS += lib/ublio/libublio.a
	else
$(error Invalid option "$(WITH_UBLIO)" for WITH_UBLIO. Use one of: none, system, local)
	endif
endif
ifneq ($(WITH_UTF8PROC), none)
	APP_FLAGS += -DHAVE_UTF8PROC
	ifeq ($(WITH_UTF8PROC), system)
		APP_LIB += -lutf8proc
	else ifeq ($(WITH_UTF8PROC), local)
		CFLAGS+=-DUTF8PROC_EXPORTS
		LIBS += lib/utf8proc/libutf8proc.a
	else
$(error Invalid option "$(WITH_UTF8PROC)" for WITH_UTF8PROC. Use one of: none, system, local)
	endif
endif

RELEASE_NAME=hfsfuse
GIT_HEAD_SHA=$(shell git rev-parse --short HEAD 2> /dev/null)
ifneq ($(GIT_HEAD_SHA), )
	VERSION = 0.$(shell git rev-list HEAD --count)
	RELEASE_NAME := $(RELEASE_NAME)-$(VERSION)
	VERSION_STRING = \"$(VERSION)-$(GIT_HEAD_SHA)\"
	CFLAGS += -DHFSFUSE_VERSION_STRING=$(VERSION_STRING)
else ifeq ($(wildcard src/version.h), )
    $(warning Warning: git repo nor prepackaged version.h found, hfsfuse will be built without version information)
	CFLAGS += -DHFSFUSE_VERSION_STRING=\"omitted\"
endif

export CONFIG PREFIX CC CFLAGS LOCAL_CFLAGS APP_FLAGS LIBDIRS AR RANLIB INCLUDE

.PHONY: all clean always_check config install uninstall install-lib uninstall-lib lib version dist

all: $(TARGETS)

%.o: %.c
	$(CC) $(INCLUDE) $(CFLAGS) $(LOCAL_CFLAGS) -c -o $*.o $^

src/hfsfuse.o: src/hfsfuse.c
	$(CC) $(FUSE_FLAGS) $(INCLUDE) $(CFLAGS) $(LOCAL_CFLAGS) -c -o $*.o $^

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

ifeq ($(OS), Haiku)
install: $(TARGETS)
	mkdir -p $(PREFIX)/add-ons/userlandfs/
	$(INSTALL) -m 644 hfsfuse $(PREFIX)/add-ons/userlandfs/
	$(INSTALL) hfsdump $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/add-ons/userlandfs/hfsfuse $(PREFIX)/bin/hfsdump
else
install:$(TARGETS)
	$(INSTALL) $^ $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/hfsfuse $(PREFIX)/bin/hfsdump
endif

version:
	echo \#define HFSFUSE_VERSION_STRING $(VERSION_STRING) > src/version.h

config:
	@echo "$$CONFIG" > config.mak

dist: version
	ln -s . $(RELEASE_NAME)
	git ls-files | sed "s/^/$(RELEASE_NAME)\//" | COPYFILE_DISABLE=1 $(TAR) -czf "$(RELEASE_NAME).tar.gz" -T - -- "$(RELEASE_NAME)/src/version.h"
	rm -f -- $(RELEASE_NAME)
