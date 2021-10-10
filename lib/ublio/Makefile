SRC = ublio.c

OUT = libublio.a

OBJ = $(SRC:.c=.o)

CFLAGS := -g -O2 -Wall $(CFLAGS)

CC ?= cc

AR ?= ar

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJ)
	$(AR) rcs $(OUT) $(OBJ)

clean:
	rm -f $(OBJ) $(OUT)

install: $(OUT)
	mkdir -pm755 $(DESTDIR)$(includedir) $(DESTDIR)$(libdir)
	$(INSTALL) -m644 ublio.h $(DESTDIR)$(includedir)
	$(INSTALL) -m644 libublio.a $(DESTDIR)$(libdir)

uninstall:
	rm -f $(DESTDIR)$(includedir)/ublio.h $(DESTDIR)$(libdir)/libublio.a
