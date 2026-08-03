# these can (and should) be overridden on the make command line for production
CFLAGS := -std=gnu99
# these are used for the benchmarks in addition to the normal CFLAGS. 
# Normally no need to overwrite unless you find a new magic flag to make
# STREAM run faster.
BENCH_CFLAGS := -O3 -ffast-math -funroll-loops
# for compatibility with old releases
CFLAGS += ${OPT_CFLAGS}
override CFLAGS += -I.

# find out if compiler supports -ftree-vectorize
VECTORIZE_SUPPORT := $(shell touch empty.c ; if $(CC) $(CFLAGS) -c -ftree-vectorize empty.c -o empty.o \
			>/dev/null 2>/dev/null ; then echo "yes" ; else echo "no"; fi)
ifeq ($(VECTORIZE_SUPPORT),yes)
	BENCH_CFLAGS += -ftree-vectorize
endif

CLEANFILES := numad.o numad .depend .depend.X empty.c empty.o

SOURCES := numad.c

prefix := /usr
docdir := ${prefix}/share/doc

all: numad

LDLIBS := -lpthread -lrt -lm

AMDSMI_CFLAGS ?=
AMDSMI_LIBS ?= -lamd_smi

ifeq ($(HAVE_AMDSMI),1)
override CFLAGS += -DHAVE_AMDSMI $(AMDSMI_CFLAGS)
LDLIBS += $(AMDSMI_LIBS)
endif

numad: numad.o

AR ?= ar
RANLIB ?= ranlib

.PHONY: install all clean html depend

# BB_FIXME MANPAGES := numa.3 numactl.8 numastat.8 migratepages.8 migspeed.8

install: numad
	mkdir -p $(DESTDIR)${prefix}/bin
	mkdir -p $(DESTDIR)${prefix}/lib/numad
	mkdir -p $(DESTDIR)${prefix}/share/man/man8
	mkdir -p $(DESTDIR)${prefix}/lib/systemd/system
	mkdir -p $(DESTDIR)/etc/init.d
	mkdir -p $(DESTDIR)/etc
	install -m 0755 numad $(DESTDIR)${prefix}/bin
	install -m 0755 numad-wrapper $(DESTDIR)${prefix}/lib/numad
	install -m 0644 numad.service $(DESTDIR)${prefix}/lib/systemd/system/numad.service
	install -m 0755 numad.init $(DESTDIR)/etc/init.d/numad
	install -m 0644 numad.conf $(DESTDIR)/etc/numad.conf
	install -m 0644 numad.8 $(DESTDIR)${prefix}/share/man/man8

clean: 
	rm -f ${CLEANFILES}
	@rm -rf html

distclean: clean
	rm -f .depend .depend.X
	rm -f *~ */*~ *.orig */*.orig */*.rej *.rej 

depend: .depend

.depend:
	${CC} -MM -DDEPS_RUN -I. ${SOURCES} > .depend.X && mv .depend.X .depend

-include .depend

Makefile: .depend

