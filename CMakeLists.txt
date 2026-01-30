cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components)
set(PARTITION_CSV_PATH ${CMAKE_SOURCE_DIR}/partitions.csv)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-airplay)