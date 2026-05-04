PRAIA_INCLUDE := $(shell praia --include-path)
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  OUT = plugins/archive.dylib
  LDFLAGS = -undefined dynamic_lookup
  # fiber.h uses swapcontext(3) which needs _XOPEN_SOURCE/_DARWIN_C_SOURCE on macOS;
  # silencing the deprecation warning since Praia has no plans to switch off ucontext.
  EXTRA_FLAGS = -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE -Wno-deprecated-declarations
else
  OUT = plugins/archive-linux-$(shell uname -m).so
  LDFLAGS =
  EXTRA_FLAGS = -D_XOPEN_SOURCE=700
endif

all:
	g++ -std=c++17 -shared -fPIC $(EXTRA_FLAGS) -I$(PRAIA_INCLUDE) $(LDFLAGS) -o $(OUT) plugins/archive.cpp -lz

clean:
	rm -f plugins/archive.dylib plugins/archive-linux-*.so

.PHONY: all clean
