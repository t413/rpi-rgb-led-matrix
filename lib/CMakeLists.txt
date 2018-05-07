project(rgbmatrix)

file(GLOB SOURCES "*.c" "*.cc")
file(GLOB HEADERS "*.h" "../include/*.h")


if(UNIX)
  list(APPEND LIBS pthread)
  if("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
    list(APPEND LIBS dl rt pthread)
  endif()
endif()

add_library(${PROJECT_NAME} ${SOURCES} ${HEADERS})
target_include_directories(${PROJECT_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(${PROJECT_NAME} ${LIBS})
