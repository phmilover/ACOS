cmake_minimum_required(VERSION 3.16)
project(carwash_mon C)

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

# служебные сборки (build-tsan, build-asan, build-tsa, build-cov) запускают цели ниже и сами их не
# определяют; у tsa и покрытия свой суффикс, чтобы не затирать обычные бинарники в bin/
set(CW_NESTED OFF CACHE BOOL "служебная сборка: без целей tsan/asan/tsa/coverage")
set(CW_SUFFIX "" CACHE STRING "суффикс бинарников вместо -tsan/-asan")
set(CW_COVERAGE OFF CACHE BOOL "инструментация для llvm-cov")
if(NOT CW_SUFFIX STREQUAL "")
    set(SUFFIX "${CW_SUFFIX}")
endif()
if(CW_COVERAGE)
    list(APPEND SAN_FLAGS -fprofile-instr-generate -fcoverage-mapping)
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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # без расширений (-std=c17) glibc не объявляет syscall() и SYS_gettid; на Linux не проверено
    list(APPEND COMMON_DEFS _DEFAULT_SOURCE)
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

add_executable(carwash-mon src/main.c)
target_link_libraries(carwash-mon PRIVATE cwcore)
set_target_properties(carwash-mon PROPERTIES OUTPUT_NAME carwash-mon${SUFFIX})
add_executable(unit_tests tests/unit_tests.c)
target_link_libraries(unit_tests PRIVATE cwcore)
set_target_properties(unit_tests PROPERTIES OUTPUT_NAME unit_tests${SUFFIX})
add_executable(run_tests tests/run_tests.c)
target_link_libraries(run_tests PRIVATE cwcore)
set_target_properties(run_tests PROPERTIES OUTPUT_NAME run_tests${SUFFIX})

# в src/, include/ и tests/ не должно быть семафоров (в том числе в комментариях)
set(NOSEM_PATTERN "sem_|semaphore\\.h|dispatch_semaphore|\\bSem[A-Z(]")
add_custom_target(nosem
    COMMAND sh -c "if grep -rnE '${NOSEM_PATTERN}' src include tests; then echo 'nosem: FAIL'; exit 1; else echo 'nosem: PASS'; fi"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

# cmake --build build --target check: проверка на семафоры, модульные, затем сценарные тесты
enable_testing()
add_test(NAME unit COMMAND unit_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME scenarios COMMAND run_tests $<TARGET_FILE:carwash-mon>
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_target(check
    COMMAND unit_tests
    COMMAND run_tests $<TARGET_FILE:carwash-mon>
    DEPENDS nosem unit_tests run_tests carwash-mon
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)

# clang-format и clang-tidy. В Homebrew llvm не попадает в PATH, поэтому смотрим и туда
set(LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(CLANG_FORMAT clang-format HINTS ${LLVM_HINTS})
find_program(CLANG_TIDY clang-tidy HINTS ${LLVM_HINTS})
set(TIDY_SOURCES ${CORE_SOURCES} src/main.c)
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
