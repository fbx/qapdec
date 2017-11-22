CROSS ?= aarch64-linux-gnu-

CC = $(CROSS)gcc
AR = $(CROSS)ar rc
PKG_CONFIG ?= pkg-config

SOURCES = qapdec.c
OBJECTS := $(SOURCES:.c=.o)
EXEC = qapdec
PKGS = qap_wrapper libavformat libavcodec libavutil

cppflags = -D_DEFAULT_SOURCE $(CPPFLAGS)
cflags = -std=gnu11 -Wall -pthread \
  $(shell $(PKG_CONFIG) --cflags $(PKGS)) \
  $(CFLAGS)

ldflags = $(LDFLAGS)
ldlibs = $(shell $(PKG_CONFIG) --libs $(PKGS))

all: $(EXEC)

%.o: %.c
	$(CC) -c $(cflags) -o $@ -MD -MP -MF $(@D)/.$(@F).d $(cppflags) $<

$(EXEC): $(OBJECTS)
	$(CC) $(ldflags) -o $(EXEC) $(OBJECTS) $(ldlibs)

clean:
	$(RM) *.o $(EXEC)

install:

.PHONY: clean all install

-include $(patsubst %,.%.d,$(OBJECTS))

GIT_VERSION = $(shell git describe --dirty --tags --always)
GIT_COMMIT_DATE = $(shell git log -1 --format=%cd)

.PHONY: .git-version
.git-version:
	v='$(GIT_VERSION)'; echo "$$v" | cmp -s - $@ || echo "$$v" > $@

.PHONY: .git-commitdate
.git-commitdate:
	v='$(GIT_COMMIT_DATE)'; echo "$$v" | cmp -s - $@ || echo "$$v" > $@

version.h: .git-version .git-commitdate
	v=`cat .git-version`; echo "#define VERSION \"$$v\"" > $@
	v=`cat .git-commitdate`; echo "#define DATE \"$$v\"" >> $@

qapdec.o: version.h
