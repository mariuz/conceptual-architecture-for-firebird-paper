cmake_minimum_required(VERSION 3.16)
project(firebird-paper-samples CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Public API headers come from the Firebird submodule checked out at the
# repository root; no generated files are needed for the client-side API.
set(FIREBIRD_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/firebird/src/include"
    CACHE PATH "Directory containing firebird/Interface.h")

# libfbclient comes from an installed Firebird client package
# (e.g. firebird-dev on Debian/Ubuntu, firebird-devel on Fedora).
find_library(FBCLIENT_LIBRARY NAMES fbclient
    HINTS /usr/lib /usr/local/lib /opt/firebird/lib)
if(NOT FBCLIENT_LIBRARY)
    message(FATAL_ERROR
        "libfbclient not found - install the Firebird client library "
        "(firebird-dev / firebird-devel) or pass -DFBCLIENT_LIBRARY=...")
endif()

add_executable(client_test client_test.cpp)
target_include_directories(client_test PRIVATE "${FIREBIRD_INCLUDE_DIR}")
target_link_libraries(client_test PRIVATE "${FBCLIENT_LIBRARY}")

# Networked OO-API client used by the wire-protocol write-up; the C++
# counterpart to nodejs/query.js (see ../firebird-wire-protocol.md).
add_executable(protocol_client protocol_client.cpp)
target_include_directories(protocol_client PRIVATE "${FIREBIRD_INCLUDE_DIR}")
target_link_libraries(protocol_client PRIVATE "${FBCLIENT_LIBRARY}")
