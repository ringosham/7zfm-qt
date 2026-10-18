add_executable(core_smoke core_smoke.cpp)
target_link_libraries(core_smoke PRIVATE 7zcore)

add_test(NAME core_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_smoke.sh $<TARGET_FILE:core_smoke>
          ${CMAKE_BINARY_DIR}/fixtures ${PROJECT_SOURCE_DIR})

if(BUILD_GUI)
  add_test(NAME gui_offscreen
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_gui.sh $<TARGET_FILE:7zfm-qt>
            ${CMAKE_BINARY_DIR}/fixtures ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/gui-test)
endif()
