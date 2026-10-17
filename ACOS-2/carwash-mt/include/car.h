/**
 * @file car.h
 * @brief Поток машины: выбор программы, въезд или отказ, проезд по маршруту.
 *
 * CarNextStage / CarRemainingWork объявлены в line.h.
 */
#ifndef CAR_H
#define CAR_H

#include "line.h"

/**
 * @brief Создаёт машину и регистрирует её поток в часах (без запуска).
 *
 * Под registry: номер = carCount, поток случайных чисел 1000 + номер, имя потока "car #N"
 * (с единицы). Машиной владеет линия (LineFree освобождает).
 * @param now минута прибытия
 * @return машина или NULL, если не хватило памяти или не открылся семафор (код выхода 5)
 */
Car *CarCreate(Line *l, int now);

/// Тело потока машины. @param arg Car * из CarCreate (до запуска - SimSpawned)
void *CarMain(void *arg);

#endif
