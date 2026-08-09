cmake_minimum_required(VERSION 3.22.1)
project("aaudiotester")

add_library(${CMAKE_PROJECT_NAME} SHARED
        aaudio_jni.cpp
        aaudio_player.cpp
        aaudio_recorder.cpp
        wav_file.cpp)

set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

target_link_libraries(${CMAKE_PROJECT_NAME} android aaudio log)
