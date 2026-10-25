cmake_minimum_required(VERSION 3.20.0)

Include(FetchContent)

project(
    ovis
    VERSION 0.1.0
    LANGUAGES C CXX
)

# Set before fetching dependencies: Catch2 compiles its string_view support
# only under C++17 or later, and Apple clang defaults to an older standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.8.1
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.3
  GIT_SHALLOW    TRUE
)
option(OVIS_BUILTIN_LLM "Run the translation model in-process with llama.cpp" ON)
set(OVIS_EMBED_MODEL "" CACHE FILEPATH
    "GGUF model to embed in the ovis executable (optional)")
# Source builds tune llama.cpp to this machine (GGML_NATIVE). A binary meant
# for other machines is built with OVIS_PORTABLE: no native tuning, but AVX2,
# FMA and F16C stay on for x86-64 (NATIVE off alone made questions 20x
# slower); on ARM the NEON kernels need no flag.
option(OVIS_PORTABLE "Build the model runtime to run on other CPUs" OFF)
if(OVIS_BUILTIN_LLM)
  if(OVIS_PORTABLE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
      set(GGML_AVX ON CACHE BOOL "" FORCE)
      set(GGML_AVX2 ON CACHE BOOL "" FORCE)
      set(GGML_FMA ON CACHE BOOL "" FORCE)
      set(GGML_F16C ON CACHE BOOL "" FORCE)
    endif()
  endif()
  FetchContent_Declare(
    llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        b11200
    GIT_SHALLOW    TRUE
  )
  set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
  set(LLAMA_BUILD_COMMON OFF CACHE INTERNAL "")
  set(LLAMA_BUILD_TESTS OFF CACHE INTERNAL "")
  set(LLAMA_BUILD_TOOLS OFF CACHE INTERNAL "")
  set(LLAMA_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(LLAMA_BUILD_SERVER OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(llama)
endif()

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(HTTPLIB_COMPILE OFF CACHE INTERNAL "")
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE INTERNAL "")
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(Catch2 nlohmann_json httplib)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
if(LLVM_LINK_LLVM_DYLIB)
  set(LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(LLVM_LIBS core support passes target native)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
file(GLOB LIBRARY_SRC_FILES CONFIGURE_DEPENDS ${SRC_DIR}/ovis/*.cpp ${SRC_DIR}/ovis/phrase/*.cpp
     ${SRC_DIR}/ovis/project/*.cpp)
file(GLOB TEST_FILES ${TEST_DIR}/*.cpp)

set(LIBRARY_NAME lib${CMAKE_PROJECT_NAME})
set(EXECUTABLE_NAME ${CMAKE_PROJECT_NAME})
set(TEST_NAME test_${CMAKE_PROJECT_NAME})

# The built-in phrase library, phrases/*.phr and its Ovis Core prelude
# phrases/*.core and *.generic, compiled into the binary.
file(GLOB PHRASE_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/phrases/*.phr
     ${CMAKE_CURRENT_SOURCE_DIR}/phrases/*.core
     ${CMAKE_CURRENT_SOURCE_DIR}/phrases/*.generic)
set(PHRASE_LIBRARY_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/phrase_library.cpp)
add_custom_command(
  OUTPUT ${PHRASE_LIBRARY_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DPHRASE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/phrases
          -DOUTPUT=${PHRASE_LIBRARY_SOURCE}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_phrases.cmake
  DEPENDS ${PHRASE_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_phrases.cmake
  COMMENT "Embedding the phrase library"
  VERBATIM
)

# eval/questions.txt, compiled in for `ovis check-phrases`.
set(QUESTION_CASES_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/question_cases.cpp)
add_custom_command(
  OUTPUT ${QUESTION_CASES_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/eval/questions.txt
          -DOUTPUT=${QUESTION_CASES_SOURCE} -DNAME=embedded_question_cases_text
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/eval/questions.txt
          ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
  COMMENT "Embedding the question cases"
  VERBATIM
)

# cmake/OvisMacros.cmake, compiled in for the CMake projects ovis generates
# from ovis.toml (proposals/projects.md §4), so they work from any ovis.
set(OVIS_MACROS_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/ovis_macros.cpp)
add_custom_command(
  OUTPUT ${OVIS_MACROS_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/OvisMacros.cmake
          -DOUTPUT=${OVIS_MACROS_SOURCE} -DNAME=embedded_ovis_macros
          -DHEADER=ovis/project/cmake_project.hpp -DNAMESPACE=ovis::project
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OvisMacros.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
  COMMENT "Embedding the CMake macros"
  VERBATIM
)

add_library(${LIBRARY_NAME} STATIC ${LIBRARY_SRC_FILES} ${PHRASE_LIBRARY_SOURCE}
            ${QUESTION_CASES_SOURCE} ${OVIS_MACROS_SOURCE})
target_include_directories(${LIBRARY_NAME} PUBLIC ${INC_DIR})
target_include_directories(${LIBRARY_NAME} SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_link_libraries(
    ${LIBRARY_NAME}
    PUBLIC ${LLVM_LIBS} nlohmann_json::nlohmann_json httplib::httplib
)

# libclang reads C headers so programs can use any C library.
find_library(LIBCLANG_LIBRARY NAMES clang libclang HINTS ${LLVM_LIBRARY_DIRS})
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS ${LLVM_INCLUDE_DIRS})
if(LIBCLANG_LIBRARY AND LIBCLANG_INCLUDE_DIR)
  message(STATUS "Found libclang: ${LIBCLANG_LIBRARY}")
  target_include_directories(${LIBRARY_NAME} SYSTEM PRIVATE ${LIBCLANG_INCLUDE_DIR})
  target_link_libraries(${LIBRARY_NAME} PUBLIC ${LIBCLANG_LIBRARY})
  target_compile_definitions(${LIBRARY_NAME} PUBLIC OVIS_HAS_C_IMPORT)
  # libclang's own headers (stddef.h): it finds them itself on Linux, not
  # always from Homebrew on macOS, so the build says where they are.
  get_filename_component(LIBCLANG_DIR "${LIBCLANG_LIBRARY}" DIRECTORY)
  file(GLOB LIBCLANG_RESOURCE_DIRS "${LIBCLANG_DIR}/clang/*")
  if(LIBCLANG_RESOURCE_DIRS)
    list(GET LIBCLANG_RESOURCE_DIRS -1 LIBCLANG_RESOURCE_DIR)
    target_compile_definitions(${LIBRARY_NAME} PRIVATE
      OVIS_CLANG_RESOURCE_DIR="${LIBCLANG_RESOURCE_DIR}")
  endif()
else()
  message(WARNING "libclang not found: (use-c ...) will be unavailable")
endif()

if(OVIS_BUILTIN_LLM)
  target_link_libraries(${LIBRARY_NAME} PUBLIC llama)
  target_compile_definitions(${LIBRARY_NAME} PUBLIC OVIS_HAS_BUILTIN_LLM
                             OVIS_LLAMA_VERSION="b11200")
endif()

add_executable(${EXECUTABLE_NAME} ${SRC_DIR}/main.cpp)
target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${LIBRARY_NAME})
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE
                           OVIS_VERSION="${PROJECT_VERSION}")
# Release packages (tools/package.sh, docs/deployment.md) carry LLVM,
# libclang and the libraries they need in lib/ beside bin/, so the installed
# ovis looks there first. On Linux an RPATH, not a RUNPATH: the loader then
# also uses it for the bundled libraries' own dependencies.
option(OVIS_RELOCATABLE "Look for shared libraries in ../lib beside ovis" OFF)
if(OVIS_RELOCATABLE)
  if(APPLE)
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
                          INSTALL_RPATH "@executable_path/../lib")
  else()
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
                          INSTALL_RPATH "$ORIGIN/../lib")
    target_link_options(${EXECUTABLE_NAME} PRIVATE "LINKER:--disable-new-dtags")
  endif()
endif()
if(OVIS_EMBED_MODEL)
  add_custom_command(
    TARGET ${EXECUTABLE_NAME} POST_BUILD
    COMMAND $<TARGET_FILE:${EXECUTABLE_NAME}> model embed ${OVIS_EMBED_MODEL}
            -o $<TARGET_FILE:${EXECUTABLE_NAME}>
    COMMENT "Embedding ${OVIS_EMBED_MODEL} in ovis"
    VERBATIM
  )
endif()

# Measures the built-in model's answers, to tune the prompts.
if(OVIS_BUILTIN_LLM)
  add_executable(ovis_eval tools/eval.cpp)
  target_link_libraries(ovis_eval PRIVATE ${LIBRARY_NAME})
endif()

# A small C library the tests import with (use-c "ovistest.h" "ovistest").
add_library(ovistest STATIC ${TEST_DIR}/clib/ovistest.c)
set_target_properties(ovistest PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/clib
    POSITION_INDEPENDENT_CODE ON)

add_executable(${TEST_NAME} ${TEST_FILES})
add_dependencies(${TEST_NAME} ovistest ${EXECUTABLE_NAME})
target_link_libraries(${TEST_NAME} PRIVATE ${LIBRARY_NAME} Catch2::Catch2WithMain)
target_compile_definitions(
    ${TEST_NAME} PRIVATE OVIS_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
                         OVIS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                         OVIS_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
                         OVIS_TEST_CLIB_INCLUDE="${TEST_DIR}/clib"
                         OVIS_TEST_CLIB_LIBRARY="${CMAKE_CURRENT_BINARY_DIR}/clib"
                         # The daemon test starts `ovis model serve`.
                         OVIS_EXECUTABLE="$<TARGET_FILE:${EXECUTABLE_NAME}>"
)
catch_discover_tests(${TEST_NAME})

# Installing: `ovis`, the runtime's C source and the package files that
# `find_package(Ovis)` reads (docs/c-libraries.md). Only this component is
# installed with `cmake --install build --component ovis`, not llama.cpp's.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(OVIS_RUNTIME_C ${CMAKE_CURRENT_BINARY_DIR}/ovis_runtime.c)
add_custom_command(
  OUTPUT ${OVIS_RUNTIME_C}
  COMMAND ${EXECUTABLE_NAME} runtime -o ${OVIS_RUNTIME_C}
  DEPENDS ${EXECUTABLE_NAME}
  COMMENT "Writing the runtime's C source"
  VERBATIM)
add_custom_target(ovis_runtime_source ALL DEPENDS ${OVIS_RUNTIME_C})
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/OvisConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(TARGETS ${EXECUTABLE_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ovis)
install(FILES ${OVIS_RUNTIME_C} DESTINATION ${CMAKE_INSTALL_DATADIR}/ovis
        COMPONENT ovis)
install(FILES cmake/OvisConfig.cmake cmake/OvisMacros.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/OvisConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Ovis COMPONENT ovis)
