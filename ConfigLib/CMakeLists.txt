project(ConfigLib)
set (ALL_SOURCES ConfigLib.cpp)
add_library("${PROJECT_NAME}" STATIC  "${ALL_SOURCES}")
target_include_directories("${PROJECT_NAME}" PUBLIC
							"${CMAKE_SOURCE_DIR}"
							"${CMAKE_SOURCE_DIR}/ThirdParty")
target_link_libraries("${PROJECT_NAME}" PUBLIC NanoLog boost_json)
