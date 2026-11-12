/**
 * @file util.h
 * @brief Время, сигналы и вывод событий - общее для сервера и клиентов.
 *
 * Вывод идёт только через write(2) по дескрипторам: строка события собирается целиком и
 * пишется одним вызовом, поэтому строки разных процессов не перемешиваются посимвольно.
 */
#ifndef UTIL_H
#define UTIL_H

#include <signal.h>
#include <stddef.h>

/// Флаги, которые выставляют обработчики сигналов; проверяются в циклах ожидания.
extern volatile sig_atomic_t gotInterrupt;  ///< SIGINT или SIGTERM
extern volatile sig_atomic_t gotStatus;     ///< SIGUSR1: показать состояние линии
extern volatile sig_atomic_t gotClose;      ///< SIGUSR2: закрыть въезд досрочно

/**
 * @brief Ставит обработчики SIGINT, SIGTERM, SIGUSR1, SIGUSR2 и игнорирует SIGPIPE.
 *
 * Обработчики ставятся без SA_RESTART: блокирующие poll, read и nanosleep прерываются
 * с EINTR, и процесс сразу видит флаг. SIGPIPE игнорируется: запись в закрытый сокет
 * возвращает EPIPE, и отвалившийся клиент просто отключается.
 */
void SignalsInstall(void);

/// Монотонное время в наносекундах; CLOCK_MONOTONIC общий для всех процессов системы.
long long NowNs(void);
/// Минута модели, прошедшая с момента @p startNs при @p delayMs мс на минуту.
int ModelMinute(long long startNs, int delayMs);
/// "08:15" (мойка открывается в 08:00) или "d2 08:15" для второго дня.
void FormatClock(int minute, char *buf, int size);

/// Спит @p ms миллисекунд; возвращает раньше, если пришёл сигнал (тогда -1).
int SleepMs(int ms);

/// Пишет буфер целиком, повторяя write после частичной записи и EINTR. @return 0 или -1
int WriteAll(int fd, const void *buf, size_t n);
/// Читает ровно @p n байт. @return n, 0 при конце файла, -1 при ошибке (errno сохранён)
long ReadAll(int fd, void *buf, size_t n);

/// Цвета строк событий в терминале.
#define C_NONE  ""
#define C_CAR   "\033[36m"
#define C_POST  "\033[0m"
#define C_WAIT  "\033[33m"
#define C_FAIL  "\033[1;31m"
#define C_FIX   "\033[1;32m"
#define C_DONE  "\033[32m"
#define C_COORD "\033[1;35m"

/**
 * @brief Куда писать события этого процесса.
 * @param journalFd дескриптор журнала (O_APPEND) или -1
 * @param quiet     1 - на экран события не выводить
 */
void OutInit(int journalFd, int quiet);
/// Дескриптор журнала, заданный в OutInit.
int OutJournal(void);
/**
 * @brief Строка события: "[08:15] tunnel.1     4242 | car #3 enters tunnel post 1".
 * @param minute минута модели
 * @param who    кто сообщает (сущность модели)
 * @param pid    процесс этой сущности
 */
void OutEvent(int minute, const char *who, int pid, const char *color, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
/// Строка итогов - и на экран (даже при -q), и в журнал.
void OutLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Кому ещё отдавать каждую строку события и итогов.
 *
 * Сервер через этот обработчик рассылает ленту клиентам-наблюдателям. @p line - без перевода
 * строки, @p color - цвет события (C_*) или "" для строк итогов.
 */
typedef void (*OutHook)(const char *line, const char *color);
void OutSetHook(OutHook hook);

#endif
