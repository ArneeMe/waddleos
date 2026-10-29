# The mesh code of the T5 (src/services/mesh, lib/WaddleMesh), built for the
# phone. What the T5 gets from Arduino and its board comes from the simulator's
# stand-ins (test/sim/stubs), with a few of the phone's own in platform/ first.
cmake_minimum_required(VERSION 3.22)
project(waddle C CXX)

set(CMAKE_CXX_STANDARD 20)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

include(FetchContent)
FetchContent_Declare(ArduinoJson GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson GIT_TAG v7.4.2)
FetchContent_GetProperties(ArduinoJson)
if(NOT arduinojson_POPULATED)
  FetchContent_Populate(ArduinoJson)
endif()

file(GLOB MESH_LIB ${REPO}/lib/WaddleMesh/src/*.cpp)
add_library(waddle SHARED
  waddle_jni.cpp
  platform_glue.cpp
  ${MESH_LIB}
  ${REPO}/lib/monocypher/src/monocypher.c
  ${REPO}/lib/monocypher/src/monocypher-ed25519.c
  ${REPO}/src/services/mesh/MeshNode.cpp
  ${REPO}/src/services/mesh/MeshReceive.cpp
  ${REPO}/src/services/mesh/MeshKeys.cpp
  ${REPO}/src/services/mesh/MeshServerSync.cpp
  ${REPO}/src/services/mesh/MeshPairing.cpp
  ${REPO}/src/services/mesh/MessageStore.cpp
  ${REPO}/src/services/mesh/MeshConfig.cpp
  ${REPO}/src/services/mesh/MeshGroups.cpp)
target_include_directories(waddle PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/platform
  ${REPO}/test/sim/stubs
  ${REPO}/src
  ${REPO}/lib/WaddleMesh/src
  ${REPO}/lib/monocypher/src
  ${arduinojson_SOURCE_DIR}/src)
target_compile_definitions(waddle PRIVATE MESH_PHONE)
target_link_libraries(waddle log)
