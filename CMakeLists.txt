cmake_minimum_required(VERSION 3.21)
project(7zfm-qt VERSION 0.2.0 LANGUAGES C CXX)

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

# Upstream's assembly LZMA decoder (USE_LZMA_DEC_ASM in 7zip_gcc.mak) decodes
# about twice as fast as LzmaDec.c. arm64's is GNU syntax; x86_64's is MASM
# syntax, so it needs uasm (or jwasm/asmc, as upstream allows).
option(LZMA_DEC_ASM "Use 7-Zip's assembly LZMA decoder when it can be built" ON)
set(LZMA_DEC_ASM_USED OFF)
if(LZMA_DEC_ASM)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    enable_language(ASM)
    target_sources(7zcore PRIVATE ${SEVENZIP_DIR}/Asm/arm64/LzmaDecOpt.S)
    set(LZMA_DEC_ASM_USED ON)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    find_program(MASM_ASSEMBLER NAMES uasm jwasm asmc)
    if(MASM_ASSEMBLER)
      set(LZMA_DEC_OPT_OBJ ${CMAKE_CURRENT_BINARY_DIR}/LzmaDecOpt.o)
      add_custom_command(OUTPUT ${LZMA_DEC_OPT_OBJ}
        COMMAND ${MASM_ASSEMBLER} -nologo -elf64 -DABI_LINUX -Fo${LZMA_DEC_OPT_OBJ}
                ${SEVENZIP_DIR}/Asm/x86/LzmaDecOpt.asm
        DEPENDS ${SEVENZIP_DIR}/Asm/x86/LzmaDecOpt.asm ${SEVENZIP_DIR}/Asm/x86/7zAsm.asm
        VERBATIM)
      # an OBJECT library doesn't pass prebuilt objects on to its users
      add_library(lzmadecopt STATIC ${LZMA_DEC_OPT_OBJ})
      set_target_properties(lzmadecopt PROPERTIES LINKER_LANGUAGE C)
      target_link_libraries(7zcore PUBLIC lzmadecopt)
      set(LZMA_DEC_ASM_USED ON)
    endif()
  endif()
endif()
if(LZMA_DEC_ASM_USED)
  set_source_files_properties(${SEVENZIP_DIR}/C/LzmaDec.c PROPERTIES
    COMPILE_DEFINITIONS Z7_LZMA_DEC_OPT)
  # the asm objects carry no .note.GNU-stack; upstream links the same way
  target_link_options(7zcore PUBLIC LINKER:-z,noexecstack)
  message(STATUS "LZMA decoder: assembly")
else()
  message(STATUS "LZMA decoder: C (on x86_64, install uasm for the faster one)")
endif()
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

# ---- w32ui: upstream Win32 dialogs on Qt (src/w32ui/Q32) ----
set(DIALOG_TABLE ${CMAKE_CURRENT_BINARY_DIR}/generated/DialogTemplates.inc)
add_custom_command(OUTPUT ${DIALOG_TABLE}
  COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen-dialogs.py
          ${SEVENZIP_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tools/rcinc ${DIALOG_TABLE}
  DEPENDS tools/gen-dialogs.py ${UPSTREAM_RC}
  COMMENT "Compiling 7-Zip dialog templates")

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
# upstream sources compiled unchanged against the Q32 layer
set(W32UI_UPSTREAM_SOURCES
  ${SEVENZIP_DIR}/CPP/Windows/Window.cpp
  ${SEVENZIP_DIR}/CPP/Windows/Control/Dialog.cpp
  ${SEVENZIP_DIR}/CPP/Windows/Control/ComboBox.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/SplitUtils.cpp
  src/gui/CompressDialog.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/PasswordDialog.cpp
  src/w32ui/Seams.cpp)
# upstream update GUI: uses CProgressThreadVirt -> the FmProgress stand-in
set(UPDATE_GUI_SOURCES
  ${SEVENZIP_DIR}/CPP/7zip/UI/GUI/UpdateGUI.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/GUI/UpdateCallbackGUI.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/GUI/UpdateCallbackGUI2.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/UpdateCallback100.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/ComboDialog.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/CopyDialog.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/SplitDialog.cpp
  src/gui/CompressCall2.cpp
  src/gui/FolderOps.cpp
  src/gui/PanelSplitFile.cpp
  src/gui/PanelCrc.cpp
  src/gui/BenchmarkDialog.cpp
  ${SEVENZIP_DIR}/CPP/Windows/Control/PropertyPage.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/SettingsPage.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/FoldersPage.cpp
  ${SEVENZIP_DIR}/CPP/7zip/UI/FileManager/EditPage.cpp
  src/gui/OptionsDialog.cpp)
set(Q32_INCLUDE "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/w32ui/Q32.h")
set_source_files_properties(${W32UI_UPSTREAM_SOURCES} PROPERTIES COMPILE_OPTIONS "${Q32_INCLUDE}")
set_source_files_properties(${UPDATE_GUI_SOURCES} PROPERTIES COMPILE_OPTIONS
  "${Q32_INCLUDE};-include;${CMAKE_CURRENT_SOURCE_DIR}/src/fm/FmProgress.h")
add_library(w32ui OBJECT src/w32ui/Q32.cpp ${W32UI_UPSTREAM_SOURCES} ${UPDATE_GUI_SOURCES} ${DIALOG_TABLE})
# copies in src/gui keep upstream's relative includes ("../FileManager/...")
set_source_files_properties(src/gui/CompressDialog.cpp src/gui/BenchmarkDialog.cpp PROPERTIES
  INCLUDE_DIRECTORIES ${SEVENZIP_DIR}/CPP/7zip/UI/GUI)
target_include_directories(w32ui PUBLIC src/w32ui src/core src/fm src/gui
  PRIVATE src/w32ui/include ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(w32ui PUBLIC fmlogic Qt6::Widgets)

# ---- GUI ----
if(BUILD_GUI)
  add_subdirectory(src)
endif()

# ---- tests ----
enable_testing()
add_subdirectory(tests)
