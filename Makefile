CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
THREAD_FLAGS := -pthread

FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS   := $(shell pkg-config --libs freetype2)

.PHONY: all clean test

all: board

board: board.cpp
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) $(FREETYPE_CFLAGS) board.cpp -o $@ $(FREETYPE_LIBS) $(THREAD_FLAGS)

test: board
	./tests/run.sh

clean:
	rm -f board
