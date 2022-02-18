cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)


############# LIBRARY ############
project(OuterSpatialEngine)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(OuterSpatialEngine outerspatial_engine.h traders/AI_trader.h common/agent.h common/messages.h auction/auction_house.h metrics/logger.h traders/inventory.h common/commodity.h common/history.h traders/fake_trader.h metrics/display.h common/concurrency.h traders/human_trader.h common/to_schema.h)
set_target_properties(OuterSpatialEngine PROPERTIES LINKER_LANGUAGE CXX)

target_link_libraries(OuterSpatialEngine PRIVATE Threads::Threads WorkerSdk)

# Should create a libOuterSpatialEngine.a object file