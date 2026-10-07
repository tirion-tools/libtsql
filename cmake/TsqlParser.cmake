# Inputs of the ANTLR 4 parsers (TSQL_BUILD_PARSER), fetched by the build and pinned:
#   - Microsoft SqlScriptDOM @ eaf3a6e (MIT): Ast.xml, the TSql<ver>.g grammars and the C# sources
#     the generators read (tools/astgen, tools/g2to4)
#   - ANTLR 4.13.2: the tool (complete jar, run with Java at build time) and the C++ runtime
#     (built here as the static library antlr4_static), with local warning fixes from
#     cmake/patches/antlr4-runtime.cmake (grep [libtsql] in the runtime sources)
# Needs Python 3 and a Java runtime. Everything generated lives in the build tree.
if(CMAKE_VERSION VERSION_LESS 3.28)   # FetchContent_Declare(... EXCLUDE_FROM_ALL SYSTEM)
    message(FATAL_ERROR "TSQL_BUILD_PARSER needs CMake 3.28 or newer")
endif()
cmake_policy(VERSION 3.28)   # include() scopes policies: the rest of the project is unaffected
include(FetchContent)

set(TSQL_SSD_COMMIT eaf3a6e8cf49350c7600bf70691de6314594ffba)
set(TSQL_SSD_COMMIT_SHORT eaf3a6e)
set(TSQL_ANTLR_VERSION 4.13.2)

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "TSQL_BUILD_PARSER needs a Python 3 interpreter: the AST and the "
                        "parsers are generated at build time by tools/astgen and tools/g2to4.")
endif()
find_package(Java COMPONENTS Runtime)
if(NOT Java_Runtime_FOUND OR NOT Java_VERSION_STRING)   # FindJava "finds" a java that fails to run
    message(FATAL_ERROR "TSQL_BUILD_PARSER needs a Java runtime (java): the ANTLR "
                        "${TSQL_ANTLR_VERSION} tool runs at build time to generate the parsers.")
endif()

FetchContent_Declare(tsql_sqlscriptdom
    URL https://github.com/microsoft/SqlScriptDOM/archive/${TSQL_SSD_COMMIT}.tar.gz
    URL_HASH SHA256=b4a2cbb1b04c31c7f11f2513801c85d61f03217bfe1a5fda43210c09778aa1c7
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)

# The runtime's own CMake project (runtime/Cpp): static library only, no tests (they would fetch
# googletest), no demo, no install rules of its own beyond the library's.
set(ANTLR_BUILD_CPP_TESTS OFF CACHE BOOL "" FORCE)
set(ANTLR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ANTLR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(WITH_DEMO OFF CACHE BOOL "" FORCE)
set(ANTLR4_INSTALL OFF CACHE BOOL "" FORCE)
# The patch runs once per population of the source. The script is a configure dependency and its
# hash is in the patch command, so editing it re-runs CMake, which re-runs the patch on the existing
# source. FETCHCONTENT_SOURCE_DIR_TSQL_ANTLR4 skips patching: a source given that way must already
# carry cmake/patches/antlr4-runtime.cmake (cmake -P it in that directory).
set(_tsql_antlr_patch ${CMAKE_CURRENT_LIST_DIR}/patches/antlr4-runtime.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_tsql_antlr_patch})
file(SHA256 ${_tsql_antlr_patch} _tsql_antlr_patch_hash)
FetchContent_Declare(tsql_antlr4
    URL https://github.com/antlr/antlr4/archive/refs/tags/${TSQL_ANTLR_VERSION}.tar.gz
    URL_HASH SHA256=9f18272a9b32b622835a3365f850dd1063d60f5045fb1e12ce475ae6e18a35bb
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    PATCH_COMMAND ${CMAKE_COMMAND} -DTSQL_PATCH_HASH=${_tsql_antlr_patch_hash} -P ${_tsql_antlr_patch}
    SOURCE_SUBDIR runtime/Cpp
    EXCLUDE_FROM_ALL
    SYSTEM)

FetchContent_MakeAvailable(tsql_sqlscriptdom tsql_antlr4)
set(TSQL_SSD_DIR ${tsql_sqlscriptdom_SOURCE_DIR})

# ANTLR tool (Maven Central; its published SHA-1 is 7df86c341abb175a0f4b76a7845074cc45f82ff8)
set(TSQL_ANTLR_JAR ${FETCHCONTENT_BASE_DIR}/antlr-${TSQL_ANTLR_VERSION}-complete.jar)
set(_tsql_antlr_jar_sha256 eae2dfa119a64327444672aff63e9ec35a20180dc5b8090b7a6ab85125df4d76)
set(_tsql_have_jar FALSE)
if(EXISTS ${TSQL_ANTLR_JAR})
    file(SHA256 ${TSQL_ANTLR_JAR} _tsql_jar_hash)
    if(_tsql_jar_hash STREQUAL _tsql_antlr_jar_sha256)
        set(_tsql_have_jar TRUE)
    endif()
endif()
if(NOT _tsql_have_jar)
    message(STATUS "Downloading antlr-${TSQL_ANTLR_VERSION}-complete.jar")
    file(DOWNLOAD
        https://repo1.maven.org/maven2/org/antlr/antlr4/${TSQL_ANTLR_VERSION}/antlr4-${TSQL_ANTLR_VERSION}-complete.jar
        ${TSQL_ANTLR_JAR}
        EXPECTED_HASH SHA256=${_tsql_antlr_jar_sha256}
        TLS_VERIFY ON
        STATUS _tsql_jar_status)
    list(GET _tsql_jar_status 0 _tsql_jar_code)
    if(NOT _tsql_jar_code EQUAL 0)
        list(GET _tsql_jar_status 1 _tsql_jar_message)
        message(FATAL_ERROR "Downloading the ANTLR ${TSQL_ANTLR_VERSION} tool failed: ${_tsql_jar_message}")
    endif()
endif()
