cmake_minimum_required (VERSION 2.8)

project (batyr)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
SET(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
SET(BUILD_SHARED_LIBS OFF)
SET(CMAKE_EXE_LINKER_FLAGS "-static")

# http://stackoverflow.com/questions/7724569/debug-vs-release-in-cmake
# Initialize CXXFLAGS.
set(CMAKE_CXX_FLAGS                "-Wall -Wno-format-extra-args -Wextra -Wformat-nonliteral -Wformat-security -Wformat=2")
set(CMAKE_CXX_FLAGS_DEBUG          "-O0 -g -D_DEBUG=1")
set(CMAKE_CXX_FLAGS_MINSIZEREL     "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE        "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

# Compiler-specific C++11 activation.
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
    if ((GCC_VERSION VERSION_GREATER 4.7 OR GCC_VERSION VERSION_EQUAL 4.7))
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    elseif (GCC_VERSION VERSION_EQUAL 4.6)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
    else ()
        message(FATAL_ERROR "${PROJECT_NAME} requires g++ 4.6 or greater.")
    endif ()
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    #set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++11")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else ()
    message(FATAL_ERROR "Your C++ compiler does not support C++11.")
endif ()

include(CheckLibraryExists)

find_package( Threads REQUIRED)

find_package( GDAL REQUIRED)

find_package( Postgres REQUIRED)
# PQescapeIdentifier exists starting with libpq version 9.0+
CHECK_LIBRARY_EXISTS(pq PQescapeIdentifier "libpq-fe.h" HAVE_PQ_ESCAPE_IDENTIFIER)
if (HAVE_PQ_ESCAPE_IDENTIFIER)
    message(STATUS "Escaping SQL Identifiers locally using libpq")
else ()
    message(STATUS "Escaping SQL Identifiers remotely on the database server")
endif ()

find_package( Poco REQUIRED Foundation Util Net)


#
# Manual project configuration
#

# version information
set(VERSION_MAJOR 0)
set(VERSION_MINOR 3)
set(VERSION_PATCH 0)

# http://brianmilco.blogspot.de/2012/11/cmake-automatically-use-git-tags-as.html
include(GetGitRevisionDescription)
get_git_head_revision(refspec VERSION_GIT_SHA1)
get_git_is_dirty(VERSION_GIT_IS_DIRTY)
STRING(SUBSTRING "${VERSION_GIT_SHA1}" 0 10 VERSION_GIT_SHA1_SHORT)
if ("${VERSION_GIT_IS_DIRTY}" STREQUAL "")
    set(VERSION_GIT_FULL "${VERSION_GIT_SHA1_SHORT}")
else()
    set(VERSION_GIT_FULL "${VERSION_GIT_SHA1_SHORT}-${VERSION_GIT_IS_DIRTY}")
endif()


# compile the http web gui in the server when this is enabled
option(ENABLE_HTTP_WEB_GUI "Enable the HTTP Web user interface" ON)
if (ENABLE_HTTP_WEB_GUI)
    message(STATUS "Building with the Web GUI")
else ()
    message(STATUS "Building without the Web GUI")
endif ()


add_subdirectory(src)

# install additional files
install(FILES   README.md
                MANUAL.md
                DEVELOPMENT.md
                batyrd.cfg.sample
    DESTINATION share/${PROJECT_NAME}
)

# vim: ft=cmake
