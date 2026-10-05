# Contract tests for the editor-support layer (tsql_editor, include/tsql/editor.hpp): completion and
# classification cases in this directory, run by test_completion (case format: see test_completion.cpp), and
# Document against the free functions, run by test_document.
if(NOT TARGET tsql_editor)
    message(STATUS "tsql_editor not built; editor contract tests skipped")
    return()
endif()

foreach(runner test_completion test_document)
    add_executable(${runner} ${runner}.cpp)
    target_link_libraries(${runner} PRIVATE tsql_editor tsql_parser)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${runner} PRIVATE -Wall -Wextra)
    endif()
endforeach()
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
# Document (incremental buffer) against the free functions over randomized edit sequences.
add_test(NAME tsql_editor_document
    COMMAND test_document ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tsql_editor_document_as_TSqlFabricDW
    COMMAND test_document --as-version TSqlFabricDW ${CMAKE_CURRENT_SOURCE_DIR})
# Document::Classify cost after an edit; timing-based, so only meaningful in optimised builds.
add_test(NAME tsql_editor_document_perf
    COMMAND test_document --suite perf ${CMAKE_CURRENT_SOURCE_DIR})
