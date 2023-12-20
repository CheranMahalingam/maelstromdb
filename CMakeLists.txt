cmake_minimum_required(VERSION 3.20)

project(maelstromdb)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif(NOT CMAKE_BUILD_TYPE)

set(CXX_FLAGS_DEBUG "-ggdb")
set(CXX_FLAGS_RELEASE "-O3")

string (TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE)
if("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS_DEBUG}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS_RELEASE}")
else()
  message(FATAL_ERROR "Unknown build type: ${CMAKE_BUILD_TYPE}")
endif()

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(glog REQUIRED)

message(STATUS "Using googletest ${GTest_VERSION}")
message(STATUS "Using protobuf ${Protobuf_VERSION}")
message(STATUS "Using grpc ${gRPC_VERSION}")
message(STATUS "Using googlebenchmark ${benchmark_VERSION}")

set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

string (TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE)
message("Configured for ${CMAKE_BUILD_TYPE} build "
        "(set with cmake -DCMAKE_BUILD_TYPE={release,debug,...})")
if("${CMAKE_BUILD_TYPE}" STREQUAL "DEBUG")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
elseif ("${CMAKE_BUILD_TYPE}" STREQUAL "RELEASE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
else()
  message(FATAL_ERROR "Unknown build type: ${CMAKE_BUILD_TYPE}")
endif()

option(MAELSTROMDB_BUILD_TESTS "Set on/off to enable/disable maelstromdb unit tests" ON)
if(MAELSTROMDB_BUILD_TESTS)
  find_package(GTest REQUIRED)
  find_package(benchmark REQUIRED)

  enable_testing()
  add_subdirectory(tests)
endif()

add_subdirectory(src)

