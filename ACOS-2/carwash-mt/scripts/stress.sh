#!/usr/bin/env bash
# Стресс: сотни прогонов (сценарий x seed) в обычном, конкурентном режиме.
# Использование: scripts/stress.sh [бинарник [число прогонов]]   (по умолчанию bin/carwash-mt, 500)
# Провал: код выхода не тот, что ждём от сценария; в выводе AUDITOR, soft assert, LOCKDEP или
# ThreadSanitizer; прогон завис дольше 10 с. Запускать из корня проекта.
set -u
BIN=${1:-bin/carwash-mt}
TOTAL=${2:-500}
LIMIT=10
# Итоговая строка аудитора («AUDITOR: invariants checked ... violations: 0») есть в каждом
# прогоне, поэтому ищем само нарушение, а не слово AUDITOR
BAD='AUDITOR: \[|violations: [1-9]|violated|soft assert|LOCKDEP|ThreadSanitizer'
JOBS=${STRESS_JOBS:-8}

[ -x "$BIN" ] || { echo "stress: no $BIN, build the project first" >&2; exit 2; }
command -v perl >/dev/null || { echo "stress: perl is needed for the watchdog" >&2; exit 2; }

# сценарий:ожидаемый код выхода
SCENARIOS=(normal:0 rush-hour:0 breakdowns:0 capacity:0 priority:0 tiny:0 zero-buffer:0
           dead-stage:2 dead-program:2)
NS=${#SCENARIOS[@]}
TMP=$(mktemp -d /tmp/carwash-stress.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# Один прогон i: пишет в $TMP/i.res одну строку - OK или причину провала
run_one() {
    local i=$1 sc=${SCENARIOS[$(($1 % NS))]}
    local cfg=${sc%%:*} want=${sc##*:} seed=$(($1 / NS + 1))
    local out=$TMP/$i.out code
    perl -e 'alarm shift; exec @ARGV' "$LIMIT" "$BIN" -m log -d 0 -s "$seed" \
        -c "data/$cfg.cfg" -l "$TMP/$i.log" >"$out" 2>&1
    code=$?
    local why=""
    if [ "$code" -eq 142 ]; then
        why="hang (> ${LIMIT} s)"
    elif [ "$code" -ne "$want" ]; then
        why="exit code $code, expected $want"
    elif grep -qE "$BAD" "$out"; then
        why="violation in output: $(grep -E -m1 "$BAD" "$out" | head -c 80)"
    fi
    if [ -n "$why" ]; then
        echo "FAIL $cfg -s $seed: $why" >"$TMP/$i.res"
    else
        echo OK >"$TMP/$i.res"
        rm -f "$out" "$TMP/$i.log"
    fi
}

START=$(date +%s)
for ((i = 0; i < TOTAL; i++)); do
    run_one "$i" &
    # не больше JOBS прогонов одновременно
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.05; done
done
wait

FAILED=$(cat "$TMP"/*.res | grep -c '^FAIL' || true)
echo "stress: $TOTAL runs of $BIN over $NS scenarios, $((TOTAL - FAILED)) ok, $FAILED failed," \
     "$(($(date +%s) - START)) s"
if [ "$FAILED" -ne 0 ]; then
    cat "$TMP"/*.res | grep '^FAIL' | head -10
    exit 1
fi
