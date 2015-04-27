execute_process(COMMAND /bin/sh -c "${CMAKE_BINARY_DIR}/bin/maxscale -f ${CMAKE_BINARY_DIR}/etc/maxscale.cnf &>/dev/null 2> /dev/null > /dev/null")
execute_process(COMMAND make test RESULT_VARIABLE RVAL)
execute_process(COMMAND killall maxscale)
if(NOT RVAL EQUAL 0)
  message(FATAL_ERROR "Test suite failed with status: ${RVAL}")
else()
  message(STATUS "Test exited with status: ${RVAL}")
endif()
