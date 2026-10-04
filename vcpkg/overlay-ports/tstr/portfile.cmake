vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Totto16/tstr.h.git
    REF c7b29108c427223a2b4b45dd16c1aba3d8865f43
    PATCHES support_c2x_as_c23.patch
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_install_meson()

vcpkg_copy_pdbs()

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/tstrConfig.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/tstr/tstrConfig.cmake"
    INSTALL_DESTINATION share/tstr
)

write_basic_package_version_file(
    "${CURRENT_PACKAGES_DIR}/share/tstr/tstrConfigVersion.cmake"
    VERSION "${VERSION}"
    COMPATIBILITY SameMajorVersion
)

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/tstrTargets.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/tstr"
)
