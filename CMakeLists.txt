# CMakeLists.txt principal
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PARTITION_TABLE_SINGLE_APP_LARGE ON)
project(musicPlayer)
