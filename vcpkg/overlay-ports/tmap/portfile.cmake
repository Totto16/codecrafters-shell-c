vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Totto16/tmap.h.git
    REF abd83bd18858341ea9048ec2f06d8670685d0c3b
    PATCHES support_c2x_as_c23.patch
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_install_meson()

vcpkg_copy_pdbs()

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/tmapConfig.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/tmap/tmapConfig.cmake"
    INSTALL_DESTINATION share/tmap
)

write_basic_package_version_file(
    "${CURRENT_PACKAGES_DIR}/share/tmap/tmapConfigVersion.cmake"
    VERSION "${VERSION}"
    COMPATIBILITY SameMajorVersion
)

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/tmapTargets.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/tmap"
)
