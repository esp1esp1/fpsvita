cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -Wall -O2 -std=c++11 -fno-rtti -fno-exceptions -fpermissive -fshort-wchar -fno-section-anchors")
# -fshort-wchar: PAF wide strings and sce_paf_swprintf use 16-bit characters;
# GCC's default 32-bit wchar_t handed the label a UTF-32 buffer.

link_directories(${CMAKE_BINARY_DIR}/kernel)

add_executable(fpsvita-ui
  main.cpp
  impose.cpp
  hud.cpp
  scepafre_shim.cpp
)

target_include_directories(fpsvita-ui BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/paf_compat
  ${CMAKE_SOURCE_DIR}/common
)

#
# The C++ class API headers (Plugin, Widget, etc.) come from ScePaf-RE
# and must be installed separately if not already present:
#   https://github.com/VitaArchive/ScePaf-RE (main branch)
#   cp -r ScePaf-RE/include/* $VITASDK/arm-vita-eabi/include/
#

#
# ScePaf stubs are generated at build time from paf_stubs/ScePafRE.yml with
# vita-libs-gen-2. The libScePaf*_stub.a archives in ScePaf-RE's lib/ are in
# Sony's .sceFStub format which vita-elf-create does not understand: linking them
# produces no import entries and every PAF call returns -1 at runtime.
#
set(PAF_STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/paf_stubs)
# Weak imports: an unresolved PAF NID leaves its stub returning -1 instead of
# making the loader reject the whole module. module_start checks every stub it
# needs and bails (with the offender in boot.log) if any is unresolved.
set(PAF_STUB_LIB ${PAF_STUB_DIR}/libScePaf_stub_weak.a)
add_custom_command(
  OUTPUT ${PAF_STUB_LIB}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PAF_STUB_DIR}
  COMMAND vita-libs-gen-2 -yml=${CMAKE_CURRENT_SOURCE_DIR}/paf_stubs/ScePafRE.yml -output=${PAF_STUB_DIR}
  COMMAND $(MAKE) -C ${PAF_STUB_DIR} libScePaf_stub_weak.a
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/paf_stubs/ScePafRE.yml
  COMMENT "Generating ScePaf import stubs"
  VERBATIM
)
add_custom_target(paf-stubs DEPENDS ${PAF_STUB_LIB})
add_dependencies(fpsvita-ui paf-stubs)

target_link_libraries(fpsvita-ui
  ${PAF_STUB_LIB}
  ScePaf_stub_weak
  SceLibKernel_stub
  SceIofilemgr_stub
  SceAppMgr_stub
  SceDisplay_stub
  SceLibDbg_stub
  SceVshBridge_stub
  taihen_stub
  kubridge_stub_weak
  SceProcessmgr_stub
  SceSysmem_stub
  SceKernelModulemgr_stub
  FpsvitaForUser_stub
  k
  gcc
)

set_target_properties(fpsvita-ui PROPERTIES
  LINK_FLAGS "-nostdlib"
)

vita_create_self(fpsvita-ui.suprx fpsvita-ui
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
)
