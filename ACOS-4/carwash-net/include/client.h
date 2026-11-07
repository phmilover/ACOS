/**
 * @file client.h
 * @brief Общее для клиентов: подключение к серверу, регистрация, часы модели, приём строк.
 *
 * Часы модели клиент ведёт сам: сервер присылает текущую минуту в WELCOME и в OPEN (въезд
 * открыт). Дальше клиент считает минуты по своим часам (-d мс на минуту у всех одинаково,
 * берётся из WELCOME), поэтому в ленте клиента то же время, что и у сервера.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include "net.h"

typedef struct {
    LineConn io;
    const char *name;  ///< имя программы для сообщений об ошибках
    int delayMs;       ///< миллисекунд на минуту модели
    long long baseNs;  ///< момент, когда была минута baseMinute
    int baseMinute;
} Session;

/**
 * @brief Подключается к @p host:@p port, отправляет @p hello и ждёт WELCOME.
 *
 * Сервер мог ещё не запуститься: подключение повторяется до @p seconds секунд.
 * @param[out] welcome строка WELCOME целиком
 * @return 0 или -1 (причина уже в stderr: нет сервера, отказ REJECT)
 */
int SessionOpen(Session *s, const char *name, const char *host, int port, int seconds,
                const char *hello, char *welcome, int size);

/// Минута модели по часам клиента.
int SessionNow(const Session *s);
/// Часы: сейчас минута @p minute (WELCOME, OPEN).
void SessionSync(Session *s, int minute);

/**
 * @brief Следующая строка от сервера не дольше @p timeoutMs (-1 - без ограничения).
 * @return 1 - строка, 0 - время вышло, -1 - сервер пропал, -3 - Ctrl+C пользователя
 */
int SessionRecv(Session *s, char *line, int size, int timeoutMs);

/// Вежливо отключается: BYE и закрытие сокета.
void SessionBye(Session *s);

/**
 * @brief Печатает причину остановки из строки "STOP <код> <текст>" и возвращает 0.
 *
 * Клиент, остановленный сервером, завершается штатно, с кодом 0.
 */
int SessionStopped(const Session *s, const char *line);

#endif
