# Editor support (include/tsql/editor.hpp): completion and colouring on the generated parsers.
# tools/g2to4/editor_meta.py reads each generated parser's keyword metadata (which words its
# actions and predicates recognise by text) into ${CMAKE_CURRENT_BINARY_DIR}/editor_meta_<G>.inc.
set(TSQL_EDITOR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(parser_bin ${PROJECT_BINARY_DIR}/src/parser)
set(parser_gen ${parser_bin}/generated)
set(editor_meta_py ${TSQL_EDITOR_ROOT}/tools/g2to4/editor_meta.py)
file(GLOB parser_base_sources CONFIGURE_DEPENDS ${TSQL_EDITOR_ROOT}/src/parser/*.cpp ${TSQL_EDITOR_ROOT}/src/parser/*.h
    ${TSQL_EDITOR_ROOT}/src/parser/helpers/*)

# SqlVersionFlags bit of each grammar's version (SqlScriptDOM SqlVersionFlags)
set(flag_TSql130 0x20)
set(flag_TSql140 0x40)
set(flag_TSql150 0x80)
set(flag_TSql160 0x100)
set(flag_TSql170 0x200)
set(flag_TSqlFabricDW 0x400)
set(flag_TSql180 0x800)

set(editor_grammar_sources)
set(editor_meta_files)
foreach(g IN LISTS TSQL_PARSER_GRAMMARS)
    set(parser_sources ${parser_gen}/${g}/${g}Parser.cpp)
    foreach(k RANGE 1 ${TSQL_PARSER_PARTS})
        list(APPEND parser_sources ${parser_gen}/${g}/${g}Parser_${k}.cpp)
    endforeach()
    set(meta ${CMAKE_CURRENT_BINARY_DIR}/editor_meta_${g}.inc)
    add_custom_command(
        OUTPUT ${meta}
        COMMAND ${Python3_EXECUTABLE} -B ${editor_meta_py} --grammar ${g} --parser-dir ${parser_gen}/${g}
            --support ${parser_gen}/support --base-dir ${TSQL_EDITOR_ROOT}/src/parser --out ${meta}
        DEPENDS ${editor_meta_py} ${parser_sources} ${parser_gen}/${g}/grammar/${g}Parser.g4
            ${parser_gen}/support/CodeGenerationSupporter.h ${parser_gen}/support/OptionHelpers.h
            ${parser_gen}/support/ParserEnums.h ${parser_base_sources}
        COMMENT "editor_meta: keyword metadata of ${g}"
        VERBATIM)
    list(APPEND editor_meta_files ${meta})
    string(REPLACE "TSql" "Sql" v ${g})
    set(G ${g})
    set(V ${v})
    set(FLAG ${flag_${g}})
    configure_file(EditorGrammar.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/EditorGrammar${g}.cpp @ONLY)
    list(APPEND editor_grammar_sources ${CMAKE_CURRENT_BINARY_DIR}/EditorGrammar${g}.cpp)
endforeach()
add_custom_target(tsql_editor_generate DEPENDS ${editor_meta_files})
add_dependencies(tsql_editor_generate tsql_parser_generate)

add_library(tsql_editor STATIC
    ${editor_grammar_sources}
    ${editor_meta_files}
    Buffer.cpp
    Builtins.cpp
    Classify.cpp
    Complete.cpp
    Document.cpp
    Grammar.cpp
    Names.cpp
    Scope.cpp
    Session.cpp
    Walker.cpp
)
add_dependencies(tsql_editor tsql_editor_generate)
add_library(tsql::editor ALIAS tsql_editor)
target_include_directories(tsql_editor
    PUBLIC ${TSQL_EDITOR_ROOT}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${TSQL_EDITOR_ROOT}/src/parser ${parser_bin})
target_link_libraries(tsql_editor PUBLIC tsql_parser PRIVATE antlr4_static)
target_compile_features(tsql_editor PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tsql_editor PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(tsql_editor_probe ${PROJECT_SOURCE_DIR}/pilot/editor_probe.cpp)
target_link_libraries(tsql_editor_probe PRIVATE tsql_editor antlr4_static)
target_include_directories(tsql_editor_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${TSQL_EDITOR_ROOT}/src/parser ${parser_bin})
