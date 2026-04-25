PRAIA_INCLUDE := $(shell praia --include-path)
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  EXT = .dylib
  LDFLAGS = -undefined dynamic_lookup
else
  EXT = .so
  LDFLAGS =
endif

all:
	g++ -std=c++17 -shared -fPIC -I$(PRAIA_INCLUDE) $(LDFLAGS) -o plugins/archive$(EXT) plugins/archive.cpp -lz

clean:
	rm -f plugins/archive.dylib plugins/archive.so

.PHONY: all clean
