#===============================================================================
# Copyright 2021-2022 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#===============================================================================

include_directories(
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/src/graph
    ${PROJECT_SOURCE_DIR}/include)

file(GLOB COMMON_UNIT_TESTS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    )

list(APPEND COMMON_UNIT_TESTS ${TEST_THREAD})
list(APPEND COMMON_UNIT_TESTS ${TEST_ALLOCATOR})

add_subdirectory(interface)
add_subdirectory(backend)

get_property(UNIT_TEST_DEPS GLOBAL PROPERTY GRAPH_UNIT_TEST_DEPS)
get_property(LIB_DEPS GLOBAL PROPERTY DNNL_LIB_DEPS)

add_executable(test_graph_unit ${COMMON_UNIT_TESTS} ${UNIT_TEST_DEPS} ${LIB_DEPS})

# Set path to non exported headers
target_include_directories(test_graph_unit
    PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/gtests/graph
    ${PROJECT_SOURCE_DIR}/tests/gtests/gtest
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/graph
    )

if(DNNL_ENABLE_JIT_PROFILING OR DNNL_ENABLE_ITT_TASKS)
    if (UNIX AND NOT APPLE)
        # Not every compiler adds -ldl automatically
        list(APPEND EXTRA_SHARED_LIBS "${CMAKE_DL_LIBS}")
    endif()
endif()

target_link_libraries(test_graph_unit
    dnnl_gtest
    ${EXTRA_SHARED_LIBS}
    )

set(sycl_rt_pattern "(DPCPP)")

# Add CPU test if CPU runtime is enabled
if(NOT DNNL_CPU_RUNTIME STREQUAL "NONE")
    add_test("test_graph_unit" "test_graph_unit" "--engine=cpu")
    maybe_configure_windows_test("test_graph_unit" TEST)
endif()

# Add GPU test if GPU runtime is enabled
if(DNNL_GPU_RUNTIME MATCHES ${sycl_rt_pattern})
    add_test("test_graph_unit_gpu" "test_graph_unit" "--engine=gpu"
        "--gtest_filter=Execute*:Compile.*:CompiledPartition.*:Scratchpad.*:\
SyclAllocatorGpu*:SyclEngineGpu*")
    maybe_configure_windows_test("test_graph_unit_gpu" TEST)
endif()