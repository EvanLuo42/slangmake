add_executable(slangmake_tests
        test_main.cpp
        test_enums.cpp
        test_permutation.cpp
        test_blob.cpp
        test_compile.cpp
        test_reflection.cpp
        test_batch.cpp
        test_include.cpp
        test_parallel.cpp
        test_incremental.cpp
        test_compression.cpp
        test_spirv_match.cpp
        test_type_specialization.cpp
        test_cursor.cpp
        test_cli.cpp)

target_link_libraries(slangmake_tests PRIVATE slangmake-lib)

# The CLI end-to-end tests invoke the built slangmake executable, so the tests
# must be rebuilt whenever the CLI changes.
add_dependencies(slangmake_tests slangmake)

target_compile_definitions(slangmake_tests PRIVATE
        SLANG_MAKE_TESTS_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        SLANG_MAKE_TESTS_CLI_EXE="$<TARGET_FILE:slangmake>")

include(ConfigureDoctest)
configure_doctest(slangmake_tests VERSION ${SLANG_MAKE_FETCH_DOCTEST_VERSION})

slang_copy_runtime_dlls(slangmake_tests)

add_test(NAME slangmake_tests COMMAND slangmake_tests)
