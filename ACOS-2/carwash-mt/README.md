# Автомойка MT (вариант 43)

Многопоточная (pthreads) имитация автоматической автомойки на C17: libc, системные вызовы и pthread.

## Сборка

Все команды - из папки `carwash-mt`.

```bash
cmake -S . -B build && cmake --build build
./bin/unit_tests                       # модульные тесты ядра
cmake --build build --target check     # unit_tests, затем сценарные тесты
cmake --build build --target format    # clang-format
cmake --build build --target tidy      # clang-tidy
cmake --build build --target docs      # Doxygen
```

Санитайзеры: `cmake -S . -B build-tsan -DCW_SANITIZE=thread` (бинарники с суффиксом `-tsan`).
