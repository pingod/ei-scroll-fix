# ei-scroll-fix
# SPDX-License-Identifier: MIT

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
PREFIX  ?= $(HOME)/.local
BUILD   := build

SHIM    := $(BUILD)/ei_scroll_fix.so
TESTSRC := test/test_shim.c test/ei_stub.c
TESTBIN := $(BUILD)/test_shim
STUBLIB := $(BUILD)/libei_stub.so

.PHONY: all shim test check install uninstall clean dist

all: shim

# ---------------------------------------------------------------- shim -------

shim: $(SHIM)

$(SHIM): src/ei_scroll_fix.c | $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl -lm

# ---------------------------------------------------------------- tests ------

# The shim overrides libei's accessors, so the test binary links against a stub
# libei that returns values we control, and the shim is LD_PRELOADed on top.
# That makes the interposition real: the test exercises the exact same symbol
# resolution path synergy-core uses.
$(STUBLIB): test/ei_stub.c | $(BUILD)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

$(TESTBIN): test/test_shim.c test/ei_stub.h $(STUBLIB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/test_shim.c \
		-L$(BUILD) -lei_stub -Wl,-rpath,'$$ORIGIN'

test: $(TESTBIN) $(SHIM)
	LD_PRELOAD=$(abspath $(SHIM)) ./$(TESTBIN)

# ---------------------------------------------------------------- meta -------

check: build-and-test

build-and-test: test

install: $(SHIM)
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

dist: clean
	@mkdir -p $(BUILD)
	@ver=$$(git describe --tags --always --dirty 2>/dev/null || echo dev); \
	tar --exclude-vcs --exclude=./$(BUILD) \
	    --transform "s,^\.,ei-scroll-fix-$$ver," \
	    -czf $(BUILD)/ei-scroll-fix-$$ver.tar.gz . ; \
	echo "wrote $(BUILD)/ei-scroll-fix-$$ver.tar.gz"

clean:
	rm -rf $(BUILD)

$(BUILD):
	@mkdir -p $@
