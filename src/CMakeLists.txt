configure_file("${CMAKE_CURRENT_SOURCE_DIR}/gitsha1.cc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/gitsha1.cc" @ONLY)

set(PHOBOS_COMMON_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/blink.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/printf.c
    ${CMAKE_CURRENT_BINARY_DIR}/gitsha1.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/usbconfig.c)

set(PHOBOS_COMMON_SRC ${PHOBOS_COMMON_SRC} PARENT_SCOPE)
