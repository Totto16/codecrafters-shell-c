vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Totto16/tvec.h.git
    REF 9135af3b242ffd9fd88b764a4bc251fc5fa29434
    PATCHES support_c2x_as_c23.patch
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_install_meson()

vcpkg_copy_pdbs()

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/tvecConfig.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/tvec/tvecConfig.cmake"
    INSTALL_DESTINATION share/tvec
)

write_basic_package_version_file(
    "${CURRENT_PACKAGES_DIR}/share/tvec/tvecConfigVersion.cmake"
    VERSION "${VERSION}"
    COMPATIBILITY SameMajorVersion
)

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/tvecTargets.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/tvec"
)
