cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-format)
project(esp32_attitude)

spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)
