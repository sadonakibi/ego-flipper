#pragma once
/* Чистая логика EGO. Без зависимостей от Flipper SDK: собирается и тестируется на ПК.
 *
 * Шкала EGO 0..11000. Круг = 1000 очков, кругов 11. Внутри круга ранги E D C B A S;
 * собранный круг даёт ранг S степень (S I ... S XI), дальше ранги идут по кругу ещё раз.
 * «Основание» - закреплённая нижняя планка.
 *
 *   Задания  - ежедневный список (16) + бонусы (3 в день). Каждое раз в сутки.
 *   Нагрузка - повторы и минуты растут на шаг за каждые 2 выполненных дня (и можно взять шаг сразу).
 *   Минимум  - еда x3, спорт, время себе, медитация, дыхание, сон. Не закрыт - серьёзный штраф.
 *   Уход     - модуль «Контроль» (влево): удержание ОК, таймер «вне EGO» и сжигание.
 *   Возврат  - удержание ОК: сжигание стоит, часть сгоревшего возвращается.
 *   Причины  - после возврата: «анализ» и «выход из EGO», каждая -1 (налог).
 *   Привычки - тот же модуль «Контроль»: заметил +1, удержался +3, срыв небольшой штраф.
 *   Сон      - лёг / встал, часы за 24 часа складываются из кусков (ночи с ребёнком рваные).
 *   Копилка  - цель 200к, шаг 1..5к, первое пополнение дня даёт очки.
 *
 * Штрафы растут вместе с EGO: множитель = (1 + 0.25 за круг) * (0.5 .. 1.5 по рангу).
 * Награды остаются прежними. Сложность заданий (код) растёт по кругам.
 */

#include <stdint.h>
#include <stdbool.h>

#define EGO_CIRCLE 1000 // очков в круге
#define EGO_CIRCLES 11
#define EGO_MAX (EGO_CIRCLE * EGO_CIRCLES)
#define EGO_START 300
#define EGO_START_FLOOR 150
#define EGO_CRITICAL_BELOW 300 // ниже этого баннер «EGO КРИТИЧЕСКИ ОСЛАБЕЛО»

#define EGO_BANDS 6 // E D C B A S
#define EGO_BAND_E 0
#define EGO_BAND_S 5

#define EGO_MAX_CATCHUP_DAYS 30
#define EGO_DAY_START_HOUR 5 // границы суток для штрафов и колоды; окна дня считаются от подъёма

/* базовые штрафы (умножаются на ego_penalty_mult) */
#define EGO_MIN_PENALTY 8 // за каждый незакрытый пункт минимума дня (вдвое строже прежнего окна)
#define EGO_CAUSE_COST 1 // за каждую выбранную причину ухода
#define EGO_CHECK_FAIL 1 // вылет в проверке
#define EGO_POSTPONE_HALF 1 // плата за два переноса (0.5 + 0.5)
#define EGO_HABIT_SLIP 5 // срыв: небольшой штраф
#define EGO_BURN_STEP_SEC 300 // шаг сжигания: 5 минут
#define EGO_BURN_RATE 2 // очков за шаг
#define EGO_BURN_RATE_FAST 5 // очков за шаг после EGO_BURN_FAST_AFTER_SEC
#define EGO_BURN_FAST_AFTER_SEC 1800
#define EGO_BURN_CAP 25 // максимум сгоревшего за один эпизод

/* награды (не масштабируются) */
#define EGO_RETURN_BONUS 1
#define EGO_RETURN_FULL_SEC 300 // вернулся за 5 минут: возвращается всё сгоревшее
#define EGO_RETURN_HALF_SEC 900 // за 15 минут: половина; позже: четверть
#define EGO_CHECK_OK 2
#define EGO_HABIT_NOTICE_GAIN 1 // заметил тягу
#define EGO_HABIT_HOLD_GAIN 3 // удержался
#define EGO_HABIT_NOTICE_MAX 10 // в сутки
#define EGO_HABIT_HOLD_MAX 7
#define EGO_HABIT_GAP_SEC 600 // не чаще раза в 10 минут на каждое действие
#define EGO_HABIT_REDUCE_BONUS 2 // за каждый срыв меньше, чем вчера (максимум EGO_HABIT_REDUCE_MAX)
#define EGO_HABIT_REDUCE_MAX 10

