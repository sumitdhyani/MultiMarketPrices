project(DataDumper)
set(ALL_SOURCES DataDumper.cpp)

# find_package(PkgConfig REQUIRED)
# pkg_check_modules(RDKAFKA REQUIRED rdkafka++ rdkafka)

add_executable("${PROJECT_NAME}" "${ALL_SOURCES}")
add_dependencies("${PROJECT_NAME}" MiddleWare)

target_include_directories("${PROJECT_NAME}" PUBLIC
                            "${CMAKE_SOURCE_DIR}"
                            "${CMAKE_SOURCE_DIR}/ThirdParty")
target_link_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
target_link_libraries("${PROJECT_NAME}" MiddleWare boost_json)

