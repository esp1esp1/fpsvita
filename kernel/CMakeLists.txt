cmake_minimum_required(VERSION 3.10)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99 -ffreestanding -fno-section-anchors")

add_executable(fpsvita-kernel
  main.c
)

target_include_directories(fpsvita-kernel PRIVATE
  ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(fpsvita-kernel
  taihen_stub
  taihenForKernel_stub
  taihenModuleUtils_stub
  SceThreadmgrForDriver_stub
  SceSysmemForDriver_stub
  SceDebugForDriver_stub
  SceProcessmgrForDriver_stub
  SceProcEventForDriver_stub
  SceDisplayForDriver_stub
  SceKernelSuspendForDriver_stub
  k
  gcc
)

set_target_properties(fpsvita-kernel PROPERTIES
  LINK_FLAGS "-nostdlib"
)

set(VITA_ELF_CREATE_FLAGS "-n")
vita_create_self(fpsvita-kernel.skprx fpsvita-kernel
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
  UNSAFE
)
unset(VITA_ELF_CREATE_FLAGS)

# Manual vita_create_stubs — passes VITASDK to inner make so AS resolves
# (vita.cmake's vita_create_stubs uses bare `make -C` without VITASDK in env,
#  so arm-vita-eabi-as is not found when VITASDK is not exported in the shell)
set(_stubs_yml ${CMAKE_CURRENT_BINARY_DIR}/fpsvita-kernel-stubs.yml)
set(_stubs_dir ${CMAKE_CURRENT_BINARY_DIR}/fpsvita-kernel-stubs)

add_custom_command(OUTPUT ${_stubs_yml}
  COMMAND ${VITA_ELF_EXPORT} kernel
          ${CMAKE_CURRENT_BINARY_DIR}/fpsvita-kernel
          ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
          ${_stubs_yml}
  DEPENDS fpsvita-kernel
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
  COMMENT "Generating imports YAML for fpsvita-kernel"
)

add_custom_target(fpsvita-kernel-stubs ALL
  COMMAND ${VITA_LIBS_GEN} ${_stubs_yml} ${_stubs_dir}
  COMMAND ${CMAKE_COMMAND} -E env "VITASDK=${VITASDK}" make -C ${_stubs_dir}
  # vita-libs-gen names the archive after the module, not the library.
  COMMAND ${CMAKE_COMMAND} -E copy
          "${_stubs_dir}/libfpsvita-kernel_stub.a"
          "${CMAKE_CURRENT_BINARY_DIR}/libFpsvitaForUser_stub.a"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${_stubs_dir}/libfpsvita-kernel_stub.a"
          "${CMAKE_CURRENT_BINARY_DIR}/libfpsvita-kernel_stub.a"
  DEPENDS ${_stubs_yml}
  COMMENT "Building stubs fpsvita-kernel-stubs"
)
add_dependencies(fpsvita-kernel-stubs fpsvita-kernel)
