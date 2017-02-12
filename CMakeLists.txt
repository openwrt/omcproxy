cmake_minimum_required(VERSION 2.8.8)

project(omcproxy C)

set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=c99")

add_definitions(-D_GNU_SOURCE -Wall -Wno-gnu)

if(${L_LEVEL})
  add_definitions(-DL_LEVEL=${L_LEVEL})
endif(${L_LEVEL})

FIND_PATH(ubox_include_dir libubox/list.h)
INCLUDE_DIRECTORIES(${ubox_include_dir})

if(WITH_LIBUBOX)
  add_definitions(-Wextra)
  set(PLATFORM_LINK ${PLATFORM_LINK} ubox)
else (WITH_LIBUBOX)
  add_definitions(-Dtypeof=__typeof)
  include_directories(BEFORE .)
  set(PLATFORM_SOURCE ${PLATFORM_SOURCE} libubox/uloop.c libubox/avl.c libubox/blobmsg.c libubox/blob.c)
endif(WITH_LIBUBOX)

add_executable(omcproxy src/client.c src/mrib.c src/querier.c src/groups.c src/igmp.c src/mld.c src/proxy.c src/omcproxy.c ${PLATFORM_SOURCE})
target_link_libraries(omcproxy ${PLATFORM_LINK})

install(TARGETS omcproxy DESTINATION sbin/)


