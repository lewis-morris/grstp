cmake_minimum_required(VERSION 3.10)
project(grstp)

set(CMAKE_CXX_STANDARD 20)

# Find GStreamer
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)

# Add include dirs and libs
include_directories(${GST_INCLUDE_DIRS})
link_directories(${GST_LIBRARY_DIRS})
add_definitions(${GST_CFLAGS_OTHER})

# Your executable
add_executable(grstp grstp.cpp)

# Link to GStreamer
target_link_libraries(grstp ${GST_LIBRARIES})