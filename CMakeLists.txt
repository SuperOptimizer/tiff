# Minimal, dependency-free TIFF reader/writer + custom 2D near-lossless codec.
# Consume via add_subdirectory():
#   add_subdirectory(tiff)
#   target_link_libraries(<your_target> tiff)
# which exposes tiff.h on the include path. Standalone (`-DTIFF_STANDALONE=ON`)
# also builds the tests.
cmake_minimum_required(VERSION 3.16)
project(tiff C)

option(TIFF_STANDALONE "build tiff tests" OFF)
if(CMAKE_PROJECT_NAME STREQUAL "tiff")
  set(TIFF_STANDALONE ON)
endif()

add_library(tiff STATIC tiff.c)
target_include_directories(tiff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tiff PUBLIC m)            # codec uses libm
set_target_properties(tiff PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(TIFF_STANDALONE)
  enable_testing()
  # container tests link the library
  add_executable(tiff_test tiff_test.c)
  target_link_libraries(tiff_test tiff)
  add_test(NAME tiff_roundtrip COMMAND tiff_test)

  add_executable(tiff_robust_test tiff_robust_test.c)
  target_link_libraries(tiff_robust_test tiff)
  add_test(NAME tiff_robust COMMAND tiff_robust_test)

  # codec tests #include tiff.c for static internals -> don't link the library
  foreach(ct test_dct test_entropy test_codec)
    add_executable(${ct} ${ct}.c)
    target_link_libraries(${ct} m)
    add_test(NAME ${ct} COMMAND ${ct})
  endforeach()
endif()
