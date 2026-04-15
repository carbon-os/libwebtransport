vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO carbon-os/libwebtransport
    REF v1.0.0 
    SHA512 a749a78d337c97ac7bf4f4b7bac821cc9aa6a77b4583088476fa27a7f2e8177b2e3fdd669815c623143616f150557b2188d1d23e5a620f70b753631732837b50
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME webtransport CONFIG_PATH share/webtransport)