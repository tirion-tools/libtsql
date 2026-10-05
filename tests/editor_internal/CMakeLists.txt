# Unit tests for the internals of the editor support (src/editor); the contract tests are in tests/editor.
add_executable(test_editor_internal test_editor_internal.cpp)
target_link_libraries(test_editor_internal PRIVATE tsql_editor tsql_parser antlr4_static)
target_include_directories(test_editor_internal PRIVATE
    ${PROJECT_SOURCE_DIR}/src/editor ${PROJECT_SOURCE_DIR}/src/parser ${PROJECT_BINARY_DIR}/src/parser)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_editor_internal PRIVATE -Wall -Wextra)
endif()
add_test(NAME tsql_editor_internal COMMAND test_editor_internal)
