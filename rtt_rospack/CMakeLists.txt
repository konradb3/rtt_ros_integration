cmake_minimum_required(VERSION 2.8.3)
project(rtt_rospack)

find_package(catkin REQUIRED COMPONENTS roslib)

find_package(OROCOS-RTT REQUIRED)

# Defines the orocos_* cmake macros. See that file for additional
# documentation.
include(${OROCOS-RTT_USE_FILE_PATH}/UseOROCOS-RTT.cmake)

#build ROS pack as a separate service
orocos_plugin(rtt_rospack src/rtt_rospack_service.cpp)
target_link_libraries(rtt_rospack ${roslib_LIBRARIES})

orocos_generate_package()

catkin_package(
  DEPENDS  rtt
  CATKIN_DEPENDS roslib 
)
