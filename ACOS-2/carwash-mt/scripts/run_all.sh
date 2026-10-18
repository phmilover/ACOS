#!/usr/bin/env bash
# Весь набор проверок подряд: сборка, тесты, санитайзеры, статический анализ, стресс.
# Запуск из любой папки: scripts/run_all.sh. Вывод каждого шага - в build/run_all/<шаг>.log.
# Итог - таблица PASS / FAIL / SKIP (SKIP - только если нет инструмента, причина в таблице);
# код выхода 0, если нет ни одного FAIL.
set -u
cd "$(dirname "$0")/.." || exit 2
LOGS=build/run_all
mkdir -p "$LOGS"

NAMES=()
RESULTS=()
NOTES=()
FAILED=0

# step ИМЯ КОМАНДА...: выполняет команду, вывод - в лог
step() {
    local name=$1
    shift
    printf '%-11s ' "$name"
    local t0=$SECONDS
    if "$@" >"$LOGS/$name.log" 2>&1; then
        echo "PASS ($((SECONDS - t0)) s)"
        record "$name" PASS "$((SECONDS - t0)) s"
    else
        echo "FAIL ($((SECONDS - t0)) s), see $LOGS/$name.log"
        record "$name" FAIL "see $LOGS/$name.log"
        FAILED=1
    fi
}

skip() {
    printf '%-11s SKIP (%s)\n' "$1" "$2"
    record "$1" SKIP "$2"
}

record() {
    NAMES+=("$1")
    RESULTS+=("$2")
    NOTES+=("$3")
}

have() { command -v "$1" >/dev/null 2>&1; }
BREW_LLVM=/opt/homebrew/opt/llvm/bin
tgt() { cmake --build build --target "$1"; }

# format не должен менять файлы: сравниваем отпечаток дерева до и после
format_clean() {
    local before after
    before=$(git diff --stat -- . | shasum; git ls-files -o --exclude-standard . | shasum)
    tgt format || return 1
    after=$(git diff --stat -- . | shasum; git ls-files -o --exclude-standard . | shasum)
    [ "$before" = "$after" ]
}

step build bash -c 'cmake -S . -B build && cmake --build build'
step check tgt check
step tsan tgt tsan
step asan tgt asan

if [ -x "$BREW_LLVM/clang" ]; then
    step tsa tgt tsa
else
    skip tsa "Homebrew clang not found: brew install llvm"
fi

if [ -x "$BREW_LLVM/clang-format" ] || have clang-format; then
    step format format_clean
else
    skip format "clang-format not found: brew install llvm"
fi

if [ -x "$BREW_LLVM/clang-tidy" ] || have clang-tidy; then
    step tidy tgt tidy
else
    skip tidy "clang-tidy not found: brew install llvm"
fi

if have cppcheck; then
    step cppcheck tgt cppcheck
else
    skip cppcheck "cppcheck not found: brew install cppcheck"
fi

step stress scripts/stress.sh bin/carwash-mt 200
# сборка -tsan уже есть после шага tsan; без неё шаг честно падает
step stress-tsan scripts/stress.sh bin/carwash-mt-tsan 100

if have xcrun && ! xcrun -f llvm-cov >/dev/null 2>&1; then
    skip coverage "xcrun llvm-cov not available"
elif have xcrun || have llvm-cov; then
    step coverage tgt coverage
else
    skip coverage "llvm-cov not found"
fi

if have doxygen; then
    # pipefail: упавший Doxygen - FAIL, даже если слова warning в выводе нет
    step docs bash -c 'set -o pipefail; cmake --build build --target docs 2>&1 | tee /dev/stderr | { ! grep -i "warning"; }'
else
    skip docs "doxygen not found: brew install doxygen"
fi

echo
echo "================ SUMMARY ================"
printf '%-11s %-6s %s\n' STEP RESULT NOTE
for i in "${!NAMES[@]}"; do
    printf '%-11s %-6s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}" "${NOTES[$i]}"
done
[ "$FAILED" -eq 0 ] && echo "ALL OK" || echo "THERE ARE FAILURES"
exit "$FAILED"
