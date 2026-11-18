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

# --- Empacotamento IGUAL ao do app antigo (que abre no seu PS4) ---
# O add_self/add_pkg do toolchain usam o "paid" 0x35 + authinfo e um param.sfo (APP_TYPE 1, FORMAT obs, ATTRIBUTE 8388640, SYSTEM_VER 1020,
# categoria gde) pensados para o GMCA. No seu console isso fazia o app fechar na hora (CE-34878-0), antes mesmo do main().
# Aqui usamos exatamente os valores do build antigo: paid 0x3800000000000011, sem authinfo, APP_TYPE 0, ATTRIBUTE 0, SYSTEM_VER 0, categoria gd.
set(CONTENT_ID "IV0000-${PSN_TITLE_ID}_00-KICKIPTV00000000")
set(PKGTOOL "${OPENORBIS}/bin/linux/PkgTool.Core")
set(DOTFIX "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1")
set(PKGDIR "${CMAKE_BINARY_DIR}/ps4")
set(SFO "${PKGDIR}/sce_sys/param.sfo")

add_custom_command(
    OUTPUT "${PROJECT_NAME}.self"
    COMMAND ${CMAKE_COMMAND} -E env "OO_PS4_TOOLCHAIN=${OPENORBIS}" "${OPENORBIS}/bin/create-fself" "-in=${PROJECT_NAME}" "-out=${PROJECT_NAME}.oelf" "--eboot" "eboot.bin" "--paid" "0x3800000000000011"
    VERBATIM
    DEPENDS "${PROJECT_NAME}"
)
add_custom_target("${PROJECT_NAME}_self" ALL DEPENDS "${PROJECT_NAME}.self")

add_custom_command(
    OUTPUT "${PROJECT_NAME}.pkg"
    COMMAND ${CMAKE_COMMAND} -E copy eboot.bin ${PKGDIR}/eboot.bin
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_new ${SFO}
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} APP_TYPE --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} APP_VER --type Utf8 --maxsize 8 --value "01.00"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} ATTRIBUTE --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} CATEGORY --type Utf8 --maxsize 4 --value "gd"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} CONTENT_ID --type Utf8 --maxsize 48 --value "${CONTENT_ID}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} SYSTEM_VER --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} TITLE --type Utf8 --maxsize 128 --value "Kick IPTV mpv"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} TITLE_ID --type Utf8 --maxsize 12 --value "${PSN_TITLE_ID}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${SFO} VERSION --type Utf8 --maxsize 8 --value "01.00"
    COMMAND "${OPENORBIS}/bin/linux/create-gp4" -out ${PKGDIR}/${PROJECT_NAME}.gp4 --content-id "${CONTENT_ID}" --path "${PKGDIR}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} pkg_build ${PKGDIR}/${PROJECT_NAME}.gp4 ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove ${PKGDIR}/eboot.bin
    COMMAND ${CMAKE_COMMAND} -E remove ${SFO}
    COMMAND ${CMAKE_COMMAND} -E remove ${PKGDIR}/${PROJECT_NAME}.gp4
    VERBATIM
    DEPENDS "${PROJECT_NAME}.self"
)
add_custom_target("${PROJECT_NAME}_pkg" ALL DEPENDS "${PROJECT_NAME}.pkg")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/ps4pkg/sce_module ${CMAKE_BINARY_DIR}/ps4/sce_module
    COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/ps4pkg/sce_sys ${CMAKE_BINARY_DIR}/ps4/sce_sys
)
