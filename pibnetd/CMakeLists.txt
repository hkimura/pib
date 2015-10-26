set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
add_executable(pibnetd main.c smp.c perf.c logger.c)
add_executable(pibping pibping.c)

CONFIGURE_FILE(
  scripts/redhat-pibnetd.init
  scripts/redhat-pibnetd.init COPYONLY)

