# Builds a SpatialOS worker using the C++ API

# Replace project name below with your own worker name
# !!! This needs to match the artifact_name in spatialos.<name>.worker.json
# !!! because ${PROJECT_NAME} is used below when creating the zip target
project(AuctionHouseWorker)
cmake_minimum_required(VERSION 3.7)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_RPATH "$ORIGIN")

set(APPLICATION_ROOT "${PROJECT_SOURCE_DIR}/../..")
set(SCHEMA_SOURCE_DIR "${APPLICATION_ROOT}/schema")
set(WORKER_SDK_DIR "${APPLICATION_ROOT}/dependencies")
set(OUTER_SPATIAL_DIR "${APPLICATION_ROOT}/outerspatial")

# Strict warnings.
if(MSVC)
  add_definitions(/W2)
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
  add_definitions(-Wall -Wextra -pedantic)
endif()

add_subdirectory(${WORKER_SDK_DIR} "${CMAKE_CURRENT_BINARY_DIR}/WorkerSdk")
add_subdirectory(${SCHEMA_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/Schema")
add_subdirectory(${OUTER_SPATIAL_DIR} "${CMAKE_CURRENT_BINARY_DIR}/OuterSpatialEngine")

# Set the default Visual Studio startup project to the worker itself. This only has an effect from
# CMake 3.6 onwards.
set(VS_STARTUP_PROJECT ${PROJECT_NAME})

# The worker binary.
file(GLOB_RECURSE SOURCE_FILES
    "src/*.cc"
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp")
add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_link_libraries(${PROJECT_NAME} WorkerSdk Schema OuterSpatialEngine)

# Creates a zip file with the worker executable and the dynamic worker library.
create_worker_zip(${PROJECT_NAME})
