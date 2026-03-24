project(MultiMarketPrices)

if(NOT CMAKE_BUILD_TYPE )
        set(CMAKE_BUILD_TYPE Release)
endif()

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        set(GTEST_LIB_SUFFIX d)
else()
        set(GTEST_LIB_SUFFIX "")
endif()

if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
	set(CMAKE_CXX_STANDARD 23)
	cmake_minimum_required(VERSION 3.5.0)
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
	set(CMAKE_EXECUTABLE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
endif()

set(GTEST_LIBS  "gtest${GTEST_LIB_SUFFIX}"
	        "gtest_main${GTEST_LIB_SUFFIX}"
	        pthread)

add_subdirectory(MiddleWare)