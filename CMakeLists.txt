cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set (needs the softfp toolchain)")
  endif()
endif()

project(aclockdown C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Batman AC Lockdown")
set(VITA_TITLEID  "ACLD00001")
set(VITA_VPKNAME  "aclockdown")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(DATA_PATH "ux0:data/aclockdown/" CACHE STRING "Game data folder (trailing slash)")
set(LIVEAREA_DIR "${CMAKE_SOURCE_DIR}/livearea" CACHE PATH "Folder with icon0/pic0/bg0/startup/template.xml")
set(PSVITAIP "" CACHE STRING "Vita IP for the send target")
set(BUILD_TAG "dev" CACHE STRING "Build name for the first log line (hdN)")

option(VERBOSE_LOG "Log every file and JNI call" OFF)
option(VITA3K "Load the .so through a VM domain instead of kubridge (for Vita3K)" OFF)
option(DEBUG_SHOTS "Log fps every 5s" OFF)
option(VITAGL_DEBUG "Build vitaGL with GL error logging" OFF)
option(GL_TRACE "Log the GL calls of a couple of frames" OFF)
option(HANG_DUMP "Crash on purpose after 30s without progress to get a coredump" OFF)
option(PERF_TRACE "Log fps, hitches, draw calls and per-thread cpu every second" OFF)
option(INPUT_TRACE "Log touches and what the game hud does with them" OFF)
option(LEAK_TRACE "Track every engine allocation by caller, print the biggest holders on level loads" OFF)

add_definitions(-DDATA_PATH="${DATA_PATH}" -DBUILD_TAG="${BUILD_TAG}")
if(VITA3K)
  add_definitions(-DSO_UTIL_VM_DOMAIN)
endif()
if(GL_TRACE)
  add_definitions(-DGL_TRACE)
endif()
if(DEBUG_SHOTS)
  add_definitions(-DDEBUG_SHOTS)
endif()
if(HANG_DUMP)
  add_definitions(-DHANG_DUMP)
endif()
if(PERF_TRACE)
  add_definitions(-DPERF_TRACE)
endif()
if(INPUT_TRACE)
  add_definitions(-DINPUT_TRACE)
endif()
if(LEAK_TRACE)
  add_definitions(-DLEAK_TRACE)
endif()
if(VERBOSE_LOG)
  add_definitions(-DVERBOSE_LOG -DFALSOJNI_DEBUGLEVEL=0)
else()
  add_definitions(-DFALSOJNI_DEBUGLEVEL=3)
endif()

set(COMMON_FLAGS "-mfloat-abi=softfp -Wl,-q -O2 -g -fno-optimize-sibling-calls -D_GNU_SOURCE -D__POSIX_VISIBLE=999999")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS} -std=gnu11 -Wall -Wno-unused-variable -Wno-unused-function")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -std=gnu++17 -fno-exceptions")

# vitaGL and vitaShaRK are built from the submodules so the ABI and flags always match
set(VITASHARK_DIR ${CMAKE_SOURCE_DIR}/lib/vitashark)
set(VITASHARK_LIB ${VITASHARK_DIR}/libvitashark.a)
add_custom_command(OUTPUT ${VITASHARK_LIB}
  COMMAND make -C ${VITASHARK_DIR} -j8 libvitashark.a
  COMMENT "Building vitaShaRK")
add_custom_target(vitashark_build DEPENDS ${VITASHARK_LIB})
add_library(vitashark_local STATIC IMPORTED)
set_target_properties(vitashark_local PROPERTIES IMPORTED_LOCATION ${VITASHARK_LIB})

set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)
if(VITAGL_DEBUG)
  set(VITAGL_FLAGS SOFTFP_ABI=1 LOG_ERRORS=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1)
else()
  set(VITAGL_FLAGS SOFTFP_ABI=1 NO_DEBUG=1 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1)
endif()
# the library builds in-tree, so a flag change has to force a clean rebuild
set(VITAGL_STAMP ${VITAGL_DIR}/.flags)
if(EXISTS ${VITAGL_STAMP})
  file(READ ${VITAGL_STAMP} VITAGL_OLD_FLAGS)
