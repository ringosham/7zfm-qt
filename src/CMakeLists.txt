find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Test)

set(CMAKE_AUTOMOC ON)
file(GLOB UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp ui/*.h)
add_executable(7zfm-qt main.cpp ${UI_SOURCES})
target_compile_options(7zfm-qt PRIVATE ${WIN32_COMPAT_INCLUDE})
target_compile_definitions(7zfm-qt PRIVATE Z7QT_VERSION="${PROJECT_VERSION}")
target_include_directories(7zfm-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ui)
# 7zcore listed directly: object files only reach targets that link them
# directly, and every format/codec object must be linked (static registration).
target_link_libraries(7zfm-qt PRIVATE w32ui fmlogic 7zcore Qt6::Widgets Qt6::Test)
install(TARGETS 7zfm-qt RUNTIME DESTINATION bin)
