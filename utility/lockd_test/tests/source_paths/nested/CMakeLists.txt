add_library(paths STATIC extra.cpp)
target_include_directories(paths PRIVATE "${LOCKD_INCLUDE_DIR}" "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(paths PRIVATE LOCKD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}/")
