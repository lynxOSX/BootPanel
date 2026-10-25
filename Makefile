# BootPanel — the lynxOS boot panel.
#
# The art is baked into the binary: src/assets_embed.c is generated from
# assets/ by tools/genassets.py (run `make assets` after editing the pack).
#
# KMS/DRM is compiled in when libdrm is available; disable it explicitly
# with `make KMS=0`. The legacy fbdev backend is always built and used as
# fallback at runtime.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra

KMS ?= $(shell pkg-config --exists libdrm && echo 1 || echo 0)

OBJS = src/main.o src/bootpanel.o src/backend_fbdev.o src/assets_embed.o

ifeq ($(KMS),1)
CPPFLAGS += -DHAVE_KMS=1 $(shell pkg-config --cflags libdrm)
LDLIBS   += $(shell pkg-config --libs libdrm)
OBJS += src/backend_kms.o
endif

# switching KMS changes compile flags: drop stale objects so nothing
# survives from a differently-configured build (this bit us in testing)
ifneq ($(KMS),$(shell cat .build-kms 2>/dev/null))
$(shell rm -f $(OBJS))
$(shell echo '$(KMS)' > .build-kms)
endif

all: bootpanel

bootpanel: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c src/bootpanel.h src/assets_embed.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# regenerate the baked-in assets after editing assets/ (python3 stdlib only)
assets:
	python3 tools/genassets.py

install: bootpanel
	install -Dm755 bootpanel $(DESTDIR)/sbin/bootpanel

clean:
	rm -f bootpanel $(OBJS) .build-kms

.PHONY: all assets install clean
