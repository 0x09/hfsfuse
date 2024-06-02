OS = $(shell uname)

ifeq ($(filter config, $(MAKECMDGOALS)),)
-include config.mak
endif

RANLIB ?= ranlib
INSTALL ?= install
TAR ?= tar
CONFIG_CFLAGS ?= -O3 -std=gnu11
#CONFIG_CFLAGS ?= -O3 -std=c11 -D_POSIX_C_SOURCE=200809L #-D_XOPEN_SOURCE=700
CONFIG_CFLAGS := $(CONFIG_CFLAGS) $(CFLAGS)

CFLAGS := -D_FILE_OFFSET_BITS=64 $(CONFIG_CFLAGS)

# extra flags we don't want to forward to the "external" libs like libhfs/ublio/utf8proc
LOCAL_CFLAGS+=-Wall -Wextra -pedantic -Wno-gnu-zero-variadic-macro-arguments -Wno-unused-parameter -Wno-error=type-limits -Wno-tautological-constant-out-of-range-compare
# older versions of gcc/clang need these as well
LOCAL_CFLAGS+=-Wno-missing-field-initializers -Wno-missing-braces

TARGETS = hfsfuse hfsdump
FUSE_FLAGS = -DFUSE_USE_VERSION=28
FUSE_LIB = -lfuse

XATTR_NAMESPACE ?= user.
ifeq ($(OS), Darwin)
	APP_FLAGS += -I/usr/local/include
	APP_LIB += -L/usr/local/lib
	XATTR_NAMESPACE = #no namespaces on macOS
	ifeq ($(shell [ -e /usr/local/lib/libosxfuse.dylib ] && echo 1), 1)
		FUSE_FLAGS += -I/usr/local/include/osxfuse
		FUSE_LIB = -losxfuse
	else ifeq ($(shell [ -e /usr/local/lib/libfuse.dylib ] && echo 1), 1)
		FUSE_FLAGS += -I/usr/local/include
	else ifeq ($(shell [ -e /usr/local/lib/libfuse-t.dylib ] && echo 1), 1)
		FUSE_FLAGS += -I/usr/local/include/fuse
		FUSE_LIB = -lfuse-t
	else
$(info no FUSE install detected, only hfsdump will be built)
		TARGETS = hfsdump
	endif
else ifeq ($(OS), Haiku)
	CFLAGS += -D_BSD_SOURCE -DB_USE_POSITIVE_POSIX_ERRORS
	APP_LIB += -lbsd -lposix_error_mapper
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
	APP_FLAGS += -I/usr/pkg/include
	APP_LIB += -L/usr/pkg/lib -Wl,-R/usr/pkg/lib
	FUSE_FLAGS += -I/usr/pkg/include
	FUSE_LIB += -L/usr/pkg/lib/ -Wl,-R/usr/pkg/lib
else ifeq ($(OS), SunOS)
	FUSE_FLAGS += -I/usr/include/fuse
else ifeq (MSYS, $(findstring MSYS, $(OS)))
$(info MSYS2 detected, only hfsdump will be built by default)
	WITH_UBLIO ?= none
	ifneq (none, $(WITH_UBLIO))
$(warning building with ublio is not supported under MSYS2)
	endif
	TARGETS = hfsdump
else ifeq (MINGW, $(findstring MINGW, $(OS)))
$(info MinGW detected, only hfsdump will be built by default)
	WITH_UBLIO ?= none
	ifneq ($(WITH_UBLIO), none)
$(warning building with ublio is not supported under MinGW)
	endif
	TARGETS = hfsdump
endif

PREFIX ?= /usr/local
prefix ?= $(PREFIX)
bindir = $(prefix)/bin
libdir = $(prefix)/lib
includedir = $(prefix)/include
WITH_UBLIO ?= local
WITH_UTF8PROC ?= local

CEXPR_TEST_CFLAGS = -Werror-implicit-function-declaration -Wno-unused-value -Wno-missing-braces\
 -Wno-missing-field-initializers -Wno-format-security -Wno-format-nonliteral

ccshellcmd = printf "%s\n" "int main(void){$(1);}" | $(CC) $(if $(CEXPR_CFLAGS),$(CEXPR_CFLAGS),$(CFLAGS)) -xc -fsyntax-only $(CEXPR_TEST_CFLAGS) $(foreach inc,$(2),-include $(inc)) -
parsecexpr = $(shell ! $(call ccshellcmd, $(1), $(2)) > $(if $(VERBOSE),/dev/stdout,/dev/null) 2> $(if $(VERBOSE),/dev/stderr,/dev/null); echo $$?)

echo_features = $(if $(filter showconfig,$(MAKECMDGOALS)),,1)

