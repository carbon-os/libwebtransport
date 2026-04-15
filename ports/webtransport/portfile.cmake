vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 a20217c376988b7d42f3c06a8e2a29dfde13557093f68bc5f0af9c0426cfe337f747cec70fca7fda14b572dd3433764bce04d591c286b37f2dbfaefeb0ee6886
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)