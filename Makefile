# Build do PKG (GitHub Actions / local). OpenOrbis ou PacBrew.
.RECIPEPREFIX = >
OO  := $(OO_PS4_TOOLCHAIN)
BIN := $(OO)/bin/linux
PKGTOOL := $(BIN)/PkgTool.Core

CONTENT_ID := IV0000-KICK00001_00-KICKIPTV00000000
TITLE_ID := KICK00001
TITLE := Kick IPTV
SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,build/%.o,$(SRCS))

# mpv (igual GMCA): make USE_LIBMPV=1 PACBREW=/caminho/openorbis
USE_LIBMPV ?= 0
PACBREW  ?=

CXXFLAGS := --target=x86_64-pc-freebsd12-elf -std=c++17 -D_GNU_SOURCE -fPIC -funwind-tables -c \
            -isysroot $(OO) -isystem $(OO)/include -isystem $(OO)/include/c++/v1
LDFLAGS  := -m elf_x86_64 -pie --script $(OO)/link.x --eh-frame-hdr -L$(OO)/lib
LIBS     := -lc -lc++ -lkernel -lSceVideoOut -lSceUserService -lSceSystemService -lScePad \
            -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lSceAvPlayer -lSceAudioOut

ifeq ($(USE_LIBMPV),1)
  ifeq ($(PACBREW),)
    $(error USE_LIBMPV=1 requer PACBREW= prefixo OpenOrbis PacBrew)
  endif
  CXXFLAGS += -DUSE_LIBMPV -I$(PACBREW)/usr/include
  LDFLAGS  += -L$(PACBREW)/usr/lib
  # Ordem importa: mpv puxa ffmpeg, mbedtls, sdl2, dav1d...
  LIBS     += -lmpv -lavformat -lavcodec -lavfilter -lswscale -lswresample -lavutil \
              -ldav1d -lmbedtls -lmbedx509 -lmbedcrypto -lSDL2 -lass -lfribidi -lfreetype \
              -lpng16 -lbz2 -lz -lsamplerate -lm -lpthread
endif

all: build/$(CONTENT_ID).pkg

build/%.o: src/%.cpp $(wildcard src/*.hpp)
> @mkdir -p build
> clang++ $(CXXFLAGS) -o $@ $<

build/app.elf: $(OBJS)
> ld.lld $(LDFLAGS) $(OO)/lib/crt1.o $(OBJS) $(LIBS) -o $@

build/eboot.bin: build/app.elf
> $(BIN)/create-fself -in=build/app.elf -out=build/app.oelf --eboot=build/eboot.bin --paid 0x3800000000000011

build/sce_sys/param.sfo:
> @mkdir -p build/sce_sys
> $(PKGTOOL) sfo_new $@
> $(PKGTOOL) sfo_setentry $@ APP_VER --type Integer --maxsize 4 --value 1
> $(PKGTOOL) sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value "gd"
> $(PKGTOOL) sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value "$(CONTENT_ID)"
> $(PKGTOOL) sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
> $(PKGTOOL) sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value "$(TITLE)"
> $(PKGTOOL) sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value "$(TITLE_ID)"
> $(PKGTOOL) sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value "01.00"

build/$(CONTENT_ID).pkg: build/eboot.bin build/sce_sys/param.sfo
> @mkdir -p build/pkg/sce_sys
> cp build/eboot.bin build/pkg/eboot.bin
> cp build/sce_sys/param.sfo build/pkg/sce_sys/
> if [ -f sce_sys/icon0.png ]; then cp sce_sys/icon0.png build/pkg/sce_sys/; fi
> $(BIN)/create-gp4 -out build/pkg.gp4 -content-id $(CONTENT_ID) -title "$(TITLE)" -path build/pkg -files eboot.bin sce_sys/param.sfo $$(test -f build/pkg/sce_sys/icon0.png && echo sce_sys/icon0.png)
> $(PKGTOOL) pkg_build build/pkg.gp4 build/
> @echo "PKG pronto: build/$(CONTENT_ID).pkg"

clean:
> rm -rf build

.PHONY: all clean
