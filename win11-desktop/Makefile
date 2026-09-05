# =====================================================================
#  Simple Makefile fallback (no CMake needed).  Linux/WSL:
#    sudo apt install libsdl2-dev
#    make            -> build/win11desktop
#    make run        -> run locally
#    make selftest   -> headless screenshot mode (SDL_VIDEODRIVER=dummy)
# =====================================================================
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc
LDLIBS   := $(shell pkg-config --cflags --libs sdl2 2>/dev/null)
LDFLAGS  += $(LDLIBS)

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)
BIN := build/win11desktop

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build:
	@mkdir -p build

run: $(BIN)
	./$(BIN)

selftest: $(BIN)
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$(BIN) --selftest

clean:
	rm -rf build

.PHONY: all run selftest clean
