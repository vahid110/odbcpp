# CMake generated Testfile for 
# Source directory: /Users/vahidsbr/odbcpp
# Build directory: /Users/vahidsbr/odbcpp/build-postgresql
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[test_datarow]=] "/Users/vahidsbr/odbcpp/build-postgresql/tests/test_datarow")
set_tests_properties([=[test_datarow]=] PROPERTIES  LABELS "unit" _BACKTRACE_TRIPLES "/Users/vahidsbr/odbcpp/cmake/Tests.cmake;21;add_test;/Users/vahidsbr/odbcpp/cmake/Tests.cmake;33;add_test_executable;/Users/vahidsbr/odbcpp/cmake/Tests.cmake;0;;/Users/vahidsbr/odbcpp/CMakeLists.txt;91;include;/Users/vahidsbr/odbcpp/CMakeLists.txt;0;")
add_test([=[it_simple_query]=] "/Users/vahidsbr/odbcpp/build-postgresql/tests/it_simple_query")
set_tests_properties([=[it_simple_query]=] PROPERTIES  LABELS "integration" _BACKTRACE_TRIPLES "/Users/vahidsbr/odbcpp/cmake/Tests.cmake;21;add_test;/Users/vahidsbr/odbcpp/cmake/Tests.cmake;41;add_test_executable;/Users/vahidsbr/odbcpp/cmake/Tests.cmake;0;;/Users/vahidsbr/odbcpp/CMakeLists.txt;91;include;/Users/vahidsbr/odbcpp/CMakeLists.txt;0;")
subdirs("_deps/googletest-build")
