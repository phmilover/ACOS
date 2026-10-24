/**
 * @file signals.h
 * @brief Поток сигналов: sigwait вместо обработчиков.
 *
 * Сигналы SIGINT, SIGTERM, SIGUSR1, SIGUSR2 блокируются во всех потоках (маску наследуют от
 * главного), и их забирает один поток через sigwait. Он ничего не меняет в модели сам: ставит
 * бит запроса в Line.requests и будит часы (SimKick сигналит их условную переменную quiet), а
 * разбирает запрос крючок часов в момент покоя (quiesce.h):
 * - SIGINT, SIGTERM - REQ_INTERRUPT: прервать (код 3, итоги и журнал сохраняются);
 * - SIGUSR1 - REQ_SNAPSHOT: граф ожидания и таблица потоков в stderr, работа продолжается;
 * - SIGUSR2 - REQ_CLOSE: закрыть въезд досрочно, домыть принятых и выйти с кодом 0.
 */
#ifndef SIGNALS_H
#define SIGNALS_H

#include "line.h"

/// Блокирует SIGINT, SIGTERM, SIGUSR1, SIGUSR2 в текущем потоке. Звать в main до создания
/// любых других потоков: они унаследуют маску, и сигнал достанется только потоку sigwait.
void SignalsBlock(void);

/// Запускает поток sigwait. Звать после LineInit (он обнуляет Line.requests).
/// @return 0 или -1 (поток не создался)
int SignalsStart(Line *l);

/// Останавливает поток сигналов: флаг выхода, pthread_kill(SIGUSR2), join. Без запущенного
/// потока ничего не делает.
void SignalsStop(void);

#endif
