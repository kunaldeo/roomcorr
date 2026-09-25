cmake_minimum_required(VERSION 3.20)
project(roomcorr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Built on and for this machine, so let the compiler use every SIMD extension
# the CPU has; the convolution inner loop auto-vectorises.
option(ROOMCORR_NATIVE "Build with -march=native" ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(ROOMCORR_NATIVE)
  add_compile_options(-march=native)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3 fftw3f)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
find_package(Threads REQUIRED)

# Everything except the PipeWire front ends, so the tests can link it.
add_library(roomcorr_core STATIC
  src/json.cpp
  src/config.cpp
  src/dsp/convolver.cpp
  src/engine.cpp
  src/analysis.cpp
  src/wav.cpp
  src/design.cpp
)
target_include_directories(roomcorr_core PUBLIC src)
target_link_libraries(roomcorr_core PUBLIC PkgConfig::FFTW PkgConfig::SNDFILE Threads::Threads)
target_compile_options(roomcorr_core PRIVATE -O3)

add_executable(roomcorr
  src/main.cpp
  src/daemon.cpp
  src/control.cpp
  src/measure.cpp
  src/calibrate.cpp
  src/devices.cpp
)
target_link_libraries(roomcorr PRIVATE roomcorr_core PkgConfig::PIPEWIRE)

enable_testing()
add_executable(roomcorr_tests tests/tests.cpp)
target_link_libraries(roomcorr_tests PRIVATE roomcorr_core)
add_test(NAME dsp COMMAND roomcorr_tests)

install(TARGETS roomcorr RUNTIME DESTINATION bin)
