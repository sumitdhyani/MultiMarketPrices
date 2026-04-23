cmake_minimum_required(VERSION 3.5.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(MultiMarketPrices)

if(NOT CMAKE_BUILD_TYPE )
        set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 23)
cmake_minimum_required(VERSION 3.5.0)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_EXECUTABLE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")

add_subdirectory(ThirdParty/NanoLog)
add_subdirectory(MiddleWare)
add_subdirectory(DataTumper)
add_subdirectory(MDGateways)
add_subdirectory(SDPMock)
add_subdirectory(ConfigLib)
add_subdirectory(Tests)
add_subdirectory(PerformanceTests)

