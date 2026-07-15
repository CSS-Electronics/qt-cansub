cmake_minimum_required(VERSION 3.30)

project(CANsub)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)

option(USE_QT6 "Build against Qt6 instead of Qt5" OFF)

# Version strings
set(CANSUB_DATEVER "2026.07.14")
set(CANSUB_DATEVER_COMPAT "20260714")

set(QT_REQUIRED_COMPONENTS Core Network SerialBus WebSockets)

if(USE_QT6)
    find_package(QT NAMES Qt6 REQUIRED)

    # In Qt 6, the state machine has been extracted into a separate library
    list(APPEND QT_REQUIRED_COMPONENTS StateMachine)
else()
    find_package(QT NAMES Qt5 REQUIRED)
endif()

# Find required dependencies
find_package(Boost REQUIRED COMPONENTS crc endian)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_REQUIRED_COMPONENTS})

# Manually add required third-party dependencies.
add_subdirectory(thirdparty/OAPI)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build QMdnsEngine as a shared library" FORCE)
add_subdirectory(thirdparty/qmdnsengine)

# ----------------------------------------------------------------------------------------------------------------------
# Common interface library for settings
# ----------------------------------------------------------------------------------------------------------------------
set(COMMON_TARGET_NAME "${PROJECT_NAME}Common-Qt${QT_VERSION_MAJOR}")
add_library(${COMMON_TARGET_NAME} INTERFACE)

set_property(TARGET ${COMMON_TARGET_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET ${COMMON_TARGET_NAME} PROPERTY CXX_STANDARD_REQUIRED YES)
set_property(TARGET ${COMMON_TARGET_NAME} PROPERTY CXX_EXTENSIONS NO)

if(${MSVC})
    # Use the conformant lambda processor. The default (legacy) processor incorrectly considers any use of a member
    # of the enclosing class inside a lambda as requiring a 'this' capture, even calls to static members such as
    # QObject::connect (warning C4573). Conformant by default only from /std:c++20, so request it explicitly here.
    target_compile_options(${COMMON_TARGET_NAME} INTERFACE /Zc:lambda)
endif()

# General Qt configuration.
if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    message(STATUS "Building debug version")
    target_compile_definitions(${COMMON_TARGET_NAME} INTERFACE QT_DEBUG)
elseif(${CMAKE_BUILD_TYPE} STREQUAL "Release")
    message(STATUS "Building release version")
    target_compile_definitions(${COMMON_TARGET_NAME} INTERFACE QT_NO_DEBUG)
    target_compile_definitions(${COMMON_TARGET_NAME} INTERFACE QT_NO_DEBUG_OUTPUT)
    if(${MSVC})
        set_target_properties(${COMMON_TARGET_NAME} PROPERTIES LINK_FLAGS_RELEASE "/DEBUG:NONE")
    endif()
else()
    message(FATAL_ERROR "Building unsupported version: \"${${CMAKE_BUILD_TYPE}}\"")
endif()

# ----------------------------------------------------------------------------------------------------------------------
# Core part of the plugin. Contains all logic, and can be statically linked
# ----------------------------------------------------------------------------------------------------------------------
set(CORE_TARGET_NAME "${PROJECT_NAME}Core-Qt${QT_VERSION_MAJOR}")

set(SOURCE_FILES_CORE
    src/Core/BatchTracker.cxx
    src/Core/BatchTracker.hxx
    src/Core/CANsub.cxx
    src/Core/CANsub.hxx
    src/Core/CANsubHelper.cxx
    src/Core/CANsubHelper.hxx
    src/Core/CANsubProtocol.cxx
    src/Core/CANsubProtocol.hxx
    src/Core/Certificate.cxx
    src/Core/Certificate.hxx
    src/Core/FrameBuffer.cxx
    src/Core/FrameBuffer.hxx
    src/Core/GuardedTransition.cxx
    src/Core/GuardedTransition.hxx
    src/Core/HostResolver.cxx
    src/Core/HostResolver.hxx
    src/Core/LogicTransition.cxx
    src/Core/LogicTransition.hxx
    src/Core/NetworkManagerWithPeerInfo.cxx
    src/Core/NetworkManagerWithPeerInfo.hxx
    src/Core/QtVersionHelper.hxx
    src/Core/ResolverFactory.hxx
    src/Core/TlsPolicy.hxx
)

# Embed the root certificate into the core library (see Certificate.cxx for the Q_INIT_RESOURCE handling required when
# the resource lives in an OBJECT/static library rather than the final target).
qt_add_resources(SOURCE_FILES_CORE src/Core/resources.qrc)

add_library(${CORE_TARGET_NAME} OBJECT ${SOURCE_FILES_CORE})
set_property(TARGET ${CORE_TARGET_NAME} PROPERTY AUTOMOC ON)
set_property(TARGET ${CORE_TARGET_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(${CORE_TARGET_NAME} PUBLIC ${COMMON_TARGET_NAME})
target_link_libraries(${CORE_TARGET_NAME} PUBLIC Boost::boost)
foreach (QT_COMPONENT ${QT_REQUIRED_COMPONENTS})
    target_link_libraries(${CORE_TARGET_NAME} PUBLIC Qt${QT_VERSION_MAJOR}::${QT_COMPONENT})
endforeach ()
target_link_libraries(${CORE_TARGET_NAME} PUBLIC qmdnsengine)
target_link_libraries(${CORE_TARGET_NAME} PUBLIC QtOpenAPIClient)

target_include_directories(${CORE_TARGET_NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/Core")

# ----------------------------------------------------------------------------------------------------------------------
# Module part of the plugin. Contains all the factory implementation the Qt CAN subsystem looks for
# ----------------------------------------------------------------------------------------------------------------------

set(TARGET_NAME "${PROJECT_NAME}-Qt${QT_VERSION_MAJOR}")

configure_file(src/Plugin/plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin.json" @ONLY)

set(SOURCE_FILES_PLUGIN
    src/Plugin/CANsubPlugin.cxx
    src/Plugin/CANsubPlugin.hxx
    src/Plugin/DeferredMdnsHostResolver.cxx
    src/Plugin/DeferredMdnsHostResolver.hxx
    src/Plugin/MdnsHostResolver.cxx
    src/Plugin/MdnsHostResolver.hxx
    ${CMAKE_CURRENT_BINARY_DIR}/generated/plugin.json
)

add_library(${TARGET_NAME} MODULE ${SOURCE_FILES_PLUGIN})
set_property(TARGET ${TARGET_NAME} PROPERTY AUTOMOC ON)
target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(${TARGET_NAME} PRIVATE ${COMMON_TARGET_NAME} ${CORE_TARGET_NAME})

if(UNIX AND NOT APPLE)
    target_link_options(${TARGET_NAME} PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.map")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(${TARGET_NAME} PRIVATE "-Wl,-s")
    endif()
endif()
