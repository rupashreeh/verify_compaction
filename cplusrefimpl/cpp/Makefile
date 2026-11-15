CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter
SRC := $(wildcard src/*.cc)
OBJ := $(patsubst src/%.cc,build/%.o,$(SRC))
TESTS := $(patsubst tests/%.cc,build/%,$(filter-out tests/test_main.cc,$(wildcard tests/test_*.cc)))

all: $(TESTS)

build/%.o: src/%.cc $(wildcard src/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

build/test_%: tests/test_%.cc tests/harness.h $(OBJ)
	$(CXX) $(CXXFLAGS) -Isrc -Itests $< $(OBJ) -o $@

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

clean:
	rm -rf build
.PHONY: all test clean
