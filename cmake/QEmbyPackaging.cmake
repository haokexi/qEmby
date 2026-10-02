include_guard(GLOBAL)

if(NOT UNIX OR APPLE)
    return()
endif()

set(_qemby_private_libdir "${CMAKE_INSTALL_LIBDIR}")
set(_qemby_private_full_libdir "${CMAKE_INSTALL_FULL_LIBDIR}")
set(_qemby_app_full_dir "${CMAKE_INSTALL_FULL_BINDIR}")
if(QEMBY_RUNTIME_LIB_SUBDIR)
    string(APPEND _qemby_private_libdir "/${QEMBY_RUNTIME_LIB_SUBDIR}")
    string(APPEND _qemby_private_full_libdir "/${QEMBY_RUNTIME_LIB_SUBDIR}")
    set(_qemby_app_full_dir "${_qemby_private_full_libdir}")
endif()

# Keep private libraries beside the installed application, without build paths.
file(RELATIVE_PATH _qemby_app_to_lib
    "${_qemby_app_full_dir}" "${_qemby_private_full_libdir}")
set_target_properties(qEmbyApp PROPERTIES
    INSTALL_RPATH "$ORIGIN/${_qemby_app_to_lib}")
set_target_properties(qEmbyCore PROPERTIES INSTALL_RPATH "$ORIGIN")

# QWindowKit's development install is disabled; its shared runtime is required.
foreach(_qemby_qwk_target IN ITEMS QWKCore QWKWidgets)
    if(TARGET ${_qemby_qwk_target})
        get_target_property(_qemby_qwk_type ${_qemby_qwk_target} TYPE)
        if(_qemby_qwk_type STREQUAL "SHARED_LIBRARY")
            set_target_properties(${_qemby_qwk_target} PROPERTIES
                INSTALL_RPATH "$ORIGIN")
            install(TARGETS ${_qemby_qwk_target}
                LIBRARY DESTINATION "${_qemby_private_libdir}")
        endif()
    endif()
endforeach()

file(RELATIVE_PATH QEMBY_LAUNCHER_RELATIVE_DIR
    "${CMAKE_INSTALL_FULL_BINDIR}" "${_qemby_app_full_dir}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/qemby.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qemby" @ONLY)
install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/qemby"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")

configure_file("${CMAKE_CURRENT_LIST_DIR}/qEmby.desktop.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qEmby.desktop" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qEmby.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${PROJECT_SOURCE_DIR}/src/qEmbyApp/resources/svg/qemby_logo.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME qemby.svg)
