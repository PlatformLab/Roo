################################################################################
##  TESTS  #####################################################################
################################################################################

# Thread library
find_package(Threads)

include(FetchContent)
FetchContent_Declare(
    docopt
    GIT_REPOSITORY https://github.com/docopt/docopt.cpp.git
    GIT_TAG        3dd23e3280f213bacefdf5fcb04857bf52e90917
)

FetchContent_GetProperties(docopt)
if(NOT docopt_POPULATED)
    FetchContent_Populate(docopt)
    add_subdirectory(
        ${docopt_SOURCE_DIR}
        ${docopt_BINARY_DIR}
        EXCLUDE_FROM_ALL
    )
endif()

## system_test #################################################################

add_executable(system_test
    system_test.cc
)
target_include_directories(system_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(system_test
    Roo::Roo
    Homa::FakeDriver
    Threads::Threads
    docopt
)

