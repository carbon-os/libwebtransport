vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 9ec929e2d6da58bc42bc739a73de601acbdd4dd0c04563b5c322b896e06bc9fb1168043d9133b09e9a11253bb5c96a3e7f0b60b3e8c9fc4875945b49882c1a0d
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)