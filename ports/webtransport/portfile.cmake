vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 6a58f592d77e9981b2725c9b1250239b59e3d27f15295b7ec457e73c7f20f7bcb3982c66e209d9df11e567616cc3d6f3409acc1ca576c3296f68b328230e883b
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)