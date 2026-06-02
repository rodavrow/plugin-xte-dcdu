cmake_minimum_required(VERSION 3.16)
project(xte_dcdu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(XTE_DCDU_VERSION "0.1.0")
add_compile_definitions(XTE_DCDU_VERSION=\"${XTE_DCDU_VERSION}\")

# -----------------------------------------------------------------------------
# X-Plane SDK
# -----------------------------------------------------------------------------
# User must drop the X-Plane SDK at plugin/XPLM/ such that
#   plugin/XPLM/CHeaders/XPLM/XPLMDefs.h
# resolves. The SDK can be obtained from https://developer.x-plane.com/sdk/.
set(XPLM_DIR "${CMAKE_SOURCE_DIR}/XPLM" CACHE PATH "Path to X-Plane SDK CHeaders root")
if(NOT EXISTS "${XPLM_DIR}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR
        "X-Plane SDK not found at ${XPLM_DIR}.\n"
        "Download it from https://developer.x-plane.com/sdk/ and unpack so that\n"
        "${XPLM_DIR}/CHeaders/XPLM/XPLMDefs.h exists, or pass -DXPLM_DIR=...")
endif()

include_directories(
    ${XPLM_DIR}/CHeaders/XPLM
    ${XPLM_DIR}/CHeaders/Widgets
)

# -----------------------------------------------------------------------------
# libjpeg-turbo (static) — built as an ExternalProject.
# libjpeg-turbo 3.x explicitly prohibits add_subdirectory / FetchContent.
# -----------------------------------------------------------------------------
include(ExternalProject)

if(WIN32)
    set(TURBO_LIB_FILENAME "turbojpeg-static.lib")
else()
    set(TURBO_LIB_FILENAME "libturbojpeg.a")
endif()

set(TURBO_INSTALL_DIR "${CMAKE_BINARY_DIR}/turbo_install")
set(TURBO_LIB         "${TURBO_INSTALL_DIR}/lib/${TURBO_LIB_FILENAME}")

# Pre-create include dir so the imported target's INTERFACE_INCLUDE_DIRECTORIES
# exists at configure time (ExternalProject only populates it at build time).
file(MAKE_DIRECTORY "${TURBO_INSTALL_DIR}/include")

ExternalProject_Add(libjpeg_turbo_ext
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
    GIT_TAG        3.0.4
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    CMAKE_ARGS
        "-DCMAKE_INSTALL_PREFIX=${TURBO_INSTALL_DIR}"
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_TURBOJPEG=ON
        -DCMAKE_BUILD_TYPE=Release
    BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target install
    BUILD_BYPRODUCTS "${TURBO_LIB}"
)

add_library(turbojpeg_imp STATIC IMPORTED GLOBAL)
set_target_properties(turbojpeg_imp PROPERTIES
    IMPORTED_LOCATION         "${TURBO_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${TURBO_INSTALL_DIR}/include"
)
add_dependencies(turbojpeg_imp libjpeg_turbo_ext)

# -----------------------------------------------------------------------------
# xxhash (single-header, vendored at third_party/xxhash/xxhash.h)
# -----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/xxhash/xxhash.h")
    message(FATAL_ERROR
        "xxhash.h not found.\n"
        "Download from https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.2/xxhash.h\n"
        "and place at plugin/third_party/xxhash/xxhash.h")
endif()
include_directories(${CMAKE_SOURCE_DIR}/third_party/xxhash)

# -----------------------------------------------------------------------------
# Plugin module
# -----------------------------------------------------------------------------
add_library(xte_dcdu MODULE
    src/plugin_main.cpp
    src/config.cpp
    src/texture_finder.cpp
    src/readback.cpp
    src/jpeg_encoder.cpp
    src/tcp_client.cpp
    src/change_detect.cpp
    src/gl_loader.cpp
    src/button_handler.cpp
)

target_include_directories(xte_dcdu PRIVATE src)

target_link_libraries(xte_dcdu PRIVATE turbojpeg_imp)
add_dependencies(xte_dcdu libjpeg_turbo_ext)

# -----------------------------------------------------------------------------
# Per-platform options
# -----------------------------------------------------------------------------
if(WIN32)
    target_compile_definitions(xte_dcdu PRIVATE
        IBM=1 APL=0 LIN=0
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1
        _CRT_SECURE_NO_WARNINGS=1
        NOMINMAX=1)
    target_link_libraries(xte_dcdu PRIVATE
        ${XPLM_DIR}/Libraries/Win/XPLM_64.lib
        ${XPLM_DIR}/Libraries/Win/XPWidgets_64.lib
        opengl32 ws2_32)
    # libjpeg-turbo's static build uses /MT; silence the resulting LNK4098
    # "defaultlib LIBCMT conflicts" warning. Both libraries still work.
    target_link_options(xte_dcdu PRIVATE /NODEFAULTLIB:LIBCMT)
    set_target_properties(xte_dcdu PROPERTIES
        OUTPUT_NAME "win"
        SUFFIX ".xpl"
        PREFIX "")
elseif(APPLE)
    target_compile_definitions(xte_dcdu PRIVATE
        APL=1 IBM=0 LIN=0
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1
        GL_SILENCE_DEPRECATION=1)
    target_link_libraries(xte_dcdu PRIVATE
        "-F${XPLM_DIR}/Libraries/Mac"
        "-framework XPLM"
        "-framework XPWidgets"
        "-framework OpenGL")
    set_target_properties(xte_dcdu PROPERTIES
        OUTPUT_NAME "mac"
        SUFFIX ".xpl"
        PREFIX ""
        BUNDLE TRUE)
else() # Linux
    target_compile_definitions(xte_dcdu PRIVATE
        LIN=1 IBM=0 APL=0
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1)
    target_link_options(xte_dcdu PRIVATE
        -shared -rdynamic -fPIC -fvisibility=hidden)
    target_link_libraries(xte_dcdu PRIVATE GL pthread dl
        ${XPLM_DIR}/Libraries/Lin/XPWidgets_64.so)
    set_target_properties(xte_dcdu PROPERTIES
        OUTPUT_NAME "lin"
        SUFFIX ".xpl"
        PREFIX "")
endif()

# Install layout: Plugin-XTE-DCDU/64/<plat>.xpl + xte-dcdu.cfg
install(TARGETS xte_dcdu
        LIBRARY DESTINATION Plugin-XTE-DCDU/64
        RUNTIME DESTINATION Plugin-XTE-DCDU/64)
install(FILES ${CMAKE_SOURCE_DIR}/xte-dcdu.cfg.example
        DESTINATION Plugin-XTE-DCDU
        RENAME xte-dcdu.cfg)
