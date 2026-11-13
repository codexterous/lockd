cmake_minimum_required(VERSION 3.24)
project(lockd_build_contracts LANGUAGES CXX)

# The build checks need the configured compiler, not daemon/Boost dependencies.
# This entry point keeps their CI gate independent of Python's unittest discovery.
get_filename_component(Lockd_BUILD_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
enable_testing()
include("${Lockd_BUILD_TEST_SOURCE}/cmake/lockd_build_tests.cmake")
