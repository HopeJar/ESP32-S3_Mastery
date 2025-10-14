# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/blank/esp/esp-idf/components/bootloader/subproject"
  "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader"
  "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix"
  "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix/tmp"
  "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix/src/bootloader-stamp"
  "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix/src"
  "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/mnt/c/LocalRepo/ESP32-S3_Mastery/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
