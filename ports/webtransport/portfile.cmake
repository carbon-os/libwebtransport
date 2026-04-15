#
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 d6358585120c90f5ae1e80d718c7845367c41339a2527b8a4c994f3b0f1ff2e9280d19ec29f7d128e247b823824890c6835d230db71d3997f1ff9f1f4f0ee63f
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)