cmake_minimum_required(VERSION 3.21)
project(7zfm-qt VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(BUILD_GUI "Build the Qt GUI" ON)
if(SANITIZE)
  # 7-Zip reads unaligned little-endian fields on purpose (MY_CPU_LE_UNALIGN).
  add_compile_options(-fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(SEVENZIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/7zip)
if(NOT EXISTS ${SEVENZIP_DIR}/CPP/7zip/UI/Agent/Agent.cpp)
  message(FATAL_ERROR "7-Zip submodule missing: run 'git submodule update --init'")
endif()

# ---- 7zcore: upstream formats, codecs, UI/Common and Agent, unmodified ----
include(cmake/7zcore-sources.cmake)
# UI/Agent is ported in src/agent (upstream never builds it on POSIX). Its
# relative includes ("../Common/...", "../../../Windows/...") resolve
# against the upstream Agent directory, which is why that is on the path.
file(GLOB AGENT_SOURCES CONFIGURE_DEPENDS src/agent/*.cpp)

# Upstream settings code, unchanged: it talks to Windows/Registry.h, which
# src/core/RegistryQt.cpp implements on QSettings.
set(SEVENZIP_SETTINGS_SOURCES
  ${SEVENZIP_DIR}/CPP/7zip/UI/Common/ZipRegistry.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/RegistryUtils.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/ViewSettings.cpp
  src/core/RegistryQt.cpp)
set(WIN32_COMPAT_INCLUDE "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/core/compat/Win32Compat.h")
set_source_files_properties(${SEVENZIP_SETTINGS_SOURCES} PROPERTIES
  COMPILE_OPTIONS "${WIN32_COMPAT_INCLUDE}")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core)

# OBJECT, not STATIC: formats and codecs register themselves from static
# constructors (REGISTER_ARC / REGISTER_CODEC) that nothing references, so a
# static archive would let the linker drop them all.
add_library(7zcore OBJECT
  ${SEVENZIP_CORE_SOURCES}
  ${AGENT_SOURCES}
  ${SEVENZIP_SETTINGS_SOURCES}
  src/core/Guids.cpp
  src/core/Seams.cpp
)
# Same defines as upstream 7zip_gcc.mak (formats linked statically, no
# Z7_EXTERNAL_CODECS, RAR enabled).
target_compile_definitions(7zcore PUBLIC
  _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE _REENTRANT
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
target_include_directories(7zcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/agent
  ${SEVENZIP_DIR}/CPP/7zip/UI/Agent
  ${SEVENZIP_DIR}/CPP
  ${SEVENZIP_DIR})
set_target_properties(7zcore PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(7zcore PUBLIC Qt6::Core Threads::Threads ${CMAKE_DL_LIBS})

# ---- fmlogic: FileManager logic (ported in src/fm, plus upstream files that
# compile unchanged), no widgets ----
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LANG_TABLE ${CMAKE_CURRENT_BINARY_DIR}/generated/LangTable.inc)
file(GLOB_RECURSE UPSTREAM_RC CONFIGURE_DEPENDS
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/*.rc ${SEVENZIP_DIR}/CPP/7zip/UI/GUI/*.rc)
add_custom_command(OUTPUT ${LANG_TABLE}
  COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-lang.py ${SEVENZIP_DIR} ${LANG_TABLE}
  DEPENDS tools/gen-lang.py ${UPSTREAM_RC}
  COMMENT "Extracting 7-Zip resource strings")

file(GLOB FM_SOURCES CONFIGURE_DEPENDS src/fm/*.cpp)
set(FM_UPSTREAM_SOURCES
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/FormatUtils.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/PropertyName.cpp)
add_library(fmlogic OBJECT ${FM_SOURCES} ${FM_UPSTREAM_SOURCES} src/core/Lang.cpp ${LANG_TABLE})
target_compile_options(fmlogic PRIVATE ${WIN32_COMPAT_INCLUDE})
target_include_directories(fmlogic PUBLIC src/fm src/core
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(fmlogic PUBLIC 7zcore)

# ---- tests ----
enable_testing()
add_subdirectory(tests)

# ---- GUI ----
if(BUILD_GUI)
  add_subdirectory(src)
endif()
