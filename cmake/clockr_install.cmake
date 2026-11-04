# clockr_install.cmake — install/export rules for the clockr package.
#
# Included from the root CMakeLists for top-level builds only: an embedding
# host owns its own install prefix and must not inherit these rules.
#
# What "installing" means, concretely:
# - install(TARGETS ... EXPORT ...) copies the built archives into
#   <prefix>/lib and records the targets in the clockrTargets export set;
# - install(DIRECTORY/FILES ...) copies headers (ours, the generated
#   version.hpp, and the bundled moodycamel headers) into <prefix>/include;
# - install(EXPORT ...) generates <prefix>/lib/cmake/clockr/
#   clockrTargets.cmake, which recreates imported clockr::* targets
#   pointing at those installed files;
# - the config + version files make find_package(clockr CONFIG) work.

# Installed target names match the in-tree aliases exactly (clockr::mem,
# clockr::conc, ...), so consumer code spells them the same in both modes.
set_target_properties(clockr PROPERTIES EXPORT_NAME clockr)
set_target_properties(clockr_mem PROPERTIES EXPORT_NAME mem)
set_target_properties(clockr_conc PROPERTIES EXPORT_NAME conc)
set_target_properties(clockr_kernel PROPERTIES EXPORT_NAME kernel)
set_target_properties(clockr_shell PROPERTIES EXPORT_NAME shell)

install(
    TARGETS clockr clockr_mem clockr_conc clockr_kernel clockr_shell
    EXPORT clockrTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    FILES "${PROJECT_BINARY_DIR}/generated/include/clockr/version.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/clockr"
)

# The installed package is self-contained: tlsf is compiled into
# clockr_mem, and the (header-only) moodycamel dependency travels as plain
# headers under clockr/third_party/, wired up through clockr_conc's
# INSTALL_INTERFACE include dir. No find_dependency needed.
install(
    FILES
        "${readerwriterqueue_SOURCE_DIR}/readerwriterqueue.h"
        "${readerwriterqueue_SOURCE_DIR}/atomicops.h"
        "${readerwriterqueue_SOURCE_DIR}/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/clockr/third_party/readerwriterqueue"
)

install(
    EXPORT clockrTargets
    FILE clockrTargets.cmake
    NAMESPACE clockr::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/clockr"
)

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/clockrConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/clockrConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/clockr"
)

# Pre-1.0: only the same minor version claims compatibility.
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/clockrConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
)

install(
    FILES
        "${PROJECT_BINARY_DIR}/clockrConfig.cmake"
        "${PROJECT_BINARY_DIR}/clockrConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/clockr"
)

install(
    FILES "${PROJECT_SOURCE_DIR}/LICENSE.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/clockr"
)

# Build-tree package: the same generated config works uninstalled, because it
# only includes the adjacent targets file. A host can consume a build
# directory directly with -Dclockr_DIR=<build-dir>.
export(
    EXPORT clockrTargets
    FILE "${PROJECT_BINARY_DIR}/clockrTargets.cmake"
    NAMESPACE clockr::
)