#define EGO_FLOOR_RISE_MAX 8 // на сколько основание подтягивается за сутки (на каждый круг)
#define EGO_FLOOR_MIN_DONE 5 // сколько пунктов минимума нужно, чтобы основание подтянулось

/* задания: 14 ежедневных (ядро) + бонусы; пул = ядро + «дочь» + ротация */
#define EGO_CORE_QUESTS 16
#define EGO_DAUGHTER_QUEST 16 // «время с дочерью»: бонус-слот в дни с ней (каждые 3 дня)
#define EGO_BONUS_FIRST 17 // первый пункт ротации бонусов
#define EGO_BONUS_POOL 35 // сколько пунктов в ротации
#define EGO_POOL_COUNT (EGO_BONUS_FIRST + EGO_BONUS_POOL) // 38
#define EGO_BONUS_SLOTS 3
#define EGO_DECK_SIZE (EGO_CORE_QUESTS + EGO_BONUS_SLOTS) // 17
#define EGO_CARD_INTERVAL_SEC 3600 // интервал между карточками и дедлайн карточки
#define EGO_POSTPONE_MAX 3 // после стольких переносов ядро снимается с дня
#define EGO_LOAD_STEP_DAYS 2 // нагрузка растёт на шаг за столько выполненных дней
#define EGO_LOAD_NONE 0
#define EGO_LOAD_REPS 1 // повторы (отжимания, приседания)
#define EGO_LOAD_MINUTES 2 // минуты с фокус-таймером
#define EGO_DAUGHTER_CYCLE 3 // дни с дочерью чередуются с женой: раз в 3 дня

/* минимум дня: 8 пунктов, биты в min_done */
#define EGO_MIN_ITEMS 8
#define EGO_MIN_SLEEP 7 // номер бита «сон зафиксирован»

#define EGO_PILLARS 5 // направления: тело, дело, деньги, позиция, тишина
#define EGO_PILLAR_STEP 10 // выполнений на один уровень направления

/* проверки в дежурном режиме */
#define EGO_CHECK_COUNT 2
#define EGO_CHECK_FLOW 0 // потоковость: час без оценки своих действий
#define EGO_CHECK_EGO 1 // быть в EGO
#define EGO_CHECK_FLOW_SEC 3600
#define EGO_CHECK_EGO_SEC 4260 // 1 час 11 минут
#define EGO_CHECK_TIMEOUT_SEC 600 // без ответа за 10 минут = вылет

/* сон: куски за последние 24 часа складываются */
#define EGO_SLEEP_SEGMENTS 8
#define EGO_SLEEP_FIXED_MIN 180 // с этого сон считается зафиксированным (пункт минимума)
#define EGO_SLEEP_SEG_MAX_SEC (16 * 3600UL)
#define EGO_SLEEP_HIST 7

/* копилка */
#define EGO_SAVINGS_START 70 // тысяч сейчас
#define EGO_SAVINGS_GOAL 200 // цель, тысяч
#define EGO_SAVINGS_DAY_BONUS 5 // очки за первое пополнение дня
#define EGO_SAVINGS_STEP_MAX 5

#define EGO_CAUSE_COUNT 2 // «анализ», «выход из EGO»
#define EGO_HISTORY_DAYS 14 // сколько завершённых суток помнит график

typedef enum {
    EgoLevelCritical,
    EgoLevelWeak,
    EgoLevelStable,
    EgoLevelStrong,
} EgoLevel; // внешний вид Ока

typedef enum {
    EgoReactionNone,
    EgoReactionReset, // контур сжимается и возвращается
    EgoReactionGrowth,
    EgoReactionPenalty, // уход / штраф
    EgoReactionReturn, // сборка из пепла
} EgoReaction;