endif()
if(NOT "${VITAGL_OLD_FLAGS}" STREQUAL "${VITAGL_FLAGS}")
  file(WRITE ${VITAGL_STAMP} "${VITAGL_FLAGS}")
endif()
add_custom_command(OUTPUT ${VITAGL_LIB}
  COMMAND make -C ${VITAGL_DIR} clean
  COMMAND make -C ${VITAGL_DIR} -j8 ${VITAGL_FLAGS}
  DEPENDS ${VITAGL_STAMP}
  COMMENT "Building vitaGL (softfp)")
add_custom_target(vitagl_build DEPENDS ${VITAGL_LIB})
add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES IMPORTED_LOCATION ${VITAGL_LIB})

# libdeflate release tarball (github.com/ebiggers/libdeflate), only the decompressor is built
set(LIBDEFLATE_DIR "${CMAKE_SOURCE_DIR}/../tools/libdeflate-1.26" CACHE PATH "libdeflate source")
add_library(deflate STATIC
  ${LIBDEFLATE_DIR}/lib/utils.c
  ${LIBDEFLATE_DIR}/lib/deflate_decompress.c
  ${LIBDEFLATE_DIR}/lib/zlib_decompress.c
  ${LIBDEFLATE_DIR}/lib/adler32.c
  ${LIBDEFLATE_DIR}/lib/arm/cpu_features.c
)
target_compile_options(deflate PRIVATE -mcpu=cortex-a9 -mfpu=neon -foptimize-sibling-calls -w)

add_executable(${PROJECT_NAME}
  source/main.c
  source/loader.c
  source/imports.c
  source/bionic.c
  source/pthread_bionic.c
  source/jni_impl.c
  source/gfx.c
  source/gltrace.c
  source/perf.c
  source/audio.c
  source/obb.c
  source/input.c
  source/patch.c
  source/util.c
  source/movie.c
  source/comic.c
  source/store.c
  source/zfast.c
  source/tilt.c
  lib/so_util/so_util.c
  lib/falso_jni/FalsoJNI.c
  lib/falso_jni/FalsoJNI_ImplBridge.c
  lib/falso_jni/FalsoJNI_Logger.c
  lib/falso_jni/converter.c
  lib/stb/stb_vorbis.c
)
if(LEAK_TRACE)
  target_sources(${PROJECT_NAME} PRIVATE source/leak.c)
endif()
add_dependencies(${PROJECT_NAME} vitagl_build vitashark_build)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/lib
  ${VITAGL_DIR}/source
  ${LIBDEFLATE_DIR}
)

set_source_files_properties(lib/stb/stb_vorbis.c PROPERTIES COMPILE_FLAGS "-O3 -w -DSTB_VORBIS_NO_PUSHDATA_API")
set_source_files_properties(lib/falso_jni/FalsoJNI.c lib/falso_jni/FalsoJNI_ImplBridge.c
  lib/falso_jni/FalsoJNI_Logger.c lib/falso_jni/converter.c lib/so_util/so_util.c
  PROPERTIES COMPILE_FLAGS "-w")

target_link_libraries(${PROJECT_NAME}
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  vitaGL
  vitashark_local
  SceShaccCgExt
  taihen_stub
  SceShaccCg_stub
  mathneon
  stdc++
  deflate
  z
  m
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAvPlayer_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceIme_stub
  SceKernelDmacMgr_stub
  SceMotion_stub
  SceLibKernel_stub
  ScePower_stub
  SceSysmodule_stub
  SceTouch_stub
  SceVshBridge_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${LIVEAREA_DIR}/icon0.png sce_sys/icon0.png
       ${LIVEAREA_DIR}/pic0.png sce_sys/pic0.png
       ${LIVEAREA_DIR}/bg0.png sce_sys/livearea/contents/bg0.png
       ${LIVEAREA_DIR}/startup.png sce_sys/livearea/contents/startup.png
       ${LIVEAREA_DIR}/template.xml sce_sys/livearea/contents/template.xml
)

if(PSVITAIP)
  add_custom_target(send
    COMMAND echo destroy | nc ${PSVITAIP} 1338
    COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
    COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
    DEPENDS eboot.bin)
endif()
