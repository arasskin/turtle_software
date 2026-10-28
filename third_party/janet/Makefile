CC = cc
BUILD ?= build
CFLAGS ?= -O2 -std=c99 -fPIC
LDLIBS = -lm -lpthread
ifeq ($(shell uname -s),Linux)
LDLIBS += -ldl
endif

.PHONY: all
all: $(BUILD)/janet

$(BUILD)/janet: janet.c janet.h shell.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. janet.c shell.c $(LDFLAGS) $(LDLIBS) -o $@