/* Результат нажатия в контроле привычек. */
typedef enum {
    EgoHabitOk,
    EgoHabitTooSoon, // раньше чем через 10 минут
    EgoHabitMax, // дневной максимум
} EgoHabitResult;

/* Задание из пула.
 * load_kind: нагрузка задания - повторы (отжимания) или минуты с фокус-таймером (код);
 * base_load - стартовая нагрузка, max_load - потолок, load_step - шаг роста;
 * per_circle - сколько минут добавляется за каждый собранный круг (только минуты).
 * min_bit >= 0: пункт входит в минимум дня (несколько заданий с одним битом закрывают его вместе);
 * pillar - направление героя. */
typedef struct {
    int8_t points;
    uint8_t window; // 0 утро, 1 день, 2 вечер, 3 в любое время (окна считаются от подъёма)
    uint8_t load_kind;
    uint8_t base_load;
    uint8_t max_load;
    uint8_t load_step;
    uint8_t per_circle;
    int8_t min_bit;
    uint8_t pillar;
    const char* label;
} EgoQuest;

/* Итог завершённых суток. */
typedef struct {
    uint16_t total;
    uint16_t gain;
    uint16_t loss;
} EgoDay;

/* Сохраняемое состояние. */
typedef struct {
    uint16_t ego; // 0..EGO_MAX
    uint16_t floor; // основание, <= ego
    uint16_t peak; // наибольшее достигнутое EGO (степень S = peak / 1000)
    uint32_t day; // номер суток (0 = ещё не инициализировано)
    uint32_t pause_day; // сутки, объявленные паузой (штрафа нет), 0 = нет
    uint32_t total_reports;
    uint8_t min_done; // биты минимума дня

    /* уход (сжигание) */
    uint32_t out_since; // время начала ухода, 0 = в EGO
    uint32_t burn_ts; // до какого момента сжигание уже применено
    uint16_t out_burned; // сгорело в этом эпизоде
    uint16_t out_mult; // множитель штрафов на момент ухода (x100)
    uint16_t returns; // всего возвратов
    uint32_t return_sec; // суммарное время ухода по всем возвратам
    uint16_t cause_counts[EGO_CAUSE_COUNT];

    /* колода дня */
    uint8_t deck[EGO_DECK_SIZE]; // номера заданий из пула
    uint32_t deck_done; // биты выполненных слотов
    uint32_t deck_drop; // биты слотов, снятых после переносов (бонусы - при пропуске)
    uint8_t deck_post[EGO_DECK_SIZE]; // сколько раз перенесён слот
    uint32_t deck_ready[EGO_DECK_SIZE]; // раньше этого времени карточку не выдавать
    uint8_t half_debt; // накопленные половинки платы за перенос
    uint16_t quest_count[EGO_POOL_COUNT]; // сколько раз выполнено за всё время
    uint8_t load[EGO_POOL_COUNT]; // текущая нагрузка: повторы или минуты
    uint8_t load_prog[EGO_POOL_COUNT]; // выполненных дней с прошлого роста
    uint8_t load_miss[EGO_POOL_COUNT]; // подряд пропущенных дней
    uint16_t pillar_count[EGO_PILLARS]; // выполнений по направлениям за всё время
    uint32_t daughter_anchor; // сутки, от которых раз в 3 дня идут дни с дочерью (0 = выключено)

    /* проверки: сколько раз «да» и сколько вылетов (включая без ответа) */
    uint16_t check_ok[EGO_CHECK_COUNT];
    uint16_t check_fail[EGO_CHECK_COUNT];

    /* контроль привычек */
    uint8_t habit_noticed; // сегодня
    uint8_t habit_held;
    uint8_t habit_slips;
    uint8_t habit_prev_slips; // срывов вчера
    uint32_t habit_notice_last;
    uint32_t habit_hold_last;
    uint16_t habit_total_noticed;
    uint16_t habit_total_held;
    uint16_t habit_total_slips;
    uint8_t slips_hist[EGO_SLEEP_HIST]; // срывов по суткам, старые -> новые
    uint16_t habit_bonus_notice; // бонус за сокращение при закрытии суток (приложение показывает и обнуляет)

    /* сон */
    uint32_t sleep_since; // когда лёг, 0 = бодрствует
    uint32_t seg_start[EGO_SLEEP_SEGMENTS];
    uint32_t seg_end[EGO_SLEEP_SEGMENTS];
    uint8_t seg_head;
    int16_t sleep_adj; // поправка итога в минутах (сбрасывается на новые сутки)
    uint8_t sleep_awarded; // очки за сон, уже выданные сегодня
    uint16_t sleep_last_total; // итог сна за 24 часа на момент последнего подъёма (минут)
    uint32_t wake_ts; // когда встал последний раз
    uint32_t wake_day;
    uint8_t sleep_hist[EGO_SLEEP_HIST]; // сон за сутки в единицах по 6 минут, старые -> новые

    /* копилка */
    uint16_t savings_k;
    uint32_t savings_day; // сутки, за которые уже выдан бонус

    /* динамика */
    uint16_t day_gain;
    uint16_t day_loss;
    uint16_t day_start_total;
    uint8_t hist_n;
    EgoDay hist[EGO_HISTORY_DAYS]; // старые -> новые
} EgoState;

