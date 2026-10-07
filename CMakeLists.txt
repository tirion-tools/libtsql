cmake_minimum_required(VERSION 3.16)
project(tsql
    VERSION 0.2.0
    DESCRIPTION "C++ T-SQL lexer, parser and editor support (port of Microsoft SqlScriptDOM)"
    LANGUAGES CXX
)

option(TSQL_BUILD_TESTS "Build libtsql tests" ON)
option(TSQL_INSTALL     "Install libtsql targets" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(tsql STATIC src/lexer.cpp)
add_library(tsql::tsql ALIAS tsql)

target_include_directories(tsql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(tsql PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tsql PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(TSQL_BUILD_TESTS)
    enable_testing()
    add_executable(test_lexer tests/test_lexer.cpp)
    target_link_libraries(test_lexer PRIVATE tsql)
    add_test(NAME tsql_lexer COMMAND test_lexer)
endif()

# Parsers (TSQL_BUILD_PARSER): AST generated from SqlScriptDOM's Ast.xml and ANTLR 4 parsers
# converted from its TSql<ver>.g grammars, both at build time, and the editor support on top of
# them. Fetches its pinned inputs (SqlScriptDOM, ANTLR 4 tool and C++ runtime) and needs Python 3
# and Java; see cmake/TsqlParser.cmake.
option(TSQL_BUILD_PARSER "Build the AST, the T-SQL parsers and the editor support" OFF)
if(TSQL_BUILD_PARSER)
    include(cmake/TsqlParser.cmake)
    add_subdirectory(src/ast)
    add_subdirectory(src/parser)
    add_subdirectory(src/editor)
    if(TSQL_BUILD_TESTS)
        add_subdirectory(tests/editor)
        add_subdirectory(tests/editor_internal)
    endif()
endif()

# Package tsql (find_package(tsql CONFIG)): tsql::tsql, plus tsql::ast, tsql::parser and
# tsql::editor (components ast, parser, editor) when built with TSQL_BUILD_PARSER.
if(TSQL_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(TSQL_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/tsql)
    install(TARGETS tsql EXPORT tsqlTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES include/tsql/tsql.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tsql)
    if(TSQL_BUILD_PARSER)
        # The parsers and the editor link the ANTLR 4 runtime built from the fetched source
        # (cmake/TsqlParser.cmake) privately, so it is installed and exported with them: under a
        # name of its own (it carries local patches and must not replace an ANTLR runtime
        # installed in the same prefix) and without its headers, which no public header includes.
        get_target_property(antlr_includes antlr4_static INTERFACE_INCLUDE_DIRECTORIES)
        list(FILTER antlr_includes EXCLUDE REGEX "^\\$<INSTALL_INTERFACE:")
        set_target_properties(antlr4_static PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${antlr_includes}"
            OUTPUT_NAME tsql_antlr4_runtime
            EXPORT_NAME antlr4_runtime)
        install(TARGETS tsql_ast tsql_parser tsql_editor antlr4_static EXPORT tsqlTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(FILES include/tsql/parser.hpp include/tsql/editor.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tsql)
        install(DIRECTORY include/tsql/ast DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tsql)
        install(DIRECTORY ${TSQL_AST_GENERATED_INCLUDE_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(FILES ${tsql_antlr4_SOURCE_DIR}/LICENSE.txt
            DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/tsql RENAME LICENSE.antlr4.txt)
    endif()
    install(EXPORT tsqlTargets NAMESPACE tsql:: DESTINATION ${TSQL_INSTALL_CMAKEDIR})
    configure_package_config_file(cmake/tsqlConfig.cmake.in ${PROJECT_BINARY_DIR}/tsqlConfig.cmake
        INSTALL_DESTINATION ${TSQL_INSTALL_CMAKEDIR})
    write_basic_package_version_file(${PROJECT_BINARY_DIR}/tsqlConfigVersion.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES ${PROJECT_BINARY_DIR}/tsqlConfig.cmake ${PROJECT_BINARY_DIR}/tsqlConfigVersion.cmake
        DESTINATION ${TSQL_INSTALL_CMAKEDIR})
endif()
