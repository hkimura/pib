cmake_minimum_required(VERSION 2.8.5)
project(pib)

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

add_subdirectory(driver)
add_subdirectory(libpib)
add_subdirectory(pibnetd)
add_subdirectory(test)