void ego_state_init(EgoState* s);
uint16_t ego_total(const EgoState* s); // = s->ego

/* Ранги и круги. circle 1..11, band 0..5 (E D C B A S), rank_index = (circle-1)*6 + band. */
uint8_t ego_circle(uint16_t ego);
uint8_t ego_band(uint16_t ego);
uint8_t ego_rank_index(uint16_t ego);
uint16_t ego_circle_base(uint16_t ego); // EGO в начале текущего круга
uint16_t ego_next_boundary(uint16_t ego); // следующая граница ранга (EGO) или 0 на максимуме
EgoLevel ego_level(uint16_t ego); // внешний вид Ока
uint8_t ego_peak_degree(const EgoState* s); // собранных кругов 0..11 (степень S)

/* Множитель штрафов (x100) и масштабирование базового штрафа. */
uint16_t ego_penalty_mult(uint16_t ego);
int ego_scaled(uint16_t ego, int base);

/* unix-время (локальное, как в RTC) -> номер суток и часовое окно 0..2 */
uint32_t ego_clock(uint32_t timestamp, uint8_t* window);
/* Окно дня от подъёма: 0-6 часов утро, 6-12 день, дальше вечер. Нет подъёма сегодня - по часам. */
uint8_t ego_window(const EgoState* s, uint32_t now, uint32_t day, uint8_t clock_window);

/* Закрывает прошедшие сутки: штраф за незакрытый минимум, основание, повторы, привычки, сон,
 * новая колода. Возвращает, сколько пунктов минимума не закрыто (по всем закрытым суткам). */
uint8_t ego_update(EgoState* s, uint32_t day);

/* Применяет сжигание до момента now. Возвращает сгоревшее за этот вызов. */
uint16_t ego_burn(EgoState* s, uint32_t now);

bool ego_is_out(const EgoState* s);
uint32_t ego_out_seconds(const EgoState* s, uint32_t now);

/* Уход: запуск таймера. Без причин и без начального удара. Повторный вызов ничего не меняет. */
void ego_depart(EgoState* s, uint32_t now);

/* Возврат. Возвращает сколько очков вернулось (с бонусом) или -1, если вы и так в EGO. */
int ego_return(EgoState* s, uint32_t now);

/* Причина ухода после возврата (0 анализ, 1 выход из EGO): штраф с множителем. */
EgoReaction ego_cause(EgoState* s, uint8_t index);

