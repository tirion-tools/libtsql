# Contract tests for the editor-support layer (tsql_editor, include/tsql/editor.hpp): completion and
# classification cases in this directory, run by test_completion (case format: see test_completion.cpp).
if(NOT TARGET tsql_editor)
    message(STATUS "tsql_editor not built; editor contract tests skipped")
    return()
endif()

add_executable(test_completion test_completion.cpp)
target_link_libraries(test_completion PRIVATE tsql_editor tsql_parser)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_completion PRIVATE -Wall -Wextra)
endif()
add_test(NAME tsql_editor_cases_valid
    COMMAND test_completion --validate-only ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tsql_editor_completion
    COMMAND test_completion --suite completion ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tsql_editor_classify
    COMMAND test_completion --suite classify ${CMAKE_CURRENT_SOURCE_DIR})
# The version-independent completion cases (written for TSql170) under every other grammar.
foreach(version TSql130 TSql140 TSql150 TSql160 TSql180 TSqlFabricDW)
    add_test(NAME tsql_editor_completion_as_${version}
        COMMAND test_completion --suite completion --as-version ${version} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
