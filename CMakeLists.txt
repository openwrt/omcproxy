cmake_minimum_required(VERSION 3.13)


# Project Definition
project(omcproxy LANGUAGES C)
add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE
	src/client.c
	src/groups.c
	src/igmp.c
	src/mld.c
	src/mrib.c
	src/omcproxy.c
	src/proxy.c
	src/querier.c
)


# Compiler Options
set_target_properties(${PROJECT_NAME} PROPERTIES C_STANDARD 11)
target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE)
target_compile_options(${PROJECT_NAME} PRIVATE -g3)
target_compile_options(${PROJECT_NAME} PRIVATE -Os)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
target_compile_options(${PROJECT_NAME} PRIVATE -Werror)
target_compile_options(${PROJECT_NAME} PRIVATE -Wextra)
target_compile_options(${PROJECT_NAME} PRIVATE -Werror=implicit-function-declaration)
target_compile_options(${PROJECT_NAME} PRIVATE -Wformat)
target_compile_options(${PROJECT_NAME} PRIVATE -Werror=format-security)
target_compile_options(${PROJECT_NAME} PRIVATE -Werror=format-nonliteral)
target_compile_options(${PROJECT_NAME} PRIVATE -Wno-unused-parameter)
target_compile_options(${PROJECT_NAME} PRIVATE -Wmissing-declarations)


# Libraries
find_path(ubox_include_dir libubox/uloop.h)
target_include_directories(${PROJECT_NAME} PRIVATE ${ubox_include_dir})
find_library(libubox ubox)
target_link_libraries(${PROJECT_NAME} PRIVATE ${libubox})


# Optional Features
if(${L_LEVEL})
	target_compile_definitions(${PROJECT_NAME} PRIVATE L_LEVEL=${L_LEVEL})
endif(${L_LEVEL})


# Installation
install(TARGETS ${PROJECT_NAME} DESTINATION sbin/)
