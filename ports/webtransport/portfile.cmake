#
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)