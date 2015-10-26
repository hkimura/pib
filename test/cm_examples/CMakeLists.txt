set(cm_dependencies ${IB_VERBS_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${RDMA_CM_LIBRARIES} rt m)

message(STATUS "RDMA CA examples linking to: ${cm_dependencies}")

add_executable(ucmatose cmatose.c common.c)
target_link_libraries(ucmatose ${cm_dependencies})

add_executable(rping rping.c)
target_link_libraries(rping ${cm_dependencies})

add_executable(udaddy udaddy.c common.c)
target_link_libraries(udaddy ${cm_dependencies})

add_executable(mckey mckey.c)
target_link_libraries(mckey ${cm_dependencies})

add_executable(rdma_client rdma_client.c)
target_link_libraries(rdma_client ${cm_dependencies})

add_executable(rdma_server rdma_server.c)
target_link_libraries(rdma_server ${cm_dependencies})

add_executable(rdma_xclient rdma_xclient.c)
target_link_libraries(rdma_xclient ${cm_dependencies})

add_executable(rdma_xserver rdma_xserver.c)
target_link_libraries(rdma_xserver ${cm_dependencies})

add_executable(rstream rstream.c common.c)
target_link_libraries(rstream ${cm_dependencies})

add_executable(riostream riostream.c common.c)
target_link_libraries(riostream ${cm_dependencies})

add_executable(rcopy rcopy.c)
target_link_libraries(rcopy ${cm_dependencies})

add_executable(udpong udpong.c common.c)
target_link_libraries(udpong ${cm_dependencies})

add_executable(cmtime cmtime.c common.c)
target_link_libraries(cmtime ${cm_dependencies})
