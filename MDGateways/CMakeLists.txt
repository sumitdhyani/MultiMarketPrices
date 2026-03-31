project(MDFsm)

add_library("${PROJECT_NAME}" PerPartitionSM.cpp)
target_include_directories("${PROJECT_NAME}" PRIVATE 
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/ThirdParty")

add_subdirectory(Binance)