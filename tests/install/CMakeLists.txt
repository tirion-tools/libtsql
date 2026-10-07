# Consumer of an installed libtsql, built by CI against `cmake --install` output so that packaging
# cannot regress:
#   cmake -S tests/install -B <dir> -DCMAKE_PREFIX_PATH=<prefix> [-DTSQL_WITH_PARSER=ON]
# lexer_consumer always (tsql::tsql); editor_consumer (tsql::editor) with TSQL_WITH_PARSER, for a
# libtsql built with TSQL_BUILD_PARSER. Both exit non-zero when a result is not the expected one.
cmake_minimum_required(VERSION 3.16)
project(tsql_install_test LANGUAGES CXX)

option(TSQL_WITH_PARSER "The installed libtsql was built with TSQL_BUILD_PARSER" OFF)
if(TSQL_WITH_PARSER)
    find_package(tsql 0.2 CONFIG REQUIRED COMPONENTS editor)
else()
    find_package(tsql 0.2 CONFIG REQUIRED)
    if(TARGET tsql::editor)
        message(FATAL_ERROR "a libtsql built without TSQL_BUILD_PARSER exports tsql::editor")
    endif()
endif()

add_executable(lexer_consumer lexer_consumer.cpp)
target_link_libraries(lexer_consumer PRIVATE tsql::tsql)

if(TSQL_WITH_PARSER)
    add_executable(editor_consumer editor_consumer.cpp)
    target_link_libraries(editor_consumer PRIVATE tsql::editor)
endif()
