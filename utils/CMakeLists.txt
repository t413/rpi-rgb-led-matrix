project(utils)


find_package(ImageMagick COMPONENTS Magick++)
if(ImageMagick_FOUND)
  add_executable(led-image-viewer led-image-viewer.cc)
  target_link_libraries(led-image-viewer rgbmatrix ${ImageMagick_LIBRARIES})
  target_include_directories(${PROJECT_NAME} PUBLIC "${ImageMagick_INCLUDE_DIRS}")
else()
  message(STATUS "ImageMagick not found, not building image-viewer")
endif()

add_executable(video-viewer video-viewer.cc)
target_link_libraries(video-viewer rgbmatrix libavcodec libavformat libswscale libavutil)
