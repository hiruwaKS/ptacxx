add_library(drivers-common STATIC
  QueryInterface.cpp
  Postprocess.cpp
)

target_include_directories(drivers-common PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(drivers-common PUBLIC common LLVMAnalysis)
