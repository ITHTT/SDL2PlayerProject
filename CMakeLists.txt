cmake_minimum_required(VERSION 3.15)
project(SDL2PlayerProject C)

set(CMAKE_C_STANDARD 99)

#添加链接库的头文件
include_directories(${PROJECT_SOURCE_DIR}/FFmpeg/include
        ${PROJECT_SOURCE_DIR}/SDL2/include)
link_directories(${PROJECT_SOURCE_DIR}/FFmpeg/lib
        ${PROJECT_SOURCE_DIR}/SDL2/lib)

add_executable(SDL2PlayerProject
        main.c
        player.h
        player.c)

#关联动态链接库
target_link_libraries(
        SDL2PlayerProject
        SDL2
        SDL2main
        avformat-58
        avcodec-58
        avdevice-58
        avfilter-7
        avutil-56
        postproc-55
        swresample-3
        swscale-5)