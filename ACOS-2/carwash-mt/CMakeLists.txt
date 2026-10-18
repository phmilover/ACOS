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

# Doxygen: cmake --build build --target docs, открыть build/doxygen/html/index.html.
# Graphviz (dot) ищем здесь: build/Doxyfile.local подключает Doxyfile и включает HAVE_DOT
# (графы вызовов и схемы @dot на странице «Потоки и синхронизация»); без dot схем нет
find_package(Doxygen QUIET)
find_program(DOT_EXECUTABLE dot HINTS /opt/homebrew/bin /usr/local/bin)
if(DOT_EXECUTABLE)
    get_filename_component(DOT_DIR ${DOT_EXECUTABLE} DIRECTORY)
    file(WRITE ${CMAKE_BINARY_DIR}/Doxyfile.local
        "@INCLUDE = Doxyfile\nHAVE_DOT = YES\nDOT_PATH = ${DOT_DIR}\n")
else()
    file(WRITE ${CMAKE_BINARY_DIR}/Doxyfile.local "@INCLUDE = Doxyfile\nHAVE_DOT = NO\n")
endif()
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile.local
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "doxygen not found: brew install doxygen (or apt install doxygen)"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()

# ---- Проверки и инструменты (определяются только в основной сборке) ------------------------
if(NOT CW_NESTED)
    set(NEST -DCW_NESTED=ON)
    set(RUN_TESTS_BIN bin/run_tests)

    # Сборка Debug, не RelWithDebInfo: тот определяет NDEBUG и отключает SOFT_ASSERT, а тесты
    # проверяют его срабатывания.
    # Сборка Debug, не RelWithDebInfo: тот определяет NDEBUG и отключает SOFT_ASSERT, а тесты
    # проверяют его срабатывания.
    # cmake --build build --target tsan: сборка build-tsan и прогон тестов под ThreadSanitizer.
    # halt_on_error=1 - первая же гонка валит прогон; тест саботажа race ждёт отчёта TSan
    # (он запускается с этой же настройкой: отчёт напечатан до остановки)
    add_custom_target(tsan
        COMMAND ${CMAKE_COMMAND} -S . -B build-tsan -DCW_SANITIZE=thread
                -DCMAKE_BUILD_TYPE=Debug ${NEST}
        COMMAND ${CMAKE_COMMAND} --build build-tsan
        COMMAND ${CMAKE_COMMAND} -E env TSAN_OPTIONS=halt_on_error=1 bin/unit_tests-tsan
        COMMAND ${CMAKE_COMMAND} -E env TSAN_OPTIONS=halt_on_error=1
                ${RUN_TESTS_BIN} bin/carwash-mt-tsan
        DEPENDS run_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)

    # asan: AddressSanitizer + UBSan. LeakSanitizer на macOS не поддержан, там detect_leaks=0
    if(APPLE)
        set(ASAN_LEAKS 0)
    else()
        set(ASAN_LEAKS 1)
    endif()
    add_custom_target(asan
        COMMAND ${CMAKE_COMMAND} -S . -B build-asan -DCW_SANITIZE=address
                -DCMAKE_BUILD_TYPE=Debug ${NEST}
        COMMAND ${CMAKE_COMMAND} --build build-asan
        COMMAND ${CMAKE_COMMAND} -E env ASAN_OPTIONS=detect_leaks=${ASAN_LEAKS}:halt_on_error=1
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 bin/unit_tests-asan
        COMMAND ${CMAKE_COMMAND} -E env ASAN_OPTIONS=detect_leaks=${ASAN_LEAKS}:halt_on_error=1
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
                ${RUN_TESTS_BIN} bin/carwash-mt-asan
        DEPENDS run_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)

    # tsa: Thread Safety Analysis настоящим clang (Homebrew LLVM). Apple clang 21 не видит
    # MEMBER_GUARDED_BY у полей структур (tsa.h), поэтому поля проверяет только этот clang
    find_program(BREW_CLANG clang HINTS ${LLVM_HINTS} NO_DEFAULT_PATH)
    if(BREW_CLANG)
        add_custom_target(tsa
            COMMAND ${CMAKE_COMMAND} -S . -B build-tsa -DCMAKE_C_COMPILER=${BREW_CLANG}
                    -DCW_SUFFIX=-tsa ${NEST}
                    "-DCMAKE_C_FLAGS=-Wthread-safety -Wthread-safety-beta -Werror"
            COMMAND ${CMAKE_COMMAND} --build build-tsa
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)
    else()
        add_custom_target(tsa
            COMMAND ${CMAKE_COMMAND} -E echo "Homebrew clang not found: brew install llvm"
            COMMAND ${CMAKE_COMMAND} -E false)
    endif()

    # stress: сотни прогонов в обычном режиме (scripts/stress.sh [бинарник [число]])
    add_custom_target(stress
        COMMAND scripts/stress.sh $<TARGET_FILE:carwash-mt> 500
        DEPENDS carwash-mt
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)

    # cppcheck
    find_program(CPPCHECK cppcheck)
    if(CPPCHECK)
        add_custom_target(cppcheck
            COMMAND ${CPPCHECK} --enable=warning,performance,portability --std=c11
                    --error-exitcode=1 --suppress=missingIncludeSystem --quiet
                    "-D__has_feature(x)=0"
                    -I include src tests
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)
    else()
        add_custom_target(cppcheck
            COMMAND ${CMAKE_COMMAND} -E echo "cppcheck not found: brew install cppcheck (or apt install cppcheck)"
            COMMAND ${CMAKE_COMMAND} -E false)
    endif()

    # coverage: build-cov с инструментацией, check, отчёт llvm-cov (HTML и таблица в консоль).
    # Инструменты берём в тон компилятору: Apple clang - xcrun, иначе llvm-* рядом с ним
    if(APPLE)
        execute_process(COMMAND xcrun -f llvm-cov RESULT_VARIABLE XC1 OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND xcrun -f llvm-profdata RESULT_VARIABLE XC2 OUTPUT_QUIET ERROR_QUIET)
        if(XC1 EQUAL 0 AND XC2 EQUAL 0)
            set(COV_PROFDATA xcrun llvm-profdata)
            set(COV_COV xcrun llvm-cov)
        endif()
    else()
        find_program(LLVM_PROFDATA llvm-profdata)
        find_program(LLVM_COV llvm-cov)
        set(COV_PROFDATA ${LLVM_PROFDATA})
        set(COV_COV ${LLVM_COV})
    endif()
    string(REPLACE ";" " " COV_PROFDATA_CMD "${COV_PROFDATA}")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND COV_PROFDATA AND COV_COV)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E remove_directory build-cov/prof
            COMMAND ${CMAKE_COMMAND} -S . -B build-cov -DCW_COVERAGE=ON -DCW_SUFFIX=-cov
                    -DCMAKE_BUILD_TYPE=Debug ${NEST}
            COMMAND ${CMAKE_COMMAND} -E env LLVM_PROFILE_FILE=build-cov/prof/%p.profraw
                    ${CMAKE_COMMAND} --build build-cov --target check
            COMMAND sh -c "${COV_PROFDATA_CMD} merge -sparse build-cov/prof/*.profraw -o build-cov/all.profdata"
            COMMAND ${COV_COV} show -format=html -output-dir=build-cov/html
                    -instr-profile=build-cov/all.profdata bin/carwash-mt-cov
                    -object bin/unit_tests-cov src
            COMMAND ${COV_COV} report -instr-profile=build-cov/all.profdata bin/carwash-mt-cov
                    -object bin/unit_tests-cov src
            COMMAND ${CMAKE_COMMAND} -E echo "HTML report: build-cov/html/index.html"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL)
    else()
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E echo "coverage needs clang with llvm-cov and llvm-profdata (xcrun on macOS)"
            COMMAND ${CMAKE_COMMAND} -E false)
    endif()
endif()
