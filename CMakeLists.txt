cmake_minimum_required(VERSION 3.27)

project(scrolloverview
    DESCRIPTION "scrolloverview plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

file(GLOB SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(scrolloverview SHARED ${SRC})
target_compile_options(scrolloverview PRIVATE -fno-gnu-unique)
if(SCROLLOVERVIEW_TESTING)
    target_compile_definitions(scrolloverview PRIVATE SCROLLOVERVIEW_TESTING)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    if(NOT DOBBY_SOURCE OR NOT DOBBY_LIBRARY)
        message(FATAL_ERROR "ARM64 experiment requires DOBBY_SOURCE and DOBBY_LIBRARY")
    endif()
    target_include_directories(scrolloverview PRIVATE "${DOBBY_SOURCE}/include")
    target_link_libraries(scrolloverview PRIVATE "${DOBBY_LIBRARY}" dl pthread)
    target_link_options(scrolloverview PRIVATE "-Wl,--exclude-libs,ALL")
endif()

set(VERSION_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VERSION_HEADER "${VERSION_HEADER_DIR}/PluginVersion.hpp")
add_custom_target(scrolloverview-version ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${VERSION_HEADER_DIR}"
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-plugin-version.sh" "${VERSION_HEADER}" "${CMAKE_CURRENT_SOURCE_DIR}"
    BYPRODUCTS "${VERSION_HEADER}"
    VERBATIM
)
add_dependencies(scrolloverview scrolloverview-version)
target_include_directories(scrolloverview PRIVATE "${VERSION_HEADER_DIR}")

find_package(PkgConfig REQUIRED)
pkg_search_module(lua REQUIRED IMPORTED_TARGET "lua5.4>=5.4" "lua>=5.4")
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(scrolloverview PRIVATE rt PkgConfig::deps PkgConfig::lua)

install(TARGETS scrolloverview)
