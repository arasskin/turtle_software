CC = cc
BUILD ?= build
FLAGS = -std=c99 -pedantic-errors -Wall -Wextra -Werror
.PHONY: test sanitize
$(BUILD)/test: tests/containers_test.c $(wildcard *.h)
	@mkdir -p $(@D)
	$(CC) $(FLAGS) -O2 -g $< -o $@
test: $(BUILD)/test
	./$(BUILD)/test
$(BUILD)/sanitize: tests/containers_test.c $(wildcard *.h)
	@mkdir -p $(@D)
	$(CC) $(FLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $< -o $@
sanitize: $(BUILD)/sanitize
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/sanitize
