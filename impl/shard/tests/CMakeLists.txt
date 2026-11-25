add_executable(vlcore_tests test_core.cpp)
target_link_libraries(vlcore_tests PRIVATE vlcore)
add_test(NAME vlcore_tests COMMAND vlcore_tests)
