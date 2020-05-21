CROSS ?= aarch64-linux-gnu-

CC = $(CROSS)gcc
AR = $(CROSS)ar
PKG_CONFIG ?= pkg-config

all:

#override CFLAGS += -fsanitize=address -g -O0
#override LDFLAGS += -fsanitize=address

#
# qd helper lib
#

qd_pkgs = qap_wrapper libavformat libavcodec libavdevice libavutil
qd_includes = $(shell $(PKG_CONFIG) --cflags $(qd_pkgs))
qd_ldlibs = $(shell $(PKG_CONFIG) --libs $(qd_pkgs))

qd_objs = qd.o
qd_cppflags = -D_DEFAULT_SOURCE $(CPPFLAGS)
qd_cflags = -std=gnu11 -Wall -pthread $(qd_includes) $(CFLAGS)

$(qd_objs): %.o: %.c
	$(CC) -c $(qd_cflags) -o $@ -MD -MP -MF $(@D)/.$(@F).d $(qd_cppflags) $<

libqd.a: $(qd_objs)
	$(AR) rcsT $@ $+

targets += libqd.a
make_deps += $(patsubst %,.%.d,$(qd_objs))

#
# qapdec
#

qapdec_objs = qapdec.o
qapdec_cppflags = -D_DEFAULT_SOURCE $(CPPFLAGS)
qapdec_cflags = -std=gnu11 -Wall -pthread $(qd_includes) $(CFLAGS)
qapdec_ldflags = $(LDFLAGS) -pthread
qapdec_ldlibs = $(qd_ldlibs)

$(qapdec_objs): %.o: %.c
	$(CC) -c $(qapdec_cflags) -o $@ -MD -MP -MF $(@D)/.$(@F).d $(qapdec_cppflags) $<

qapdec: $(qapdec_objs) libqd.a
	$(CC) $(qapdec_ldflags) $+ -o $@ $(qapdec_ldlibs)

targets += qapdec
make_deps += $(patsubst %,.%.d,$(qapdec_objs))

#
# qaptest
#

qaptest_pkgs = fftw3
qaptest_pkg_cflags = $(shell $(PKG_CONFIG) --cflags $(qaptest_pkgs))
qaptest_pkg_libs = $(shell $(PKG_CONFIG) --libs $(qaptest_pkgs))

qaptest_objs = qaptest.o munit/munit.o
qaptest_cppflags = -D_DEFAULT_SOURCE $(CPPFLAGS)
qaptest_cflags = -std=gnu11 -Wall -pthread $(qd_includes) $(qaptest_pkg_cflags) $(CFLAGS)
qaptest_ldflags = $(LDFLAGS) -pthread
qaptest_ldlibs = -lm $(qd_ldlibs) $(qaptest_pkg_libs)

$(qaptest_objs): %.o: %.c
	$(CC) -c $(qaptest_cflags) -o $@ -MD -MP -MF $(@D)/.$(@F).d $(qaptest_cppflags) $<

qaptest: $(qaptest_objs) libqd.a
	$(CC) $(qaptest_ldflags) $+ -o $@ $(qaptest_ldlibs)

targets += qaptest
make_deps += $(patsubst %,.%.d,$(qaptest_objs))

#
# common rules
#

all: $(targets)

clean:
	$(RM) *.o .*.o.d $(targets)

install:

.PHONY: clean all install

-include $(make_deps)

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
