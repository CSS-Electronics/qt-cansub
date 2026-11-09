cmake_minimum_required(VERSION 3.30)

project(CANsub)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

set(CMAKE_AUTOMOC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(USE_QT6 "Build against Qt6 instead of Qt5" OFF)

# Version strings
set(CANSUB_DATEVER "2026.11.09")
set(CANSUB_DATEVER_COMPAT "20261109")

set(QT_REQUIRED_COMPONENTS Core Network SerialBus)

if(USE_QT6)
    find_package(QT NAMES Qt6 REQUIRED)
else()
    find_package(QT NAMES Qt5 REQUIRED)
endif()

# Find required dependencies
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_REQUIRED_COMPONENTS})

# ----------------------------------------------------------------------------------------------------------------------
# Third-party dependencies
# ----------------------------------------------------------------------------------------------------------------------

# qmdnsengine is linked statically into the plugin. It picks up QT_VERSION and QT_VERSION_MAJOR from this scope, so it
# is built against the same Qt as the plugin.
block()
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(thirdparty/qmdnsengine EXCLUDE_FROM_ALL)
endblock()

# The CANsub library (c-cpp-sdk-cansub) is fetched at a fixed tag and linked statically into the plugin. Its
# "cansub::cansub" target provides the include path and the CANSUB_STATIC definition. To build against a local checkout instead, e.g.
# without access to GitHub, set FETCHCONTENT_SOURCE_DIR_CANSUB to the root of that checkout.
include(FetchContent)
FetchContent_Declare(cansub
    GIT_REPOSITORY https://github.com/CSS-Electronics/c-cpp-sdk-cansub.git
    GIT_TAG 2026-11-09
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
block()
    # The CANsub library is plain C, without anything for moc.
    set(CMAKE_AUTOMOC OFF)
    FetchContent_MakeAvailable(cansub)
endblock()

# ----------------------------------------------------------------------------------------------------------------------
# Common interface library for settings
# ----------------------------------------------------------------------------------------------------------------------
set(COMMON_TARGET_NAME "${PROJECT_NAME}Common-Qt${QT_VERSION_MAJOR}")
add_library(${COMMON_TARGET_NAME} INTERFACE)

if(${MSVC})
    # Use the conformant lambda processor. The default (legacy) processor incorrectly considers any use of a member
    # of the enclosing class inside a lambda as requiring a 'this' capture, even calls to static members such as
    # QObject::connect (warning C4573). Conformant by default only from /std:c++20, so request it explicitly here.
    target_compile_options(${COMMON_TARGET_NAME} INTERFACE /Zc:lambda)

    target_compile_options(${COMMON_TARGET_NAME} INTERFACE /Gy /Gw)
else()
    target_compile_options(${COMMON_TARGET_NAME} INTERFACE -ffunction-sections -fdata-sections)
endif()

# General Qt configuration.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Building debug version")
    target_compile_definitions(${COMMON_TARGET_NAME} INTERFACE QT_DEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "Building release version")
    target_compile_definitions(${COMMON_TARGET_NAME} INTERFACE QT_NO_DEBUG)
    target_compile_definitions(${COMMON_TARGET_NAME} INTERFACE QT_NO_DEBUG_OUTPUT)
else()
    message(FATAL_ERROR "Building unsupported version: \"${CMAKE_BUILD_TYPE}\"")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Core part of the plugin. An OBJECT library.
# ----------------------------------------------------------------------------------------------------------------------
set(CORE_TARGET_NAME "${PROJECT_NAME}Core-Qt${QT_VERSION_MAJOR}")

set(SOURCE_FILES_CORE
    src/Core/CANsub.cxx
    src/Core/CANsub.hxx
    src/Core/CANsubConnection.cxx
    src/Core/CANsubConnection.hxx
    src/Core/CANsubFrames.cxx
    src/Core/CANsubFrames.hxx
    src/Core/CANsubRest.cxx
    src/Core/CANsubRest.hxx
    src/Core/CANsubTarget.cxx
    src/Core/DeviceDiscovery.cxx
    src/Core/DeviceDiscovery.hxx
    src/Core/HostLookup.cxx
    src/Core/HostLookup.hxx
    src/Core/Logging.hxx
    src/Core/MetaTypes.cxx
    src/Core/MetaTypes.hxx
    src/Core/TcpKeepAlive.cxx
    src/Core/TcpKeepAlive.hxx
)

add_library(${CORE_TARGET_NAME} OBJECT ${SOURCE_FILES_CORE})
target_link_libraries(${CORE_TARGET_NAME} PUBLIC ${COMMON_TARGET_NAME})
foreach (QT_COMPONENT ${QT_REQUIRED_COMPONENTS})
    target_link_libraries(${CORE_TARGET_NAME} PUBLIC Qt${QT_VERSION_MAJOR}::${QT_COMPONENT})
endforeach ()

# HostLookup resolves ".local" names with mDNS
target_link_libraries(${CORE_TARGET_NAME} PUBLIC qmdnsengine)

if(WIN32)
    # Keep-alive tuning of the TCP sockets uses WSAIoctl()
    target_link_libraries(${CORE_TARGET_NAME} PUBLIC ws2_32)
endif()

target_include_directories(${CORE_TARGET_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/Core")

# The CANsub C library, consumed through its FFI header and linked statically
target_link_libraries(${CORE_TARGET_NAME} PUBLIC cansub::cansub)

if(WIN32)
    # The static CANsub library bundles the Mbed TLS objects, but does not pass on the system libraries they need.
    # The entropy source of Mbed TLS uses BCryptGenRandom().
    target_link_libraries(${CORE_TARGET_NAME} PUBLIC bcrypt)
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Module part of the plugin: Only the factory code Qt needs to find and load the plugin. Everything else belongs in the
# core part above.
# ----------------------------------------------------------------------------------------------------------------------

set(TARGET_NAME "${PROJECT_NAME}-Qt${QT_VERSION_MAJOR}")

configure_file(src/Plugin/plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin.json" @ONLY)

set(SOURCE_FILES_PLUGIN
    src/Plugin/CANsubPlugin.cxx
    src/Plugin/CANsubPlugin.hxx
    ${CMAKE_CURRENT_BINARY_DIR}/generated/plugin.json
)

add_library(${TARGET_NAME} MODULE ${SOURCE_FILES_PLUGIN})
target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(${TARGET_NAME} PRIVATE ${COMMON_TARGET_NAME} ${CORE_TARGET_NAME})

# Qt loads CAN bus plugins from the "canbus" folder of each plugin path, so the build folder "plugins" can be used as
# QT_PLUGIN_PATH directly, e.g. by the tests or canbusutil.
set_target_properties(${TARGET_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/canbus")

if(UNIX AND NOT APPLE)
    target_link_options(${TARGET_NAME} PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.map")
    # The CANsub header gives its functions default visibility, so without this the plugin would export every function
    # of the static libraries it links (CANsub and qmdnsengine)
    target_link_options(${TARGET_NAME} PRIVATE "-Wl,--exclude-libs,ALL")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(${TARGET_NAME} PRIVATE "-Wl,-s")
    endif()
endif()

if(MSVC)
    target_link_options(${TARGET_NAME} PRIVATE /OPT:REF /OPT:ICF $<$<CONFIG:Debug>:/INCREMENTAL:NO> $<$<CONFIG:Release>:/DEBUG:NONE>)
else()
    target_link_options(${TARGET_NAME} PRIVATE "-Wl,--gc-sections")
endif()
