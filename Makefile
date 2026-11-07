CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS   := $(shell pkg-config --libs freetype2)

.PHONY: all clean

all: board

board: board.cpp
	$(CXX) $(CXXFLAGS) $(FREETYPE_CFLAGS) board.cpp -o $@ $(FREETYPE_LIBS)

clean:
	rm -f board
