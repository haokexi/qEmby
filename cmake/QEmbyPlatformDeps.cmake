# Reconstructed platform-deps module.
# NOTE: upstream qEmby (v0.0.5) failed to commit the cmake/ helper modules,
# so this file is a local reconstruction sufficient to build on Linux.
include_guard(GLOBAL)

# qEmbyCore is built as a SHARED library and links the QCoro/spdlog static
# libraries pulled in via FetchContent. Those static libs must therefore be
# built position-independent, or linking libqEmbyCore.so fails with
# "relocation R_X86_64_PC32 ... recompile with -fPIC". Enable PIC globally.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Append optional Qt install prefixes. On Linux the system Qt6 is discovered
# automatically by find_package, so this is a no-op unless QEMBY_QT_ROOT is set.
function(qemby_append_default_qt_prefixes)
    if(QEMBY_QT_ROOT)
        list(APPEND CMAKE_PREFIX_PATH "${QEMBY_QT_ROOT}")
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
    endif()
endfunction()

# Link libmpv into the given target.
# Linux/macOS: use the system libmpv discovered via pkg-config.
function(qemby_link_libmpv target)
    if(WIN32)
        message(FATAL_ERROR
            "qemby_link_libmpv: Windows bundled libmpv is not handled by this "
            "reconstructed module (Linux build only).")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    target_link_libraries(${target} PRIVATE PkgConfig::MPV)
endfunction()
