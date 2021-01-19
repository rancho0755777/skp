cmake_minimum_required(VERSION 3.0)
project(skp)

if (NOT UNIX)
	message(FATAL_ERROR "not support this platform!!!")
endif(NOT UNIX)

#set(CMAKE_C_COMPILER "/usr/bin/gcc")
#set(CMAKE_CXX_COMPILER "/usr/bin/g++")

set(WERROR OFF CACHE BOOL "Treat Warnings as Errors.")
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared libraries")
set(INSTALL_TEMP ON CACHE BOOL "Install project to /tmp/install")

find_package(Threads REQUIRED)
if(THREADS_HAVE_PTHREAD_ARG)
	add_compile_options(-pthread)
endif()

include(./cmake/Utils.cmake)
include(./cmake/FindThread.cmake)
include(./cmake/FindOpenssl.cmake)

option(UMALLOC_MANGLE "usage umalloc?" ON)
option(SPINLOCK_DEBUG "debug spinlock?" OFF)
option(RWLOCK_DEBUG "debug rwlock?" OFF)
option(MUTEX_DEBUG "debug mutex?" OFF)
option(RWSEM_DEBUG "debug rwsem?" OFF)
option(BUDDY_DEBUG "debug buddy system?" OFF)
option(EVENT_DEBUG "debug event?" OFF)
option(XPRT_DEBUG "debug xprt?" OFF)
option(SLAB_DEBUG "debug slab memory alloctor?" OFF)
option(DICT_DEBUG "debug dict?" OFF)
option(ENABLE_SSL "debug slab memory alloctor?" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LOWER)

if ("${CMAKE_BUILD_TYPE_LOWER}" STREQUAL "debug")
	#add_definitions(-DDEBUG -DCONFIG_LOG_COLOR)
	add_definitions(-DDEBUG)
endif()

set(SKP_LIBRARIES_OUTPUT_DIR /tmp/skp/lib)

set(EXECUTABLE_OUTPUT_PATH /tmp/skp/bin)
set(LIBRARY_OUTPUT_PATH ${SKP_LIBRARIES_OUTPUT_DIR})
link_directories(${SKP_LIBRARIES_OUTPUT_DIR} /usr/local/lib)
include_directories(/usr/local/include ${PROJECT_SOURCE_DIR}/include)

# 是否为临时安装路径
set_unix_options()
add_subdirectory(src)

include(CTest)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

message(STATUS "CMAKE_SYSTEM:             " ${CMAKE_SYSTEM} )
message(STATUS "CMAKE_SYSTEM_NAME:        " ${CMAKE_SYSTEM_NAME} )
message(STATUS "CMAKE_SYSTEM_VERSION:     " ${CMAKE_SYSTEM_VERSION} )
message(STATUS "CMAKE_SYSTEM_PROCESSOR:   " ${CMAKE_SYSTEM_PROCESSOR} )
message(STATUS "CMAKE_C_FLAGS:            " ${CMAKE_C_FLAGS} )
message(STATUS "CMAKE_CXX_FLAGS:          " ${CMAKE_CXX_FLAGS} )
message(STATUS "CMAKE_BUILD_TYPE:         " ${CMAKE_BUILD_TYPE} )
message(STATUS "CMAKE_C_COMPILER:         " ${CMAKE_C_COMPILER} )
message(STATUS "CMAKE_C_COMPILER_ID:      " ${CMAKE_C_COMPILER_ID})
message(STATUS "CMAKE_CXX_COMPILER:       " ${CMAKE_CXX_COMPILER} )
message(STATUS "CMAKE_CXX_COMPILER_ID:    " ${CMAKE_CXX_COMPILER_ID})