# Build do Kick IPTV com libmpv, na MESMA toolchain do GMCA (container xfangfang/pacbrew: OpenOrbis + musl).
# Usado so pelo workflow ".github/workflows/build-mpv.yml". O build antigo (Makefile) continua igual.
cmake_minimum_required(VERSION 3.18)

if (NOT DEFINED ENV{OPENORBIS})
    message(FATAL_ERROR "defina OPENORBIS (ex.: /opt/pacbrew/ps4/openorbis)")
endif ()
set(OPENORBIS $ENV{OPENORBIS} CACHE PATH "OPENORBIS")
set(CMAKE_TOOLCHAIN_FILE "${OPENORBIS}/cmake/ps4.cmake" CACHE PATH "toolchain file")

project(KickIPTV C CXX)
set(PSN_TITLE_ID "KICK00002")      # id diferente do app antigo (KICK00001): os dois podem ficar instalados
set(APP_VERSION "01.00")
set(PS4_SFO_ATTRIBUTE 8388640)     # igual ao GMCA

add_definitions(-DPS4 -D__PS4__ -DUSE_LIBMPV)

file(GLOB APP_SRC ${CMAKE_SOURCE_DIR}/src/*.cpp)
set(PS4_C_SRC ${CMAKE_SOURCE_DIR}/ps4c/cxa_guard.c ${CMAKE_SOURCE_DIR}/ps4c/clock_ids.c)

# O link.x do OpenOrbis junta `.data` mas nao `.data.*`; com -fdata-sections isso zera globais no console.
# Mesmo remendo do GMCA: usa uma copia do link.x que junta os dois.
file(READ ${OPENORBIS}/link.x PS4_LINK_SCRIPT)
string(REGEX REPLACE "\\*\\(\\.data\\)" "*(.data .data.*)" PS4_LINK_SCRIPT_FIXED "${PS4_LINK_SCRIPT}")
if (PS4_LINK_SCRIPT_FIXED STREQUAL PS4_LINK_SCRIPT)
    message(FATAL_ERROR "nao consegui remendar ${OPENORBIS}/link.x")
endif ()
file(WRITE ${CMAKE_BINARY_DIR}/link.x "${PS4_LINK_SCRIPT_FIXED}")
string(REPLACE "${OPENORBIS}/link.x" "${CMAKE_BINARY_DIR}/link.x" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(FIND "${CMAKE_EXE_LINKER_FLAGS}" "${CMAKE_BINARY_DIR}/link.x" PS4_LINK_SCRIPT_FLAG)
if (PS4_LINK_SCRIPT_FLAG EQUAL -1)
    message(FATAL_ERROR "as flags de link nao citam ${OPENORBIS}/link.x: ${CMAKE_EXE_LINKER_FLAGS}")
endif ()

add_executable(${PROJECT_NAME} ${APP_SRC} ${PS4_C_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE ${OPENORBIS}/usr/include)
target_compile_options(${PROJECT_NAME} PRIVATE
    -O2 -fno-strict-aliasing -fno-omit-frame-pointer -funwind-tables -D_GNU_SOURCE)
target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--wrap=clock_gettime")
target_link_directories(${PROJECT_NAME} PRIVATE ${OPENORBIS}/usr/lib)
# Sem -Wl,--start-group aqui: o ld.lld e chamado direto (nao pelo clang) e nao entende '-Wl,'; o cmake do toolchain ja envolve tudo em --start-group/--end-group.
# Ordem: o mpv puxa ffmpeg, mbedtls, SDL2 (audio), libass etc. (lista = Libs.private do mpv.pc do pacote)
target_link_libraries(${PROJECT_NAME} PRIVATE
    mpv avformat avcodec avfilter swscale swresample avutil dav1d mbedtls mbedx509 mbedcrypto
    ass fribidi freetype bz2 png16 samplerate SDL2 z
    SceVideoOut SceUserService SceSystemService SceSysUtil ScePad SceSysmodule SceNet SceSsl SceHttp SceAvPlayer SceAudioOut
    ScePigletv2VSH SceLibcInternal rt pthread m
)

add_self(${PROJECT_NAME})
add_pkg(${PROJECT_NAME}
        ${CMAKE_BINARY_DIR}/ps4
        ${PSN_TITLE_ID}
        "Kick IPTV mpv"
        ${APP_VERSION}
        ${PS4_SFO_ATTRIBUTE})
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/ps4pkg/sce_module ${CMAKE_BINARY_DIR}/ps4/sce_module
    COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/ps4pkg/sce_sys ${CMAKE_BINARY_DIR}/ps4/sce_sys
)
