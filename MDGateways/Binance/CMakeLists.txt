project(BinanceMD)
set(ALL_SOURCES Binance.cpp RestApi.cpp)

add_executable("${PROJECT_NAME}" "${ALL_SOURCES}")
add_dependencies("${PROJECT_NAME}" PlatformComm NanoLog ConfigLib)

target_include_directories("${PROJECT_NAME}" PUBLIC
                            "${CMAKE_SOURCE_DIR}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../"
                            "${CMAKE_SOURCE_DIR}/ThirdParty"
                            "${CMAKE_SOURCE_DIR}/ThirdParty/MTTools")
target_link_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
target_link_libraries("${PROJECT_NAME}" PlatformComm NanoLog ConfigLib ssl crypto boost_json)

