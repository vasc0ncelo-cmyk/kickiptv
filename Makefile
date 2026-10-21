# Build do PKG (rodado pelo GitHub Actions). Baseado nos samples do OpenOrbis; pode precisar de ajustes.
# Requer: OO_PS4_TOOLCHAIN, clang, ld.lld, dotnet.
.RECIPEPREFIX = >
OO  := $(OO_PS4_TOOLCHAIN)
BIN := $(OO)/bin/linux

CONTENT_ID := IV0000-BREW00000_00-HELLOWORLD000000
SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

CXXFLAGS := --target=x86_64-pc-freebsd12-elf -std=c++17 -fPIC -funwind-tables -c -isysroot $(OO) \
            -isystem $(OO)/include -isystem $(OO)/include/c++/v1
LDFLAGS  := -m elf_x86_64 -pie --script $(OO)/link.x --eh-frame-hdr -L$(OO)/lib
LIBS     := -lc -lc++ -lkernel

all: build/$(CONTENT_ID).pkg

build/%.o: src/%.cpp $(wildcard src/*.hpp)
> @mkdir -p build
> clang++ $(CXXFLAGS) -o $@ $<

build/app.elf: $(OBJS)
> ld.lld $(LDFLAGS) $(OO)/lib/crt1.o $(OBJS) $(LIBS) -o $@

build/eboot.bin: build/app.elf
> $(BIN)/create-fself -in=build/app.elf -out=build/app.oelf --eboot=build/eboot.bin --paid 0x3800000000000011

build/pkg.gp4: build/eboot.bin
> cp build/eboot.bin eboot.bin
> $(BIN)/create-gp4 -out build/pkg.gp4 --content-id=$(CONTENT_ID) \
>   --files "eboot.bin $(shell find sce_sys sce_module -type f 2>/dev/null | tr '\n' ' ')"

build/$(CONTENT_ID).pkg: build/pkg.gp4
> $(BIN)/PkgTool.Core pkg_build build/pkg.gp4 build
> @ls -la build

clean:
> rm -rf build eboot.bin
