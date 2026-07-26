function(add_smoke_test_target TARGET SCRIPT)
  cmake_parse_arguments(PB "" "ARGS" "FILE_DEPS" ${ARGN})
  set(stamp_file "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_smoke.stamp")
  add_custom_command(
    OUTPUT ${stamp_file}
    COMMAND python3 ${SCRIPT}
            $<TARGET_FILE:${TARGET}>
            ${PB_FILE_DEPS}
            ${PB_ARGS}
    COMMAND ${CMAKE_COMMAND} -E echo "Smoke test for ${TARGET} - pass"
    COMMAND ${CMAKE_COMMAND} -E touch "${stamp_file}"
    DEPENDS ${TARGET} ${SCRIPT} ${PB_FILE_DEPS}
  )
  add_custom_target(smoke_${TARGET}
    DEPENDS ${stamp_file}
  )
endfunction()
