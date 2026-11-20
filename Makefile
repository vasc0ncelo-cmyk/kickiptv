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

# -O2: sem otimizacao o app desenha ~10x mais devagar (o log mostrava "otimizado=NAO", menu com 130 ms por quadro e video travando).
# Mesmas flags do CMakeLists.txt (-fno-strict-aliasing e frame pointer mantidos por seguranca).
OPTFLAGS := -O2 -fno-strict-aliasing -fno-omit-frame-pointer
CXXFLAGS := --target=x86_64-pc-freebsd12-elf -std=c++17 $(OPTFLAGS) -D_GNU_SOURCE -fPIC -funwind-tables -c \
            -isysroot $(OO) -isystem $(OO)/include -isystem $(OO)/include/c++/v1
LDFLAGS  := -m elf_x86_64 -pie --script $(OO)/link.x --eh-frame-hdr -L$(OO)/lib
LIBS     := -lc -lc++ -lkernel -lSceVideoOut -lSceUserService -lSceSystemService -lScePad \
            -lSceSysmodule -lSceNet -lSceSsl -lSceHttp -lSceAvPlayer -lSceAudioOut

COBJS :=
ifeq ($(USE_LIBMPV),1)
  ifeq ($(PACBREW),)
    $(error USE_LIBMPV=1 requer PACBREW= prefixo OpenOrbis PacBrew)
  endif
  CXXFLAGS += -DUSE_LIBMPV -I$(PACBREW)/usr/include
  # As libs do PacBrew foram feitas com headers musl: o clock_gettime precisa do remendo do GMCA (ps4c/clock_ids.c) e o guard de statics (ps4c/cxa_guard.c)
  LDFLAGS  += -L$(PACBREW)/usr/lib --wrap=clock_gettime --allow-multiple-definition
  COBJS    := build/cxa_guard.o build/clock_ids.o
  # Ordem importa: mpv puxa ffmpeg, mbedtls, sdl2, dav1d... (o --start-group abaixo resolve dependencias circulares)
  LIBS     += -lmpv -lavformat -lavcodec -lavfilter -lswscale -lswresample -lavutil \
              -ldav1d -lmbedtls -lmbedx509 -lmbedcrypto -lSDL2 -lass -lfribidi -lfreetype \
              -lpng16 -lbz2 -lz -lsamplerate -lm -lpthread
  # Bibliotecas de sistema extras que o SDL2/ffmpeg importam; so entram se o toolchain tiver o stub
  OPT_LIBS := $(foreach l,SceSysUtil ScePigletv2VSH SceLibcInternal rt,$(if $(wildcard $(OO)/lib/lib$(l).*),-l$(l)))
  $(info libs de sistema extras encontradas: $(OPT_LIBS))
  LIBS     += $(OPT_LIBS)
  # Todas as outras bibliotecas estaticas do PacBrew (libass, freetype, harfbuzz, png, zlib...). Dentro do --start-group a ordem nao importa
  # e o lld so puxa o que e usado. Fora da lista: as que o OpenOrbis ja fornece (libc, libm, pthread...) para nao misturar com a musl.
  EXCL := c m pthread rt dl util crypt resolv xnet c++ c++abi unwind mpv avformat avcodec avfilter swscale swresample avutil dav1d mbedtls mbedx509 mbedcrypto SDL2 SDL2main
  EXTRA_A := $(filter-out $(EXCL),$(patsubst lib%.a,%,$(notdir $(wildcard $(PACBREW)/usr/lib/lib*.a))))
  $(info bibliotecas extras do PacBrew: $(EXTRA_A))
  LIBS     += $(addprefix -l,$(EXTRA_A))
endif

all: build/$(CONTENT_ID).pkg

build/%.o: src/%.cpp $(wildcard src/*.hpp)
> @mkdir -p build
> clang++ $(CXXFLAGS) -o $@ $<

build/%.o: ps4c/%.c
> @mkdir -p build
> clang --target=x86_64-pc-freebsd12-elf $(OPTFLAGS) -D_GNU_SOURCE -fPIC -funwind-tables -c -isysroot $(OO) -isystem $(OO)/include -o $@ $<

build/app.elf: $(OBJS) $(COBJS)
> ld.lld $(LDFLAGS) $(OO)/lib/crt1.o $(OBJS) $(COBJS) --start-group $(LIBS) --end-group -o $@

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
