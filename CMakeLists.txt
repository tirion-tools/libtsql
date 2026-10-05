cmake_minimum_required(VERSION 3.16)
project(tsql
    VERSION 0.1.0
    DESCRIPTION "C++ T-SQL lexer (port of Microsoft SqlScriptDOM)"
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

# ANTLR 4 parser pilot (branch antlr4-pilot): AST generated from SqlScriptDOM's Ast.xml and
# parsers converted from its TSql<ver>.g grammars, both at build time. Fetches its pinned inputs
# (SqlScriptDOM, ANTLR 4 tool and C++ runtime) and needs Python 3 and Java; see
# cmake/TsqlParserPilot.cmake.
option(TSQL_BUILD_PARSER_PILOT "Build the ANTLR 4 parser pilot" OFF)
if(TSQL_BUILD_PARSER_PILOT)
    include(cmake/TsqlParserPilot.cmake)
    add_subdirectory(src/ast)
    add_subdirectory(src/parser)
    add_subdirectory(src/editor)
    if(TSQL_BUILD_TESTS)
        add_subdirectory(tests/editor)
        add_subdirectory(tests/editor_internal)
    endif()
endif()

if(TSQL_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS tsql EXPORT tsqlTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
