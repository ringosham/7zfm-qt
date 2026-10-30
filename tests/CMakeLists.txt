add_executable(core_smoke core_smoke.cpp)
target_link_libraries(core_smoke PRIVATE 7zcore)

# Upstream 7zz (Bundles/Alone2) from the same sources: the tests' reference
# for listing / testing / extracting archives the GUI produced.
set(SEVENZZ_SOURCES ${SEVENZIP_CORE_SOURCES})
# extras that 7zfm-qt needs but upstream's Alone2 does not link
list(FILTER SEVENZZ_SOURCES EXCLUDE REGEX "(UI/Common/WorkDir|UI/Common/ArchiveName|Windows/DLL|Windows/FileSystem)\\.cpp$")
add_executable(7zz ${SEVENZZ_SOURCES} ${SEVENZIP_CONSOLE_SOURCES})
target_compile_definitions(7zz PRIVATE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE _REENTRANT NDEBUG)
set_source_files_properties(${SEVENZIP_DIR}/CPP/7zip/UI/Console/Main.cpp PROPERTIES
  COMPILE_DEFINITIONS Z7_PROG_VARIANT_Z)
target_link_libraries(7zz PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# Both drivers use the generated images: build them once, before either
# runs, so that parallel ctest (-j) cannot race on them.
add_test(NAME fixtures
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.sh ${CMAKE_BINARY_DIR}/fixtures)
set_tests_properties(fixtures PROPERTIES FIXTURES_SETUP images)

add_test(NAME core_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_smoke.sh $<TARGET_FILE:core_smoke>
          ${CMAKE_BINARY_DIR}/fixtures ${PROJECT_SOURCE_DIR})

if(BUILD_GUI)
  add_test(NAME gui_offscreen
    COMMAND env SEVENZZ=$<TARGET_FILE:7zz> ${CMAKE_CURRENT_SOURCE_DIR}/run_gui.sh $<TARGET_FILE:7zfm-qt>
            ${CMAKE_BINARY_DIR}/fixtures ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/gui-test)
endif()

set_tests_properties(core_smoke PROPERTIES FIXTURES_REQUIRED images)
if(BUILD_GUI)
  set_tests_properties(gui_offscreen PROPERTIES FIXTURES_REQUIRED images)
endif()
