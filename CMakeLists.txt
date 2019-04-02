cmake_minimum_required(VERSION 3.13)
project(testtask)

set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -Woverloaded-virtual -fpermissive" )
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpermissive" )

include_directories(/usr/include/ffmpeg)



add_executable(testtask main.cpp )
target_link_libraries(testtask avformat avfilter avutil avcodec avdevice swscale)


