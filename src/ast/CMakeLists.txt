# C++ AST generated from SqlScriptDOM's Ast.xml (tools/astgen/astgen.py, run at build time into
# the build tree) plus hand-ported TSqlFragment / TSqlParserToken / partial-class members.
set(TSQL_AST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ---- generated sources (astgen writes a file only when its content changes)
set(TSQL_AST_VISITOR_CHUNKS 4)
set(gen_inc_root ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
set(gen_inc ${gen_inc_root}/tsql/ast/generated)
set(gen_src ${CMAKE_CURRENT_BINARY_DIR}/generated/src)
set(TSQL_AST_GENERATED_HEADERS)
foreach(f token_types.hpp enums.hpp nodes.hpp fragment_type_id.hpp visitor_members.inc concrete_visitor_members.inc)
    list(APPEND TSQL_AST_GENERATED_HEADERS ${gen_inc}/${f})
endforeach()
set(TSQL_AST_GENERATED_SOURCES ${gen_src}/enums.cpp ${gen_src}/dump.cpp)
math(EXPR last_chunk "${TSQL_AST_VISITOR_CHUNKS} - 1")
foreach(i RANGE ${last_chunk})
    list(APPEND TSQL_AST_GENERATED_SOURCES ${gen_src}/visitor_${i}.cpp ${gen_src}/accept_${i}.cpp)
endforeach()
# the directory is a dependency too: adding a partial re-runs astgen without re-running CMake
file(GLOB partial_incs ${TSQL_AST_ROOT}/include/tsql/ast/partial/*.inc)
list(APPEND partial_incs ${TSQL_AST_ROOT}/include/tsql/ast/partial)
add_custom_command(
    OUTPUT ${TSQL_AST_GENERATED_HEADERS} ${TSQL_AST_GENERATED_SOURCES}
    COMMAND ${Python3_EXECUTABLE} -B ${TSQL_AST_ROOT}/tools/astgen/astgen.py
        --ssd ${TSQL_SSD_DIR} --ssd-commit ${TSQL_SSD_COMMIT_SHORT}
        --partial-dir ${TSQL_AST_ROOT}/include/tsql/ast/partial
        --out-include ${gen_inc} --out-src ${gen_src} --chunks ${TSQL_AST_VISITOR_CHUNKS}
    DEPENDS ${TSQL_AST_ROOT}/tools/astgen/astgen.py ${partial_incs}
        ${TSQL_SSD_DIR}/SqlScriptDom/Parser/TSql/Ast.xml
        ${TSQL_SSD_DIR}/SqlScriptDom/Parser/TSql/TSqlTokenTypes.g
    COMMENT "astgen: generating the AST from SqlScriptDOM's Ast.xml"
    VERBATIM)
add_custom_target(tsql_ast_generate DEPENDS ${TSQL_AST_GENERATED_HEADERS} ${TSQL_AST_GENERATED_SOURCES})
# for tools/g2to4 (src/parser): the generated member enums
set(TSQL_AST_ENUMS_HPP ${gen_inc}/enums.hpp PARENT_SCOPE)

add_library(tsql_ast STATIC
    token.cpp
    fragment.cpp
    fragment_factory.cpp
    dump.cpp
    enum_format.cpp
    visitor.cpp
    partial/Identifier.cpp
    partial/IdentifierOrValueExpression.cpp
    ${TSQL_AST_GENERATED_SOURCES}
    ${TSQL_AST_GENERATED_HEADERS}
)
add_dependencies(tsql_ast tsql_ast_generate)
add_library(tsql::ast ALIAS tsql_ast)
target_include_directories(tsql_ast
    PUBLIC ${TSQL_AST_ROOT}/include ${gen_inc_root}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(tsql_ast PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tsql_ast PRIVATE -Wall -Wextra)
endif()

if(TSQL_BUILD_TESTS)
    foreach(test ast_dump ast_visitor)
        add_executable(test_${test} ${TSQL_AST_ROOT}/tests/ast/test_${test}.cpp)
        target_link_libraries(test_${test} PRIVATE tsql_ast)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(test_${test} PRIVATE -Wall -Wextra)
        endif()
        add_test(NAME tsql_${test} COMMAND test_${test})
    endforeach()
endif()
