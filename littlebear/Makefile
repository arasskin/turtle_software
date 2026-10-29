CC = cc
CXX = g++
CFLAGS = -std=c99 -pedantic-errors -Wall -Wextra $(NIX_CFLAGS_COMPILE)
BUILD ?= build
CPPFLAGS += -I. -Itests
CXXFLAGS = -std=c++23 -Wall -Wextra $(NIX_CFLAGS_COMPILE)
LDFLAGS ?=
ASAN_OPTIONS ?= quarantine_size_mb=16:thread_local_quarantine_size_kb=64
SANITIZERS = -fsanitize=address,undefined -fno-omit-frame-pointer
# Compiler-generated dependencies rebuild only affected targets, including shared headers.
DEPFLAGS = -MMD -MP
TEST_NAMES := size_class_test wide_tree_test hash_table_test storage_test reconciliation_test index_sync_test
TESTS := $(addprefix $(BUILD)/tests/,$(TEST_NAMES)) $(BUILD)/tests/c99_test $(BUILD)/tests/connections_test $(BUILD)/tests/delivery_test $(BUILD)/tests/batch_write_test $(BUILD)/tests/websocket_test
SANITIZER_TESTS := $(addprefix $(BUILD)/sanitize/,$(TEST_NAMES)) $(BUILD)/sanitize/c99_test $(BUILD)/sanitize/connections_test $(BUILD)/sanitize/delivery_test $(BUILD)/sanitize/batch_write_test $(BUILD)/sanitize/websocket_test

.PHONY: all server debug test sanitize architecture clean
all: server
server: $(BUILD)/server
$(BUILD)/server: server.c Makefile
	@mkdir -p $(@D) $(BUILD)/deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(BUILD)/deps/$(@F).d -O3 -DNDEBUG -o $@ $< $(LDFLAGS) -luring -lsodium

debug: $(BUILD)/server-debug
$(BUILD)/server-debug: server.c Makefile
	@mkdir -p $(@D) $(BUILD)/deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -MF $(BUILD)/deps/$(@F).d -O1 -g $(SANITIZERS) -o $@ $< $(LDFLAGS) -luring -lsodium

$(BUILD)/tests/c99_test $(BUILD)/tests/batch_write_test $(BUILD)/tests/websocket_test: $(BUILD)/tests/%: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -O2 -g -o $@ $< $(LDFLAGS)
$(BUILD)/sanitize/c99_test $(BUILD)/sanitize/batch_write_test $(BUILD)/sanitize/websocket_test: $(BUILD)/sanitize/%: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -O1 -g $(SANITIZERS) -o $@ $< $(LDFLAGS)

$(BUILD)/tests/connections_test $(BUILD)/tests/delivery_test: $(BUILD)/tests/%: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -O2 -g -o $@ $< $(LDFLAGS) -lsodium
$(BUILD)/sanitize/connections_test $(BUILD)/sanitize/delivery_test: $(BUILD)/sanitize/%: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -O1 -g $(SANITIZERS) -o $@ $< $(LDFLAGS) -lsodium

$(BUILD)/tests/%: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -O2 -g -o $@ $< $(LDFLAGS) $(TEST_LIBS)
$(BUILD)/sanitize/%: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -O1 -g $(SANITIZERS) -o $@ $< $(LDFLAGS) $(TEST_LIBS)
ifeq ($(shell uname -s),Linux)
$(BUILD)/tests/storage_test $(BUILD)/sanitize/storage_test: TEST_LIBS = -luring
endif

architecture:
	python3 ../scripts/check_littlebear_dependencies.py

test: architecture $(TESTS)
	@set -e; for test in $(TESTS); do "$$test"; done
sanitize: architecture $(SANITIZER_TESTS)
	@set -e; for test in $(SANITIZER_TESTS); do ASAN_OPTIONS="$(ASAN_OPTIONS)" UBSAN_OPTIONS=halt_on_error=1 "$$test"; done

clean:
	rm -rf $(BUILD)
# Keep server depfiles separate from pre-flattening build metadata, whose
# removed src/server.c prerequisite would otherwise block incremental builds.
-include $(wildcard $(BUILD)/deps/server.d $(BUILD)/deps/server-debug.d $(BUILD)/tests/*.d $(BUILD)/sanitize/*.d)
