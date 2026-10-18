add_executable(core_smoke core_smoke.cpp)
target_link_libraries(core_smoke PRIVATE 7zcore)

add_test(NAME core_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_smoke.sh $<TARGET_FILE:core_smoke>
          ${CMAKE_BINARY_DIR}/fixtures ${PROJECT_SOURCE_DIR})
