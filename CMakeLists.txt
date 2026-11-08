cmake_minimum_required(VERSION 3.24)

# Dependency policies must reach the toolchain loaded by project().
include(cmake/lockd_build_options.cmake NO_POLICY_SCOPE)
include(cmake/lockd_version.cmake)
project(lockd VERSION ${Lockd_VERSION} LANGUAGES C CXX)

include(cmake/lockd_build.cmake)
