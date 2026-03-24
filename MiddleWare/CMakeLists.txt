project(MiddleWare)
set(ALL_SOURCES Interface.cpp)
add_library("${PROJECT_NAME}" STATIC  "${ALL_SOURCES}")