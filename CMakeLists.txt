# Backrooms Iris - one build for every platform.
#
#   PC (Linux / Windows):  cmake -S . -B build && cmake --build build
#   PS Vita:               cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#
# Multiplayer is PC only and peer-hosted: one player's game is the host, friends connect to it
# directly. There is no dedicated server. The Vita build is single player and has no network code.
#
cmake_minimum_required(VERSION 3.16)
project(BackroomsIris VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(VITA)
  set(IRIS_PLATFORM "vita")
elseif(WIN32)
  set(IRIS_PLATFORM "windows")
else()
  set(IRIS_PLATFORM "unix")
endif()

option(IRIS_BUILD_CLIENT "Build the game" ON)

if(MSVC)
  add_compile_options(/W3 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- shared core
# Pure C++: world generation, collision, movement, the game simulation. Single player runs it
# locally; in multiplayer the host runs it for everyone.
add_library(iris_core STATIC
  src/core/world.cpp
  src/core/collision.cpp
  src/core/player.cpp
  src/core/sim.cpp
)
target_include_directories(iris_core PUBLIC src)

# networking (UDP, PC only): the in-game host + the joining client
if(NOT VITA)
  add_library(iris_net STATIC
    src/net/udp.cpp
    src/net/server.cpp
    src/net/client.cpp
  )
  target_link_libraries(iris_net PUBLIC iris_core)
  if(WIN32)
    target_link_libraries(iris_net PUBLIC ws2_32)
  endif()
endif()

option(IRIS_BUILD_TESTS "Build the core tests (ctest)" ON)
if(IRIS_BUILD_TESTS AND NOT VITA AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(iris_core_tests tests/core_tests.cpp src/client/tailscale.cpp)
  target_link_libraries(iris_core_tests PRIVATE iris_net)
  add_test(NAME core COMMAND iris_core_tests)
endif()

# --------------------------------------------------------------------- client
if(IRIS_BUILD_CLIENT)
  set(IRIS_CLIENT_SOURCES
    src/client/main.cpp
    src/client/app.cpp
    src/client/session.cpp
    src/client/effects.cpp
    src/client/settings.cpp
    src/client/font.cpp
    src/client/gl.cpp
    src/client/textures.cpp
    src/client/chunk_mesh.cpp
    src/client/renderer.cpp
    src/client/models.cpp
    src/client/audio.cpp
    src/platform/sdl_gamepad.cpp
  )

  if(VITA)
    # ---------------------------------------------------------- PS Vita (.vpk)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_APP_NAME "Backrooms Iris")
    set(VITA_TITLEID  "BKIR00001")
    set(VITA_VERSION  "01.00")
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12") # extended memory mode

    add_executable(iris_vita ${IRIS_CLIENT_SOURCES} src/platform/vita/platform_vita.cpp)
    target_compile_definitions(iris_vita PRIVATE IRIS_MULTIPLAYER=0)
    target_compile_options(iris_vita PRIVATE -O2 -ffast-math -mfpu=neon -mcpu=cortex-a9)
    target_link_libraries(iris_vita PRIVATE
      iris_core
      SDL2
      vitaGL vitashark SceShaccCgExt mathneon taihen_stub
      SceShaccCg_stub SceKernelDmacMgr_stub SceLibKernel_stub SceAppMgr_stub SceAppUtil_stub
      SceCommonDialog_stub SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCtrl_stub
      SceTouch_stub SceHid_stub SceMotion_stub SceAudio_stub SceAudioIn_stub ScePower_stub
      SceIme_stub SceIofilemgr_stub SceProcessmgr_stub SceSysmem_stub
      pthread m c
    )
    vita_create_self(eboot.bin iris_vita UNSAFE)
    vita_create_vpk(BackroomsIris.vpk ${VITA_TITLEID} eboot.bin
      VERSION ${VITA_VERSION}
      NAME ${VITA_APP_NAME}
      FILE vita/sce_sys/icon0.png sce_sys/icon0.png
           vita/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
           vita/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
           vita/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    )
  else()
    # ------------------------------------------------------ PC (Linux, Windows)
    find_package(SDL2 CONFIG QUIET)
    if(NOT TARGET SDL2::SDL2)
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
      add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
    endif()
    set(OpenGL_GL_PREFERENCE GLVND)
    find_package(OpenGL REQUIRED)

    add_executable(backrooms-iris ${IRIS_CLIENT_SOURCES} src/client/tailscale.cpp src/platform/pc/platform_pc.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(backrooms-iris PRIVATE iris_net OpenGL::GL Threads::Threads)
    if(WIN32)
      # Release builds are GUI apps (no console window); Debug keeps the console for logs
      set_target_properties(backrooms-iris PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
      if(TARGET SDL2::SDL2main)
        target_link_libraries(backrooms-iris PRIVATE SDL2::SDL2main)
      endif()
      if(MINGW)
        target_link_options(backrooms-iris PRIVATE -static-libgcc -static-libstdc++)
      endif()
    endif()
    target_link_libraries(backrooms-iris PRIVATE SDL2::SDL2)

    # Windows: put SDL2.dll next to the .exe so it runs straight from the build folder
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
      add_custom_command(TARGET backrooms-iris POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:backrooms-iris> $<TARGET_FILE_DIR:backrooms-iris>
        COMMAND_EXPAND_LISTS)
    endif()
    install(TARGETS backrooms-iris RUNTIME DESTINATION bin)
  endif()
endif()

message(STATUS "Backrooms Iris ${PROJECT_VERSION}: platform=${IRIS_PLATFORM}")
