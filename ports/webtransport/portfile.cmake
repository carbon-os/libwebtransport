#
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 d69d63e3c00542d37bb686e0dce9ef2143ca88b4bb65101a7cf47d482cb3d7c46a999ec9a70c89538103328d779ccadcd690e12708f32802fbb86d04463c2b13
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)