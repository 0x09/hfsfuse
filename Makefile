##
#
# LZVN encode/decode routines
#
##

PREFIX ?= /usr/local

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
	$(INSTALL) -m644 FastCompression.h $(PREFIX)/include
	$(INSTALL) -m644 libFastCompression.a $(PREFIX)/lib
	$(INSTALL) lzvn $(PREFIX)/bin

uninstall:
	$(RM) $(PREFIX)/include/FastCompression.h $(PREFIX)/lib/libFastCompression.a $(PREFIX)/bin/lzvn
