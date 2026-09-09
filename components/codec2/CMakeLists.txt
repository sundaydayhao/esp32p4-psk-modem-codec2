include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
idf_component_register(SRCS ${C2_SOURCES} INCLUDE_DIRS "vendor/src" "include" PRIV_REQUIRES heap)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CODEC2_MODE_EN_DEFAULT=0 CODEC2_MODE_700C_EN=1)
# Preserve upstream files; narrow warnings for third-party code only.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function)
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include" "${CMAKE_CURRENT_LIST_DIR}/include/codec2_esp_alloc.h")
