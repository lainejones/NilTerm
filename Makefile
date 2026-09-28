# NilTerm - an ANSI telnet client for the Amiga.  Build under WSL/Linux with the bebbo
# amiga-gcc toolchain on PATH:   make      -> out/NilTerm
# The sources are NilBBS's (github.com/lainejones/NilBBS): NilTerm is its sysop terminal,
# and its file transfers are the BBS's own ZMODEM / X/YMODEM code (src/node).
CC      = m68k-amigaos-gcc
VERSION = $(shell cat VERSION)
VERDATE = $(shell date +%-d.%-m.%Y)
CFLAGS  = -m68020 -O2 -noixemul -Wall -Wno-pointer-sign -Wno-format -fomit-frame-pointer -s -Isrc/common
CFLAGS += -DBBS_VERSION='"$(VERSION)"' -DBBS_VERDATE='"$(VERDATE)"'
SRCS    = src/nilterm/nilterm.c src/nilterm/ntzm.c src/nilterm/ntxy.c \
          src/common/util.c src/common/shared.c src/common/cfg.c

out/NilTerm: $(SRCS) src/nilterm/*.h src/node/zmodem.c src/node/xymodem.c src/node/*.h src/common/*.h VERSION
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf out

.PHONY: clean
