set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fPIC")
set(map_file "${CMAKE_CURRENT_SOURCE_DIR}/src/pib.map")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--version-script=${map_file}") # any clean way to do this..
add_library(pib-rdmav2 SHARED src/pib.c)
