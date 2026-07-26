add_library(hook SHARED hook.cpp PointerSymbolTable.cpp)

add_smoke_test_target(hook
  ${CMAKE_SOURCE_DIR}/tests/units/hook/smokeHook.py
  FILE_DEPS
  ${CMAKE_CURRENT_SOURCE_DIR}/export.map
)

target_include_directories(hook PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hook PRIVATE drivers-common)

target_link_options(hook PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/export.map)

set_target_properties(hook PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/export.map)
