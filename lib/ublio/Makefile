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
	$(INSTALL) ublio.h $(PREFIX)/include/
	$(INSTALL) libublio.a $(PREFIX)/lib/

uninstall:
	rm -f $(PREFIX)/include/ublio.h
