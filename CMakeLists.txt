cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

project(workflow_objc)

include(CTest)
include(GoogleTest)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.h)

if(DEMO)
    add_library(${PROJECT_NAME} STATIC ${SOURCES})
else()
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
endif()

if(NOT BN_INTERNAL_BUILD)
    find_path(
        BN_API_PATH
        NAMES binaryninjaapi.h
        HINTS binaryninjaapi binaryninja-api $ENV{BN_API_PATH}
        REQUIRED)
    add_subdirectory(${BN_API_PATH} binaryninjaapi)
endif()

target_link_libraries(${PROJECT_NAME} binaryninjaapi)

if(APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,-dead_strip -Wl,-undefined,dynamic_lookup)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    set(WORKFLOW_OBJC_TEST_SOURCES ${SOURCES})
    list(FILTER WORKFLOW_OBJC_TEST_SOURCES EXCLUDE REGEX ".*/Plugin\\.cpp$")

    set(WORKFLOW_OBJC_GTEST_ROOT "${BN_API_PATH}/vendor/fmt/test/gtest")
    add_library(${PROJECT_NAME}_gtest STATIC
        ${WORKFLOW_OBJC_GTEST_ROOT}/gmock-gtest-all.cc)
    target_include_directories(${PROJECT_NAME}_gtest SYSTEM PUBLIC
        ${WORKFLOW_OBJC_GTEST_ROOT})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(${PROJECT_NAME}_gtest PRIVATE -Wno-character-conversion)
    endif()
    set_target_properties(${PROJECT_NAME}_gtest PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)

    add_executable(${PROJECT_NAME}_tests
        tests/TestHelpers.cpp
        ${WORKFLOW_OBJC_TEST_SOURCES})
    target_link_libraries(${PROJECT_NAME}_tests
        ${PROJECT_NAME}_gtest
        binaryninjaapi)
    target_include_directories(${PROJECT_NAME}_tests PRIVATE src)
    set_target_properties(${PROJECT_NAME}_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)
    gtest_discover_tests(${PROJECT_NAME}_tests
        TEST_PREFIX workflow_objc.
        DISCOVERY_MODE PRE_TEST)
endif()

if(BN_INTERNAL_BUILD)
    plugin_rpath(${PROJECT_NAME})
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${BN_CORE_PLUGIN_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${BN_CORE_PLUGIN_DIR})
else()
    bn_install_plugin(${PROJECT_NAME})
endif()
