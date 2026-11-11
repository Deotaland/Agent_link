include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/profile_build.cmake")
string(JSON _store ERROR_VARIABLE _error GET "${BUILD_PROFILE}" community_store)
if(_error)
    idf_component_register()
    return()
endif()
idf_component_register(SRCS "third_party/lodepng.cpp" "png_memory.cc"
    INCLUDE_DIRS "third_party" PRIV_REQUIRES heap)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LODEPNG_NO_COMPILE_ENCODER
    LODEPNG_NO_COMPILE_DISK LODEPNG_NO_COMPILE_CPP LODEPNG_NO_COMPILE_ALLOCATORS)
configure_file("third_party/LICENSE.txt" "${CMAKE_BINARY_DIR}/lodepng-LICENSE.txt" COPYONLY)
