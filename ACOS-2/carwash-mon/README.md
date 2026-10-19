# Автомойка MON (вариант 43)

Многопоточная (pthreads) имитация автоматической автомойки на C17: libc, системные вызовы и
pthread. Это вторая программа второго индивидуального задания: та же модель, что в
`ACOS-2/carwash-mt`, но потоки взаимодействуют без единого семафора - только мониторами
(мьютекс и условные переменные), `pthread_rwlock`, барьером старта и атомарными операциями C11.

Проверено только на macOS (Apple clang 21). Код для Linux предусмотрен (`_DEFAULT_SOURCE`),
но там не собирался и не запускался; GCC не пробовали.

Состояние: каркас. Готовы синхропримитивы (`sync.h`: Mutex, Cond, RwLock с проверкой порядка
захвата LOCKDEP), барьер (`barrier.h`), создание потоков (`thread.h`).

## Сборка

Все команды - из папки `carwash-mon`.

```bash
cmake -S . -B build && cmake --build build
./bin/unit_tests                       # модульные тесты
cmake --build build --target check     # проверка на семафоры (nosem) и модульные тесты
cmake --build build --target format    # clang-format
cmake --build build --target tidy      # clang-tidy
```

Цель `nosem` ищет в `src`, `include` и `tests` следы семафоров (`sem_`, `semaphore.h`,
`dispatch_semaphore`, `Sem...`) и падает, если находит.
