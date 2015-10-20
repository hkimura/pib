set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)


# These two cmake scripts are from https://github.com/biddisco/rdmahelper/
find_package(IB_VERBS REQUIRED)
include_directories(${IB_VERBS_INCLUDE_DIRS})
# find_package(RDMA_CM REQUIRED)
# include_directories(${RDMA_CM_INCLUDE_DIRS})

set(test_dependencies ${IB_VERBS_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} rt m)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -D_GNU_SOURCE")

add_executable(test-ipoib-01 test-ipoib-01.c)
target_link_libraries(test-ipoib-01 ${test_dependencies})

add_executable(test-ib_reg_mr-01 test-ib_reg_mr-01.c)
target_link_libraries(test-ib_reg_mr-01 ${test_dependencies})

add_executable(comp_vector comp_vector.c)
target_link_libraries(comp_vector ${test_dependencies})

add_executable(show_mem_reg show_mem_reg.c)
target_link_libraries(show_mem_reg ${test_dependencies})

add_executable(qp-roundrobin qp-roundrobin.c)
target_link_libraries(qp-roundrobin ${test_dependencies})

add_executable(query_pkey query_pkey.c)
target_link_libraries(query_pkey ${test_dependencies})


set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fno-strict-aliasing")
add_executable(hk_playground hk_playground.cpp)
target_link_libraries(hk_playground ${test_dependencies})
