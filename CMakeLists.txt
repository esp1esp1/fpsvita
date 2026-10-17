cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(FPSVita-UI C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

#
# Build order matters: kernel module must be built first to generate
# FpsvitaForUser stub library used by companion and shell.
#

# 1. Kernel module (generates stubs)
add_subdirectory(kernel)

# 2. Companion / helper (links kernel stubs)
add_subdirectory(companion)

# 3. Shell UI (links kernel stubs + ScePaf)
add_subdirectory(shell)

#
# Make companion and shell depend on kernel stubs being generated.
# vita_create_stubs creates a target named <prefix>-stubs.
#
add_dependencies(fpsvita-helper fpsvita-kernel-stubs)
add_dependencies(fpsvita-ui fpsvita-kernel-stubs)
