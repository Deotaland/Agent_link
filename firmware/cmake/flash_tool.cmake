# Called from main after component registration, before IDF creates flash targets.
# The adapter transparently forwards offline esptool commands and performs device
# checks only when an explicitly requested Flash command writes a managed app.
if(NOT PROFILE_factory STREQUAL "")
    if(COMMAND __esptool_py_setup)
        __esptool_py_setup()
    endif()
    idf_build_get_property(_flash_python PYTHON)
    idf_build_get_property(_flash_chip IDF_TARGET)
    idf_component_set_property(esptool_py ESPTOOLPY_CMD
        "${_flash_python};${FIRMWARE_ROOT}/tools/flash_firmware.py;--plan;${CMAKE_BINARY_DIR}/flash-plan.json;--;--chip;${_flash_chip}")
endif()
