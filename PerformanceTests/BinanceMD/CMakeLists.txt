project(BinanceMDPerformanceTests)
set(ALL_SOURCES BinanceMDPerformanceTests.cpp)

add_executable("${PROJECT_NAME}" "${ALL_SOURCES}")
add_dependencies("${PROJECT_NAME}" MiddleWare NanoLog ConfigLib)

target_include_directories("${PROJECT_NAME}" PUBLIC
                            "${CMAKE_SOURCE_DIR}"
                            "${CMAKE_SOURCE_DIR}/ThirdParty"
                            /usr/local/include)
target_link_directories("${PROJECT_NAME}" PRIVATE
                            "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}"
                            /usr/local/lib)
target_link_libraries("${PROJECT_NAME}" MiddleWare NanoLog ConfigLib boost_json gtest gtest_main pthread)
