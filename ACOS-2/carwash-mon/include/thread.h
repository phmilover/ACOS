/**
 * @file thread.h
 * @brief Создание потоков с заданным стеком и тестовым крючком отказа.
 */
#ifndef THREAD_H
#define THREAD_H

#include <pthread.h>
#include <stddef.h>

/// Стек потока машины: потоков много, а работы на стеке мало.
#define CAR_STACK_SIZE ((size_t)256 * 1024)

/// Создаёт поток. Все потоки программы создаются только так.
/// Крючок для тестов: при CARWASH_FAIL_THREAD_AFTER=N вызов с номером N (с нуля) и все
/// последующие возвращают EAGAIN; переменная читается при каждом вызове.
/// @param t куда записать поток @param fn функция потока @param arg её аргумент
/// @param stack размер стека, 0 - по умолчанию
/// @return 0 или код ошибки pthread_create
int ThreadCreate(pthread_t *t, void *(*fn)(void *), void *arg, size_t stack);

#endif
