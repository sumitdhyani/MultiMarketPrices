project(MiddleWare)
set(ALL_SOURCES Interface.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RDKAFKA REQUIRED rdkafka++ rdkafka)

add_library("${PROJECT_NAME}" STATIC  "${ALL_SOURCES}")

target_include_directories("${PROJECT_NAME}" PUBLIC
                            "${CMAKE_SOURCE_DIR}"
                            "${CMAKE_SOURCE_DIR}/ThirdParty"
                            "${CMAKE_SOURCE_DIR}/ThirdParty/modern-cpp-kafka/include")

target_link_libraries("${PROJECT_NAME}" rdkafka++ rdkafka)