/* Задания дня. slot - номер карточки в колоде (0..EGO_DECK_SIZE-1). */
const EgoQuest* ego_quest(uint8_t pool_index);
const EgoQuest* ego_slot_quest(const EgoState* s, uint8_t slot);
uint8_t ego_slot_reps(const EgoState* s, uint8_t slot); // 0, если нагрузка не в повторах
uint8_t ego_slot_minutes(const EgoState* s, uint8_t slot); // 0, если нет фокус-таймера
bool ego_load_raise(EgoState* s, uint8_t slot); // взять шаг нагрузки сразу (если не выполнено сегодня)
bool ego_slot_is_bonus(uint8_t slot);
EgoReaction ego_quest_done(EgoState* s, uint8_t slot);
uint8_t ego_quests_today(const EgoState* s); // выполнено ядра
uint8_t ego_bonus_today(const EgoState* s);
uint8_t ego_min_progress(const EgoState* s); // сколько пунктов минимума закрыто из EGO_MIN_ITEMS

/* Проверка (потоковость / быть в EGO): ok = +2, иначе вылет с множителем. */
EgoReaction ego_check(EgoState* s, uint8_t kind, bool ok);

/* Перенос ядра: первый бесплатный, дальше -0.5. Возвращает 0 бесплатно, 1 добавлена половинка,
 * 2 списано -1 (набралось две половинки), -1 если слот неверный. Бонус пропускается бесплатно. */
int ego_quest_postpone(EgoState* s, uint8_t slot, uint32_t now);
int ego_postpone_cost(const EgoState* s, uint8_t slot); // 0 бесплатно, 1 стоит 0.5

/* Следующая карточка для выдачи: слот или -1. Сначала ядро (окно, просроченные, «в любое»), затем бонусы. */
int ego_next_card(const EgoState* s, uint8_t window, uint32_t now);

/* Контроль привычек: заметил (+1), удержался (+3), срыв (штраф). Возвращает результат. */
EgoHabitResult ego_habit_notice(EgoState* s, uint32_t now);
EgoHabitResult ego_habit_hold(EgoState* s, uint32_t now);
int ego_habit_slip(EgoState* s); // списанное

/* Сон: лёг / встал. «Встал» складывает куски за 24 часа, выдаёт очки по часам (7+ -> 8, 6 -> 5, 5 -> 3)
 * и закрывает «Подъём». Возвращает true, если теперь бодрствуете. */
bool ego_sleeping(const EgoState* s);
bool ego_sleep_toggle(EgoState* s, uint32_t now, uint32_t day);
uint16_t ego_sleep_minutes(const EgoState* s, uint32_t now); // за последние 24 часа, с поправкой и текущим куском
void ego_sleep_adjust(EgoState* s, int delta_min, uint32_t now);

/* Копилка, тысяч. Возвращает выданные очки (первое пополнение за сутки). */
int ego_savings_add(EgoState* s, int delta_k, uint32_t day);

/* День с дочерью: включить / выключить сегодня (дальше раз в 3 дня). Возвращает, включено ли. */
bool ego_daughter_toggle(EgoState* s, uint32_t day);
bool ego_daughter_day(const EgoState* s, uint32_t day);

/* Изменение EGO с начала суток (может быть отрицательным). */
int ego_day_delta(const EgoState* s);

/* Прирост и слив (очки EGO) за последние days суток, включая текущие. */
void ego_sum(const EgoState* s, uint8_t days, uint16_t* gain, uint16_t* loss);

/* Пауза на сутки: решение человека, за эти сутки штрафа нет. */
void ego_set_pause(EgoState* s, uint32_t day, bool on);
bool ego_is_paused(const EgoState* s, uint32_t day);

/* Данные (ego_data.c) */
const char* ego_cause_label(uint8_t index);
const char* ego_band_letter(uint8_t band); // "E" .. "S"
const char* ego_band_name(uint8_t band); // КРИТИЧЕСКИЙ .. ВЫСШИЙ
const char* ego_roman(uint8_t n); // 1..11 -> I..XI, иначе пустая строка
const char* ego_pillar_name(uint8_t pillar);
