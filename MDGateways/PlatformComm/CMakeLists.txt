project(PlatformComm)
set(ALL_SOURCES PlatformComm.cpp PerPartitionSM.cpp)

add_library("${PROJECT_NAME}" "${ALL_SOURCES}")
add_dependencies("${PROJECT_NAME}" MiddleWare)

target_include_directories("${PROJECT_NAME}" PUBLIC
                            "${CMAKE_SOURCE_DIR}"
                            "${CMAKE_CURRENT_SOURCE_DIR}/../"
                            "${CMAKE_SOURCE_DIR}/ThirdParty")
target_link_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
target_link_libraries("${PROJECT_NAME}" MiddleWare NanoLog ssl crypto boost_json)

