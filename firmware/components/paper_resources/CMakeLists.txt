idf_component_register(SRCS "resource_pack.cc" "resource_partition.cc" "../../boards/common/raster_montserrat_14.c"
    INCLUDE_DIRS "include" "../../boards/common"
    REQUIRES esp_partition log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../../boards/common/fonts/OFL.txt"
               "${CMAKE_BINARY_DIR}/fonts-OFL.txt" COPYONLY)
foreach(_license pinyin jieba)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/assets/ime-data/${_license}-LICENSE.txt"
                   "${CMAKE_BINARY_DIR}/${_license}-LICENSE.txt" COPYONLY)
endforeach()
