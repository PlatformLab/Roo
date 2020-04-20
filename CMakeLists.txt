cmake_minimum_required (VERSION 3.11)

project(Roo VERSION 0.1.0.0 LANGUAGES CXX)

################################################################################
## Dependencies ################################################################

find_package(Homa REQUIRED)

################################################################################
## Target Definition ###########################################################

add_library(Roo
    src/Debug.cc
    src/Roo.cc
    src/RooPCImpl.cc
    src/ServerTaskImpl.cc
    src/SocketImpl.cc
    src/StringUtil.cc
    src/ThreadId.cc
)
add_library(Roo::Roo ALIAS Roo)
target_include_directories(Roo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(Roo
    PRIVATE
        Homa::Homa
)
target_compile_features(Roo
    PUBLIC
        cxx_std_11
)
target_compile_options(Roo
    PRIVATE
        -Wall
        -Wextra
)
set_target_properties(Roo PROPERTIES
    VERSION ${Roo_VERSION}
)

################################################################################
## Install & Export ############################################################

install(TARGETS Roo EXPORT RooTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(
    DIRECTORY
        include/Roo
    DESTINATION
        include
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/Roo/RooConfigVersion.cmake"
    VERSION ${Roo_VERSION}
    COMPATIBILITY SameMinorVersion
)

export(EXPORT RooTargets
    NAMESPACE Roo::
    FILE "${CMAKE_CURRENT_BINARY_DIR}/Roo/RooTargets.cmake"
)

install(EXPORT RooTargets
    DESTINATION lib/cmake/Roo
    NAMESPACE Roo::
    FILE RooTargets.cmake
)

configure_file(cmake/RooConfig.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/Roo/RooConfig.cmake"
    COPYONLY
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/Roo/RooConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/Roo/RooConfigVersion.cmake"
        DESTINATION lib/cmake/Roo
)

################################################################################
##  Unit Tests  ################################################################

include(FetchContent)

## Google Test #################################################################
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        release-1.10.0
)
FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
    FetchContent_Populate(googletest)
    add_subdirectory(
        ${googletest_SOURCE_DIR}
        ${googletest_BINARY_DIR}
        EXCLUDE_FROM_ALL
    )
endif()
include(GoogleTest)

enable_testing()
add_executable(unit_test
    src/DebugTest.cc
    src/RooTest.cc
    src/RooPCImplTest.cc
    src/ServerTaskImplTest.cc
    src/SocketImplTest.cc
    src/SpinLockTest.cc
    src/StringUtilTest.cc
    src/ThreadIdTest.cc
)
target_link_libraries(unit_test Roo gmock_main)
# -fno-access-control allows access to private members for testing
target_compile_options(unit_test PRIVATE -fno-access-control)
gtest_discover_tests(unit_test)

################################################################################
##  Additional Tests  ##########################################################

add_subdirectory(test)
