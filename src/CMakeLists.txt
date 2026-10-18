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
include(GNUInstallDirs)
install(TARGETS 7zfm-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/7zfm-qt.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
foreach(size 16 32 48 256)
  install(FILES ${PROJECT_SOURCE_DIR}/data/icons/${size}.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME 7zfm-qt.png)
endforeach()
install(FILES ${PROJECT_SOURCE_DIR}/data/icons/7zfm-qt.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
