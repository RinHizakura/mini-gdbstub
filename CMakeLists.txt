cmake_minimum_required(VERSION 3.25)

project(mini-gdbstub LANGUAGES C)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

file(GLOB_RECURSE SOURCES lib/*.c)
add_library(${PROJECT_NAME} STATIC ${SOURCES})
target_include_directories(
    ${PROJECT_NAME} PUBLIC 
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_options(${PROJECT_NAME} PUBLIC -Wall -Wextra -MMD)
set_target_properties(
    ${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "gdbstub"
    PUBLIC_HEADER "include/gdbstub.h"
)

include(install_action)
