find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets DBus Test)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
file(GLOB UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp ui/*.h)
add_executable(7zfm-qt main.cpp ${UI_SOURCES} ${PROJECT_SOURCE_DIR}/data/7zfm-qt.qrc)
target_compile_options(7zfm-qt PRIVATE ${WIN32_COMPAT_INCLUDE})
target_compile_definitions(7zfm-qt PRIVATE Z7QT_VERSION="${PROJECT_VERSION}")
target_include_directories(7zfm-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ui)
# 7zcore listed directly: object files only reach targets that link them
# directly, and every format/codec object must be linked (static registration).
target_link_libraries(7zfm-qt PRIVATE w32ui fmlogic 7zcore Qt6::Widgets Qt6::DBus Qt6::Test)
# the pointer's real button state during an X11 drag (Panel::DropHasHappened)
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(XCB IMPORTED_TARGET xcb)
endif()
if(XCB_FOUND)
  target_link_libraries(7zfm-qt PRIVATE PkgConfig::XCB)
  target_compile_definitions(7zfm-qt PRIVATE Z7QT_HAVE_XCB)
endif()
include(GNUInstallDirs)
install(TARGETS 7zfm-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/7zfm-qt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(size 16 32 48 256)
  install(FILES ${PROJECT_SOURCE_DIR}/data/icons/${size}.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME 7zfm-qt.png)
endforeach()
install(FILES ${PROJECT_SOURCE_DIR}/data/icons/7zfm-qt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# context-menu entries for the file managers (tools/gen-menus.py), which run
# the command-line modes of ui/ShellCommand.cpp
option(INSTALL_FILE_MANAGER_MENUS "Install context-menu entries for Dolphin, Nemo, PCManFM-Qt, Caja and Thunar" ON)
if(INSTALL_FILE_MANAGER_MENUS)
  set(MENUS_DIR ${CMAKE_CURRENT_BINARY_DIR}/menus)
  set(MENUS_INPUTS ${PROJECT_SOURCE_DIR}/tools/gen-menus.py ${PROJECT_SOURCE_DIR}/data/7zfm-qt.desktop)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MENUS_INPUTS})
  file(REMOVE_RECURSE ${MENUS_DIR})
  execute_process(COMMAND ${Python3_EXECUTABLE} -I ${MENUS_INPUTS} ${MENUS_DIR}
    COMMAND_ERROR_IS_FATAL ANY)
  install(DIRECTORY ${MENUS_DIR}/kio/ DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus)
  install(DIRECTORY ${MENUS_DIR}/nemo/ DESTINATION ${CMAKE_INSTALL_DATADIR}/nemo/actions)
  install(DIRECTORY ${MENUS_DIR}/fma/ DESTINATION ${CMAKE_INSTALL_DATADIR}/file-manager/actions)
  # thunar-archive-plugin looks in its libexec directory (/usr/libexec on
  # Debian and Fedora; Arch's PKGBUILD passes /usr/lib/xfce4)
  set(THUNAR_ARCHIVE_PLUGIN_DIR ${CMAKE_INSTALL_FULL_LIBEXECDIR}/thunar-archive-plugin
    CACHE PATH "thunar-archive-plugin's directory for .tap scripts")
  install(PROGRAMS ${PROJECT_SOURCE_DIR}/data/menus/7zfm-qt.tap DESTINATION ${THUNAR_ARCHIVE_PLUGIN_DIR})
endif()