define cccheck
ifndef $(1)
    $$(if $$(VERBOSE),$$(info Checking $(1) with `$(call ccshellcmd, $(2), $(3))`))
    $(1) := $$(call parsecexpr, $(2), $(3))
    $$(if $$(echo_features),$$(info $(1): $$(if $$(filter $$($(1)),1),yes,no)))
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

non_build_targets = dist version authors uninstall uninstall-lib clean distclean

ifneq ($(filter-out $(non_build_targets),$(or $(MAKECMDGOALS),all)),)
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

    $(eval $(call cccheck,HAVE_LZFSE,,lzfse.h))
    $(eval $(call cccheck,HAVE_ZLIB,,zlib.h))
endif

$(foreach cfg,OS CC AR RANLIB INSTALL TAR PREFIX WITH_UBLIO WITH_UTF8PROC XATTR_NAMESPACE CONFIG_CFLAGS $(FEATURES),$(eval CONFIG:=$(CONFIG)$(cfg)=$$($(cfg))\n))
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

APP_LIB+=$(if $(filter $(HAVE_ZLIB),1),-lz)
APP_LIB+=$(if $(filter $(HAVE_LZFSE),1),-llzfse)

RELEASE_NAME=hfsfuse
RELEASE_BRANCH=master
GIT_HEAD_SHA=$(shell git rev-parse --short $(RELEASE_BRANCH) 2> /dev/null)
ifneq ($(GIT_HEAD_SHA), )
	VERSION = 0.$(shell git rev-list $(RELEASE_BRANCH) --count)
	RELEASE_NAME := $(RELEASE_NAME)-$(VERSION)
	VERSION_STRING = \"$(VERSION)-$(GIT_HEAD_SHA)\"
	CFLAGS += -DHFSFUSE_VERSION_STRING=$(VERSION_STRING)
else ifeq ($(wildcard src/version.h), )
    $(warning Warning: git repo nor prepackaged version.h found, hfsfuse will be built without version information)
	CFLAGS += -DHFSFUSE_VERSION_STRING=\"omitted\"
endif

export CONFIG PREFIX prefix bindir libdir includedir DESTDIR CC CFLAGS LOCAL_CFLAGS APP_FLAGS LIBDIRS AR RANLIB INSTALL INCLUDE

vpath %.o src

.PHONY: all clean always_check config showconfig install install-lib lib $(non_build_targets)

all: $(TARGETS)

%.o: CPPFLAGS += $(INCLUDE)
%.o: CFLAGS += $(LOCAL_CFLAGS)

src/hfsfuse.o: CPPFLAGS += $(FUSE_FLAGS) -DXATTR_NAMESPACE=$(XATTR_NAMESPACE)

$(LIBS): always_check
	$(MAKE) -C $(dir $@)

lib: $(LIBS)

hfsfuse: LDLIBS += $(LIBS) $(APP_LIB) $(FUSE_LIB) -lpthread
hfsfuse: src/hfsfuse.o $(LIBS)

hfsdump: LDLIBS += $(LIBS) $(APP_LIB) -lpthread
hfsdump: src/hfsdump.o $(LIBS)

clean:
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir clean; done
	$(RM) src/hfsfuse.o hfsfuse src/hfsdump.o hfsdump

distclean: clean
	$(RM) config.mak src/version.h AUTHORS

install-lib: $(LIBS)
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir install; done

uninstall-lib: $(LIBS)
	for dir in $(LIBDIRS); do $(MAKE) -C $$dir uninstall; done

ifeq ($(OS), Haiku)
install: $(TARGETS)
	mkdir -pm755 $(prefix)/add-ons/userlandfs/
	$(INSTALL) -m644 hfsfuse $(DESTDIR)$(prefix)/add-ons/userlandfs/
	$(INSTALL) -m755 hfsdump $(DESTDIR)$(bindir)

uninstall:
	$(RM) $(DESTDIR)$(prefix)/add-ons/userlandfs/hfsfuse $(DESTDIR)$(bindir)/hfsdump
else
install: $(TARGETS)
	mkdir -pm755 $(DESTDIR)$(bindir)
	$(INSTALL) -m755 $^ $(DESTDIR)$(bindir)

uninstall:
	$(RM) $(DESTDIR)$(bindir)/hfsfuse $(DESTDIR)$(bindir)/hfsdump
endif

version:
	echo \#define HFSFUSE_VERSION_STRING $(VERSION_STRING) > src/version.h

authors:
	git shortlog -sne $(RELEASE_BRANCH) | cut -d $$'\t' -f 2- > AUTHORS

showconfig:
	@echo "$$CONFIG"

config:
	@echo "$$CONFIG" > config.mak

dist: version authors
	git archive $(RELEASE_BRANCH) -o "$(RELEASE_NAME).tar.gz" --prefix "$(RELEASE_NAME)/src/" --add-file src/version.h --prefix "$(RELEASE_NAME)/" --add-file AUTHORS
