cmake_minimum_required(VERSION 3.10)

project(cachex VERSION 0.15)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS False)

# Add standard compile options for warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -O3)
endif()

# We only have one executable
add_executable(cachex
    src/main.cpp
    src/result.h
    src/scsi_status.h
)

# On Linux we need to handle sg_io, no special libraries usually needed, but include directories might be.
# On Windows, need to be careful with statically linking runtime if desired, but default is fine.

# If we want a static build as in the original Makefile:
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Uncomment to enable static linking
    # target_link_options(cachex PRIVATE -static -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_executable(unit_tests tests/test_result.cpp)
add_test(NAME result_test COMMAND unit_tests)
