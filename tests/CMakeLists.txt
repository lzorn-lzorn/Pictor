# Tests — lightweight test executables registered with CTest.
#
# No external framework: every test uses the tiny harness in TestFramework.h.
# Test data files (e.g. config.ini) live in this directory and are located
# through the TEST_DATA_DIR compile definition, so tests work no matter which
# working directory they are launched from (CTest, an IDE, or a shell).
#
# `InfraTests` is a convenience target that builds every test executable at
# once; the tests themselves run through CTest (`ctest --test-dir <build>`).
set(INFRA_TEST_TARGETS "")

function(pictor_add_test TestName)
    add_executable(${TestName} ${ARGN})

    target_link_libraries(${TestName} PRIVATE Infra)
    target_include_directories(${TestName} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${TestName} PRIVATE cxx_std_23)
    target_compile_definitions(${TestName} PRIVATE
        TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

    if(MSVC)
        target_compile_options(${TestName} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${TestName} PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME ${TestName} COMMAND ${TestName})
    set(INFRA_TEST_TARGETS ${INFRA_TEST_TARGETS} ${TestName} PARENT_SCOPE)
endfunction()

# serde module tests
pictor_add_test(IniFileReaderTests IniFileReaderTests.cpp)
pictor_add_test(SerdeEngineTests SerdeEngineTests.cpp)

# containers module tests
pictor_add_test(IntrusiveRBTreeTests IntrusiveRBTreeTests.cpp)

add_custom_target(InfraTests DEPENDS ${INFRA_TEST_TARGETS})
