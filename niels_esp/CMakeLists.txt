# Niels ESP - ESP-IDF-project (ESP32-S3)
#
#   idf.py set-target esp32s3
#   idf.py menuconfig        -> "Niels ESP": wifi en pinnen
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(niels_esp)
