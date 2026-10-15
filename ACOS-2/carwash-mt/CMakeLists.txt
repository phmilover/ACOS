cmake_minimum_required(VERSION 3.16)
project(carwash_mt C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # нужен clang-tidy

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# -DCW_SANITIZE=thread|address: инструментированная сборка, бинарники с суффиксом -tsan/-asan
set(CW_SANITIZE "" CACHE STRING "thread, address или пусто")
set(SUFFIX "")
set(SAN_FLAGS "")
if(CW_SANITIZE STREQUAL "thread")
    set(SUFFIX "-tsan")
    set(SAN_FLAGS -fsanitize=thread -g)
elseif(CW_SANITIZE STREQUAL "address")
    set(SUFFIX "-asan")
    set(SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g)
elseif(NOT CW_SANITIZE STREQUAL "")
    message(FATAL_ERROR "CW_SANITIZE: thread, address или пусто")
endif()

# бинарники кладём в bin/, чтобы не искать их внутри build/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/*.c)
list(FILTER CORE_SOURCES EXCLUDE REGEX "/main\\.c$")
file(GLOB HEADERS CONFIGURE_DEPENDS include/*.h tests/*.h)
set(TEST_SOURCES tests/unit_tests.c tests/run_tests.c)

set(COMMON_DEFS _POSIX_C_SOURCE=200809L)
if(APPLE)
    list(APPEND COMMON_DEFS _DARWIN_C_SOURCE)
endif()
set(COMMON_OPTS -Wall -Wextra -Werror ${SAN_FLAGS})
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND COMMON_OPTS -Wthread-safety -Wthread-safety-beta)
endif()
find_package(Threads REQUIRED)

# ядро: всё из src/ кроме main.c
add_library(cwcore STATIC ${CORE_SOURCES})
target_include_directories(cwcore PUBLIC include)
target_compile_definitions(cwcore PUBLIC ${COMMON_DEFS})
target_compile_options(cwcore PUBLIC ${COMMON_OPTS})
target_link_options(cwcore PUBLIC ${SAN_FLAGS})
target_link_libraries(cwcore PUBLIC Threads::Threads)
set_target_properties(cwcore PROPERTIES OUTPUT_NAME cwcore${SUFFIX})

# цели появляются, когда есть их исходники (main.c и run_tests.c приходят в следующих задачах)
set(CHECK_DEPS "")
if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.c)
    add_executable(carwash-mt src/main.c)
    target_link_libraries(carwash-mt PRIVATE cwcore)
    set_target_properties(carwash-mt PROPERTIES OUTPUT_NAME carwash-mt${SUFFIX})
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/unit_tests.c)
    add_executable(unit_tests tests/unit_tests.c)
    target_link_libraries(unit_tests PRIVATE cwcore)
    set_target_properties(unit_tests PROPERTIES OUTPUT_NAME unit_tests${SUFFIX})
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/run_tests.c)
    add_executable(run_tests tests/run_tests.c)
    target_link_libraries(run_tests PRIVATE cwcore)
    set_target_properties(run_tests PROPERTIES OUTPUT_NAME run_tests${SUFFIX})
endif()

# cmake --build build --target check: сначала модульные, затем сценарные тесты
enable_testing()
if(TARGET unit_tests)
    add_test(NAME unit COMMAND unit_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    list(APPEND CHECK_CMDS COMMAND unit_tests)
    list(APPEND CHECK_DEPS unit_tests)
endif()
if(TARGET run_tests AND TARGET carwash-mt)
    add_test(NAME scenarios COMMAND run_tests $<TARGET_FILE:carwash-mt>
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    list(APPEND CHECK_CMDS COMMAND run_tests $<TARGET_FILE:carwash-mt>)
    list(APPEND CHECK_DEPS run_tests carwash-mt)
endif()
add_custom_target(check ${CHECK_CMDS} DEPENDS ${CHECK_DEPS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)

# clang-format и clang-tidy. В Homebrew llvm не попадает в PATH, поэтому смотрим и туда
set(LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(CLANG_FORMAT clang-format HINTS ${LLVM_HINTS})
find_program(CLANG_TIDY clang-tidy HINTS ${LLVM_HINTS})
set(MAIN_SOURCE "")
if(EXISTS ${CMAKE_SOURCE_DIR}/src/main.c)
    set(MAIN_SOURCE src/main.c)
endif()
set(TIDY_SOURCES ${CORE_SOURCES} ${MAIN_SOURCE})
foreach(t ${TEST_SOURCES})
    if(EXISTS ${CMAKE_SOURCE_DIR}/${t})
        list(APPEND TIDY_SOURCES ${t})
    endif()
endforeach()
set(ALL_CODE ${TIDY_SOURCES} ${HEADERS})

if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${ALL_CODE}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found: brew install llvm (or apt install clang-format)"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()

# clang-tidy из Homebrew сам не знает, где на macOS системные заголовки
set(TIDY_EXTRA "")
if(APPLE)
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(TIDY_EXTRA --extra-arg=-isysroot --extra-arg=${MACOS_SDK})
endif()

if(CLANG_TIDY)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --quiet ${TIDY_EXTRA} ${TIDY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    add_custom_target(tidy
        COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found: brew install llvm (or apt install clang-tidy)"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()

# Doxygen: cmake --build build --target docs, открыть build/doxygen/html/index.html
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "doxygen not found: brew install doxygen (or apt install doxygen)"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()
