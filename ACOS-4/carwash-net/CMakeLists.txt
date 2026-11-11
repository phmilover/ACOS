cmake_minimum_required(VERSION 3.16)
project(carwash_net C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # нужен clang-tidy

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# -DCW_SANITIZE=ON: ASan + UBSan, бинарники в bin-asan/ (тесты: tests/run_tests.sh bin-asan)
option(CW_SANITIZE "ASan и UBSan" OFF)
if(CW_SANITIZE)
    set(BIN_DIR "${CMAKE_SOURCE_DIR}/bin-asan")
else()
    set(BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
endif()
# бинарники кладём в bin/, чтобы скрипты и тесты не искали их внутри build/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}")

file(GLOB COMMON CONFIGURE_DEPENDS src/common/*.c)
file(GLOB HEADERS CONFIGURE_DEPENDS include/*.h)
set(PROGRAMS server post cars view)

add_library(common STATIC ${COMMON})
target_include_directories(common PUBLIC include)

foreach(p ${PROGRAMS})
    add_executable(cw-${p} src/${p}.c)
    target_link_libraries(cw-${p} PRIVATE common)
endforeach()

foreach(target common ${PROGRAMS})
    if(NOT target STREQUAL "common")
        set(target cw-${target})
    endif()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
    endif()
    if(CW_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

# cmake --build build --target check (или ctest --test-dir build)
enable_testing()
add_test(NAME scenarios COMMAND tests/run_tests.sh ${BIN_DIR} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set(ALL_BINS)
foreach(p ${PROGRAMS})
    list(APPEND ALL_BINS cw-${p})
endforeach()
add_custom_target(check
    COMMAND tests/run_tests.sh ${BIN_DIR}
    DEPENDS ${ALL_BINS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

# те же тесты под ASan и UBSan в отдельной сборке build-asan
if(NOT CW_SANITIZE)
    add_custom_target(asan
        COMMAND ${CMAKE_COMMAND} -S . -B build-asan -DCW_SANITIZE=ON
        COMMAND ${CMAKE_COMMAND} --build build-asan
        COMMAND tests/run_tests.sh bin-asan
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL)
endif()

# clang-format и clang-tidy. В Homebrew llvm не попадает в PATH, поэтому смотрим и туда
set(LLVM_HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(CLANG_FORMAT clang-format HINTS ${LLVM_HINTS})
find_program(CLANG_TIDY clang-tidy HINTS ${LLVM_HINTS})
set(SOURCES ${COMMON})
foreach(p ${PROGRAMS})
    list(APPEND SOURCES ${CMAKE_SOURCE_DIR}/src/${p}.c)
endforeach()

if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${SOURCES} ${HEADERS}
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
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --quiet ${TIDY_EXTRA} ${SOURCES}
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
