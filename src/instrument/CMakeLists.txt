add_executable(instrument instrument.cpp)

# TODO: smoke tests

target_include_directories(instrument PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(instrument PRIVATE common hook)
