BUILD ?= build/native
JOBS ?= 2
.DEFAULT_GOAL := native
.PHONY: native test native-test sanitize clean
native:
	python3 tool/generate_attributes.py --check
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j $(JOBS)
test native-test: native
	ctest --test-dir $(BUILD) --output-on-failure
sanitize:
	cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
	cmake --build build/sanitize -j $(JOBS)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/sanitize --output-on-failure --timeout 180
clean:
	rm -rf $(BUILD)
