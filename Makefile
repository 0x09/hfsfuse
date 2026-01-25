##
#
# LZVN encode/decode routines
#
##

PREFIX ?= /usr/local
prefix ?= $(PREFIX)
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
libdir = $(exec_prefix)/lib
includedir = $(prefix)/include

RANLIB ?= ranlib
INSTALL ?= install
ARFLAGS ?= crus
CFLAGS := -std=gnu99 -O3 -ffast-math $(CFLAGS)

all: lzvn

libFastCompression.a: lzvn_encode.o lzvn_decode.o
	$(AR) $(ARFLAGS) $@ $+
	$(RANLIB) $@

lzvn: libFastCompression.a

clean:
	$(RM) *.o *.a lzvn

install: lzvn
	$(INSTALL) -m644 FastCompression.h $(DESTDIR)$(includedir)
	$(INSTALL) -m644 libFastCompression.a $(DESTDIR)$(libdir)
	$(INSTALL) lzvn $(DESTDIR)$(bindir)

uninstall:
	$(RM) $(DESTDIR)$(includedir)/FastCompression.h $(DESTDIR)$(libdir)/libFastCompression.a $(DESTDIR)$(bindir)/lzvn
