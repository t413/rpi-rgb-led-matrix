project(rpi-rgb-led-matrix)
cmake_minimum_required(VERSION 2.8)

option(WALL_ON "Compile with all warnings" ON)
if(WALL_ON)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pedantic -Wextra")
endif()


find_library(FOUND_PCAP pcap)
message(STATUS "FOUND_PCAP -> ${FOUND_PCAP}")

add_subdirectory(lib)
add_subdirectory(utils)

