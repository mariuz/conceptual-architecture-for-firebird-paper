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

# Event-notification demo (queEvents/POST_EVENT, commit-time delivery);
# accompanies ../firebird-events.md.
add_executable(events_demo events_demo.cpp)
target_include_directories(events_demo PRIVATE "${FIREBIRD_INCLUDE_DIR}")
target_link_libraries(events_demo PRIVATE "${FBCLIENT_LIBRARY}")
find_package(Threads REQUIRED)
target_link_libraries(events_demo PRIVATE Threads::Threads)

# Per-document hands-on samples: one small program per companion document,
# all sharing cpp/fb_sample.h.  Each cpp/<topic>.cpp builds to <topic>.
# Debug info (-g) is always included so the gdb walk-throughs in the
# documents' "Hands-on" sections work out of the box.
file(GLOB HANDSON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*.cpp")
foreach(src ${HANDSON_SOURCES})
    get_filename_component(name "${src}" NAME_WE)
    add_executable("${name}" "${src}")
    target_include_directories("${name}" PRIVATE
        "${FIREBIRD_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/cpp")
    target_link_libraries("${name}" PRIVATE "${FBCLIENT_LIBRARY}" Threads::Threads)
    target_compile_options("${name}" PRIVATE -g)
endforeach()

# fb-cpp twins: the same hands-on samples written against fb-cpp
# (https://github.com/asfernandes/fb-cpp), the modern C++ wrapper over the
# OO API, vendored as the extern/fb-cpp submodule.  The library is built
# directly from its sources here — no vcpkg needed; it requires C++20 and
# the Boost headers (libboost-dev + libboost-filesystem-dev on
# Debian/Ubuntu: Boost.DLL loads fbclient at runtime and pulls in
# Boost.Filesystem; Boost.Multiprecision powers INT128/DECFLOAT).
set(FBCPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/fb-cpp/src")
if(EXISTS "${FBCPP_DIR}/fb-cpp/fb-cpp.h")
    find_package(Boost COMPONENTS filesystem)
    if(Boost_FOUND)
        file(GLOB FBCPP_LIB_SOURCES "${FBCPP_DIR}/fb-cpp/*.cpp")
        add_library(fbcpp STATIC ${FBCPP_LIB_SOURCES})
        target_include_directories(fbcpp PUBLIC "${FBCPP_DIR}" "${FIREBIRD_INCLUDE_DIR}")
        target_compile_features(fbcpp PUBLIC cxx_std_20)
        target_link_libraries(fbcpp PUBLIC Boost::filesystem ${CMAKE_DL_LIBS} Threads::Threads)
        target_compile_options(fbcpp PRIVATE -g)

        file(GLOB FBCPP_SAMPLES "${CMAKE_CURRENT_SOURCE_DIR}/fb-cpp/*.cpp")
        foreach(src ${FBCPP_SAMPLES})
            get_filename_component(name "${src}" NAME_WE)
            add_executable("fbcpp_${name}" "${src}")
            target_include_directories("fbcpp_${name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fb-cpp")
            target_link_libraries("fbcpp_${name}" PRIVATE fbcpp)
            target_compile_options("fbcpp_${name}" PRIVATE -g)
        endforeach()
    else()
        message(WARNING "Boost not found - skipping the fb-cpp samples "
            "(install libboost-dev and libboost-filesystem-dev)")
    endif()
else()
    message(WARNING "extern/fb-cpp submodule not checked out - skipping the "
        "fb-cpp samples (run: git submodule update --init extern/fb-cpp)")
endif()
