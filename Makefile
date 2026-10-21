# Build do PKG (rodado pelo GitHub Actions). Baseado nos samples do OpenOrbis.
# Requer: OO_PS4_TOOLCHAIN, clang, ld.lld, dotnet.
.RECIPEPREFIX = >
OO  := $(OO_PS4_TOOLCHAIN)
BIN := $(OO)/bin/linux
PKGTOOL := $(BIN)/PkgTool.Core

# IDs proprios (o Title ID tem 9 caracteres; o Content ID tem 36)
CONTENT_ID := IV0000-KICK00001_00-KICKIPTV00000000
TITLE_ID := KICK00001
TITLE := Kick IPTV
SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

CXXFLAGS := --target=x86_64-pc-freebsd12-elf -std=c++17 -D_GNU_SOURCE -fPIC -funwind-tables -c -isysroot $(OO) \
            -isystem $(OO)/include -isystem $(OO)/include/c++/v1
LDFLAGS  := -m elf_x86_64 -pie --script $(OO)/link.x --eh-frame-hdr -L$(OO)/lib
LIBS     := -lc -lc++ -lkernel -lSceVideoOut -lSceUserService -lScePad -lSceSysmodule -lSceNet -lSceSsl -lSceHttp

all: build/$(CONTENT_ID).pkg

build/%.o: src/%.cpp $(wildcard src/*.hpp)
> @mkdir -p build
> clang++ $(CXXFLAGS) -o $@ $<

build/app.elf: $(OBJS)
> ld.lld $(LDFLAGS) $(OO)/lib/crt1.o $(OBJS) $(LIBS) -o $@

build/eboot.bin: build/app.elf
> $(BIN)/create-fself -in=build/app.elf -out=build/app.oelf --eboot=build/eboot.bin --paid 0x3800000000000011

# Sempre regenera o param.sfo (o do sample copiado pelo script teria o ID antigo)
.PHONY: sfo
sfo:
> rm -f sce_sys/param.sfo
> $(PKGTOOL) sfo_new sce_sys/param.sfo
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo APP_TYPE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo APP_VER --type Utf8 --maxsize 8 --value '01.00'
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo ATTRIBUTE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo CATEGORY --type Utf8 --maxsize 4 --value 'gd'
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo SYSTEM_VER --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
> $(PKGTOOL) sfo_setentry sce_sys/param.sfo VERSION --type Utf8 --maxsize 8 --value '01.00'

pkg.gp4: build/eboot.bin sfo
> cp build/eboot.bin eboot.bin
> $(BIN)/create-gp4 -out pkg.gp4 --content-id=$(CONTENT_ID) --files "eboot.bin $$(find sce_sys sce_module -type f ! -name .gitkeep | tr '\n' ' ')"

build/$(CONTENT_ID).pkg: pkg.gp4
> $(PKGTOOL) pkg_build pkg.gp4 .
> mv $(CONTENT_ID).pkg build/
> @ls -la build

clean:
> rm -rf build eboot.bin pkg.gp4 sce_sys/param.sfo
