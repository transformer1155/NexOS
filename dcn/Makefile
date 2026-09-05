# DCN standalone library — host test build (mingw-w64)
# Build:  make            (uses x86_64-w64-mingw32-g++.exe from mingw-w64)
# Run:    ./dcn_test.exe

CXX     ?= x86_64-w64-mingw32-g++.exe
CFLAGS  ?= -std=c++14 -O2 -Wall -Idcn
SRC      = dcn_crypto.cpp dcn_transport.cpp dcn_discovery.cpp \
           dcn_chunk.cpp dcn_op.cpp dcn_sched.cpp dcn_wifi.cpp dcn_kernel.cpp tests/test_main.cpp

.PHONY: all clean
all: dcn_test.exe

dcn_test.exe: $(SRC)
	$(CXX) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f dcn_test.exe
