vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Totto16/trtti.git
    REF e9361bcadf01d4503fddeb3a8ddbc6f8ffd21cc7
    PATCHES support_c2x_as_c23.patch
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_install_meson()

vcpkg_copy_pdbs()

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/trttiConfig.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/trtti/trttiConfig.cmake"
    INSTALL_DESTINATION share/trtti
)

write_basic_package_version_file(
    "${CURRENT_PACKAGES_DIR}/share/trtti/trttiConfigVersion.cmake"
    VERSION "${VERSION}"
    COMPATIBILITY SameMajorVersion
)

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/trttiTargets.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/trtti"
)
