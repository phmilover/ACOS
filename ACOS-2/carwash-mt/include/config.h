/**
 * @file config.h
 * @brief Параметры модели и их разбор.
 *
 * Параметры читаются из файла вида "ключ = значение" (ключ -c),
 * а если файла нет - из встроенной конфигурации CONFIG_BUILTIN.
 */
#ifndef CONFIG_H
#define CONFIG_H

#define MAX_STAGES   6
#define MAX_POSTS    4
#define MAX_CAPACITY 4  ///< сколько машин одновременно может быть на одном посту
#define MAX_PROGRAMS 6
#define MAX_QUEUE    16  ///< размер массива; вместимость из конфига строго меньше
#define NAME_LEN     12

/// Кого первым забирать из очереди.
typedef enum {
    PRIO_FIFO,      ///< кто раньше приехал
    PRIO_PROGRAM,   ///< у чьей программы приоритет выше
    PRIO_SHORTEST,  ///< кому осталось меньше мыться
} PriorityRule;

typedef enum {
    VIEW_AUTO,  ///< live в терминале, log если вывод перенаправлен
    VIEW_LOG,
    VIEW_LIVE,
} ViewMode;

/// Стадия мойки: строка "stage = имя посты tmin tmax очередь [ёмкость]".
typedef struct {
    char name[NAME_LEN];
    int posts;       ///< 0 постов допустимо - так проверяется тупик
    int tmin, tmax;  ///< время операции, минут
    int queue;       ///< вместимость накопителя перед стадией
    int capacity;  ///< сколько машин одновременно на одном посту, 1..MAX_CAPACITY (по умолчанию 1)
} StageSpec;

/// Вид намеренной ошибки для демонстрации средств проверки (ключ -x).
typedef enum { SAB_NONE, SAB_OVERFLOW, SAB_RACE, SAB_DEADLOCK } Sabotage;

/// Программа обслуживания: маршрут по стадиям.
typedef struct {
    char name[NAME_LEN];
    int weight;  ///< как часто клиенты выбирают программу
    int priority;
    int route[MAX_STAGES];
    int len;
} ProgramSpec;

typedef struct {
    StageSpec stage[MAX_STAGES];
    int stages;
    ProgramSpec program[MAX_PROGRAMS];
    int programs;

    int cars;  ///< 0 - без ограничения
    int arrMin, arrMax;
    int day;  ///< длина дня в минутах, 0 - без ограничения
    double failP;
    int repairMin, repairMax;
    PriorityRule rule;

    unsigned seed;
    int seedSet;
    int delayMs;  ///< -1 - выбрать по режиму
    ViewMode view;
    char journal[256];
    Sabotage sabotage;  ///< ключ -x
    int deterministic;  ///< ключ -D: детерминированный порядок потоков
    int threadColumn;   ///< ключ -t: колонка потока в ленте
    char dotFile[256];  ///< ключ -g: куда записать граф ожидания при тупике (пусто - не писать)
} Config;

/// Конфигурация по умолчанию: обычный день, четыре стадии, три программы.
extern const char *CONFIG_BUILTIN;

void ConfigDefaults(Config *c);

/// @return 0 или -1, причина ошибки - в @p err
int ConfigParseText(Config *c, const char *text, char *err, int errSize);
/// @return 0 или -1, причина ошибки - в @p err
int ConfigLoadFile(Config *c, const char *path, char *err, int errSize);
/// Проверяет, что параметры не противоречат друг другу. @return 0 или -1
int ConfigValidate(const Config *c, char *err, int errSize);

/// Ключ -i: спрашивает основные параметры с клавиатуры.
void ConfigInteractive(Config *c);
void ConfigPrint(const Config *c, int fd);
const char *ConfigRuleName(PriorityRule rule);

/// Разбирает имя вида саботажа ("overflow", "race", "deadlock"). @return 0 и вид в *out, или -1
int ConfigParseSabotage(const char *s, Sabotage *out);

/// @return 0 и число в *out, или -1, если строка - не целое число
int ParseInt(const char *s, int *out);

#endif
