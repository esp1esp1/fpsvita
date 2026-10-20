cmake_minimum_required(VERSION 3.10)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99 -fno-section-anchors")

link_directories(${CMAKE_BINARY_DIR}/kernel)

# Full helper (talks to fpsvita-kernel.skprx) and the lite helper (no kernel
# module, no quick menu: config.txt and state.txt only), from the same source.
foreach(variant fpsvita-helper fpsvita-helper-lite)
  add_executable(${variant} main.c)
  target_include_directories(${variant} PRIVATE ${CMAKE_SOURCE_DIR}/common)
  target_link_libraries(${variant}
    taihen_stub
    SceLibKernel_stub_weak
    SceDisplay_stub
    SceCtrl_stub
    SceAppMgr_stub_weak
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceKernelModulemgr_stub
    ScePower_stub
  )
  set_target_properties(${variant} PROPERTIES LINK_FLAGS "-nostdlib")
  vita_create_self(${variant}.suprx ${variant}
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
  )
endforeach()

target_link_libraries(fpsvita-helper FpsvitaForUser_stub k gcc)
target_compile_definitions(fpsvita-helper-lite PRIVATE FPSVITA_LITE=1)
target_link_libraries(fpsvita-helper-lite k gcc)
