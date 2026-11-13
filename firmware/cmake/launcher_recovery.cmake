# The immutable compatibility entry is a small independent image, built as a
# subproject (like IDF's bootloader), never a copy of the production source tree.
include(ExternalProject)
set(_recovery_source "${CMAKE_CURRENT_LIST_DIR}/../recovery/sdgoods-esp32s3")
set(_recovery_build "${CMAKE_BINARY_DIR}/recovery")
ExternalProject_Add(launcher_recovery
    SOURCE_DIR "${_recovery_source}" BINARY_DIR "${_recovery_build}"
    CMAKE_ARGS -G Ninja -DIDF_TARGET=esp32s3
        "-DSDKCONFIG=${_recovery_build}/sdkconfig"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build . --target app --parallel 1
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1
    BUILD_BYPRODUCTS "${_recovery_build}/SDGOODS_LAUNCHER.bin")
string(JSON _recovery_label GET "${_flash_plan}" recovery_partition)
string(JSON _recovery_offset GET "${_flash_plan}" partitions "${_recovery_label}" offset)
math(EXPR _recovery_offset "${_recovery_offset}" OUTPUT_FORMAT HEXADECIMAL)
esptool_py_flash_target_image(flash recovery "${_recovery_offset}" "${_recovery_build}/SDGOODS_LAUNCHER.bin")
add_dependencies(flash launcher_recovery)
# Reset only upgrade metadata in an initial full package; ordinary app-flash
# never writes the immutable entry, journal, or either Launcher.
file(WRITE "${CMAKE_BINARY_DIR}/launcher-state-reset.bin" "")
execute_process(COMMAND "${PYTHON}" -c
    "from pathlib import Path; Path(r'${CMAKE_BINARY_DIR}/launcher-state-reset.bin').write_bytes(bytes([255])*8192)")
esptool_py_flash_to_partition(flash launcher_state "${CMAKE_BINARY_DIR}/launcher-state-reset.bin")
