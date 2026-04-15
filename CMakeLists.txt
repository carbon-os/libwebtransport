cmake_minimum_required(VERSION 3.20)
project(webtransport CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

find_library(QUICHE_LIB NAMES quiche REQUIRED)
find_path(QUICHE_INCLUDE_DIR NAMES "quiche/quic/core/quic_types.h" REQUIRED)

set(QUICHE_EXTRA_LIBS "")
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    file(GLOB _found "${_prefix}/lib/libquiche_*.a")
    list(APPEND QUICHE_EXTRA_LIBS ${_found})
endforeach()

add_library(quiche::quiche STATIC IMPORTED)
set_target_properties(quiche::quiche PROPERTIES
    IMPORTED_LOCATION             "${QUICHE_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${QUICHE_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES      "${QUICHE_EXTRA_LIBS}"
)

find_library(BORINGSSL_SSL_LIB NAMES ssl REQUIRED)
find_library(BORINGSSL_CRYPTO_LIB NAMES crypto REQUIRED)
find_library(ZLIB_LIB NAMES z zlib REQUIRED)
find_library(PROTOBUF_LIB NAMES protobuf REQUIRED)
find_library(UTF8_RANGE_LIB NAMES utf8_range utf8range REQUIRED)

add_library(boringssl::ssl    STATIC IMPORTED)
add_library(boringssl::crypto STATIC IMPORTED)
set_target_properties(boringssl::ssl    PROPERTIES IMPORTED_LOCATION "${BORINGSSL_SSL_LIB}")
set_target_properties(boringssl::crypto PROPERTIES IMPORTED_LOCATION "${BORINGSSL_CRYPTO_LIB}")

add_library(z::z STATIC IMPORTED)
set_target_properties(z::z PROPERTIES IMPORTED_LOCATION "${ZLIB_LIB}")

add_library(protobuf::protobuf STATIC IMPORTED)
set_target_properties(protobuf::protobuf PROPERTIES
    IMPORTED_LOCATION        "${PROTOBUF_LIB}"
    INTERFACE_LINK_LIBRARIES "${UTF8_RANGE_LIB}"
)

set(ABSL_LIBS "")
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    file(GLOB _found "${_prefix}/lib/libabsl_*.a")
    list(APPEND ABSL_LIBS ${_found})
endforeach()
if(NOT ABSL_LIBS)
    message(FATAL_ERROR "No abseil libraries found")
endif()
add_library(absl::absl INTERFACE IMPORTED)
set_target_properties(absl::absl PROPERTIES INTERFACE_LINK_LIBRARIES "${ABSL_LIBS}")

if(APPLE)
    find_library(CORE_FOUNDATION_FW CoreFoundation REQUIRED)
endif()

# ── Core Library ──────────────────────────────────────────────────────────────
add_library(webtransport_core STATIC
    src/wtengine.cc
    src/wtsession.cc
    src/wtserver.cc
    src/wtclient.cc
)

target_include_directories(webtransport_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(webtransport_core PRIVATE /W4 /WX)
else()
    target_compile_options(webtransport_core PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

target_link_libraries(webtransport_core
    PUBLIC
        Threads::Threads
        $<$<BOOL:${APPLE}>:${CORE_FOUNDATION_FW}>
    PRIVATE
        quiche::quiche
        boringssl::ssl
        boringssl::crypto
        z::z
        protobuf::protobuf
        absl::absl
)

# ── Public API ────────────────────────────────────────────────────────────────
add_library(webtransport STATIC src/webtransport.cc)

target_include_directories(webtransport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(webtransport PRIVATE /W4 /WX)
else()
    target_compile_options(webtransport PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

target_link_libraries(webtransport PUBLIC webtransport_core)
target_compile_definitions(webtransport PRIVATE $<$<CONFIG:Debug>:WT_DEBUG>)

# ── Installation Rules ────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS webtransport webtransport_core
    EXPORT webtransportTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT webtransportTargets
    FILE webtransportTargets.cmake
    NAMESPACE webtransport::
    DESTINATION ${CMAKE_INSTALL_DATADIR}/webtransport
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/webtransportConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/webtransportConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_DATADIR}/webtransport
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/webtransportConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/webtransport
)