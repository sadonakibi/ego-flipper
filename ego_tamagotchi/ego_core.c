#include "ego_core.h"
#include <string.h>

static const uint16_t k_band_start[EGO_BANDS] = {0, 150, 300, 500, 700, 900};
static const uint8_t k_band_mult[EGO_BANDS] = {50, 50, 75, 100, 125, 150}; // x100

/* ---------- ранги, круги, множитель штрафов ---------- */

uint8_t ego_circle(uint16_t ego) {
    uint16_t c = ego / EGO_CIRCLE + 1;
    return (uint8_t)(c > EGO_CIRCLES ? EGO_CIRCLES : c);
}

uint16_t ego_circle_base(uint16_t ego) {
    return (uint16_t)((ego_circle(ego) - 1) * EGO_CIRCLE);
}

uint8_t ego_band(uint16_t ego) {
    uint16_t rel = ego - ego_circle_base(ego);
    for(int b = EGO_BANDS - 1; b > 0; b--)
        if(rel >= k_band_start[b]) return (uint8_t)b;
    return 0;
}

uint8_t ego_rank_index(uint16_t ego) {
    return (uint8_t)((ego_circle(ego) - 1) * EGO_BANDS + ego_band(ego));
}

uint16_t ego_next_boundary(uint16_t ego) {
    if(ego >= EGO_MAX) return 0;
    uint16_t base = ego_circle_base(ego), rel = ego - base;
    for(int b = 1; b < EGO_BANDS; b++)
        if(rel < k_band_start[b]) return (uint16_t)(base + k_band_start[b]);
    return (uint16_t)(base + EGO_CIRCLE); // верхняя полоса S: до сборки круга
}

EgoLevel ego_level(uint16_t ego) {
    static const EgoLevel by_band[EGO_BANDS] = {
        EgoLevelCritical, EgoLevelWeak, EgoLevelWeak, EgoLevelStable, EgoLevelStrong, EgoLevelStrong};
    EgoLevel level = by_band[ego_band(ego)];
    if(ego_circle(ego) >= 2 && level < EgoLevelStable) level = EgoLevelStable; // собранное EGO не выглядит критическим
    return level;
}

uint8_t ego_peak_degree(const EgoState* s) {
    uint16_t d = s->peak / EGO_CIRCLE;
    return (uint8_t)(d > EGO_CIRCLES ? EGO_CIRCLES : d);
}

uint16_t ego_penalty_mult(uint16_t ego) {
    uint32_t circle_factor = 100 + 25 * (uint32_t)(ego_circle(ego) - 1); // +25% за круг
    return (uint16_t)(circle_factor * k_band_mult[ego_band(ego)] / 100);
}

int ego_scaled(uint16_t ego, int base) {
    int v = (base * (int)ego_penalty_mult(ego) + 50) / 100;
    return v < 1 ? 1 : v;
}

/* ---------- состояние ---------- */

static uint16_t sat16(uint32_t v) {
    return v > 65535 ? 65535 : (uint16_t)v;
}

static int popcount8(uint8_t v) {
    int n = 0;
    for(int i = 0; i < 8; i++)
        if(v & (1 << i)) n++;
    return n;
}

/* Выше основания: именно это сжигание может забрать. */
static int room(const EgoState* s) {
    return s->ego > s->floor ? s->ego - s->floor : 0;
}

/* Меняет EGO с насыщением и ведёт учёт прироста и слива. Основание не может быть выше EGO:
 * если EGO упало ниже него (штраф пробивает), основание опускается следом. */
static int change(EgoState* s, int delta) {
    int before = s->ego;
    int n = before + delta;
    if(n < 0) n = 0;
    if(n > EGO_MAX) n = EGO_MAX;
    s->ego = (uint16_t)n;
    int real = n - before;
    if(real > 0)
        s->day_gain = sat16((uint32_t)s->day_gain + (uint32_t)real);
    else
        s->day_loss = sat16((uint32_t)s->day_loss + (uint32_t)(-real));
    if(s->ego > s->peak) s->peak = s->ego;
    if(s->floor > s->ego) s->floor = s->ego;
    return real;
}

void ego_state_init(EgoState* s) {
    memset(s, 0, sizeof(*s));
    s->ego = EGO_START;
    s->floor = EGO_START_FLOOR;
    s->peak = EGO_START;
    s->day_start_total = s->ego;
    s->savings_k = EGO_SAVINGS_START;
    for(int i = 0; i < EGO_POOL_COUNT; i++) s->load[i] = ego_quest((uint8_t)i)->base_load;
}

uint16_t ego_total(const EgoState* s) {
    return s->ego;
}

uint32_t ego_clock(uint32_t timestamp, uint8_t* window) {
    uint32_t shifted = timestamp > EGO_DAY_START_HOUR * 3600UL ?
                           timestamp - EGO_DAY_START_HOUR * 3600UL :
                           0;
    uint32_t hours = (shifted % 86400UL) / 3600UL; // часы от начала суток
    *window = (uint8_t)(hours < 6 ? 0 : (hours < 12 ? 1 : 2));
    return shifted / 86400UL + 1; // 0 зарезервирован под «не инициализировано»
}

uint8_t ego_window(const EgoState* s, uint32_t now, uint32_t day, uint8_t clock_window) {
    if(s->wake_ts && s->wake_day == day && !s->sleep_since && now >= s->wake_ts) {
        uint32_t hours = (now - s->wake_ts) / 3600;
        return (uint8_t)(hours < 6 ? 0 : (hours < 12 ? 1 : 2));
    }
    return clock_window;
}

/* ---------- сутки: минимум, основание, повторы, привычки, сон, колода ---------- */

static uint8_t min_penalty(EgoState* s) {
    int missing = EGO_MIN_ITEMS - popcount8(s->min_done);
    if(missing <= 0 || s->day == s->pause_day) return 0;
    for(int i = 0; i < missing; i++) change(s, -ego_scaled(s->ego, EGO_MIN_PENALTY));
    return (uint8_t)missing;
}

/* Основание подтягивается к EGO: половина разрыва, не больше EGO_FLOOR_RISE_MAX за круг в сутки. */
static void consolidate(EgoState* s) {
    if(popcount8(s->min_done) < EGO_FLOOR_MIN_DONE && s->day != s->pause_day) return;
    if(s->ego <= s->floor) return;
    int rise = (s->ego - s->floor) / 2;
    int cap = EGO_FLOOR_RISE_MAX * ego_circle(s->ego);
    if(rise > cap) rise = cap;
    s->floor = (uint16_t)(s->floor + rise);
}

/* Завершённые сутки уходят в историю графика. */
static void history_push(EgoState* s) {
    if(s->hist_n == EGO_HISTORY_DAYS) {
        memmove(&s->hist[0], &s->hist[1], (EGO_HISTORY_DAYS - 1) * sizeof(EgoDay));
        s->hist_n--;
    }
    EgoDay* d = &s->hist[s->hist_n++];
    d->total = s->ego;
    d->gain = s->day_gain;
    d->loss = s->day_loss;
}

/* Нагрузка: +шаг за каждые EGO_LOAD_STEP_DAYS выполненных дней; два пропущенных подряд - минус шаг. */
static void load_rollover(EgoState* s) {
    for(int slot = 0; slot < EGO_DECK_SIZE; slot++) {
        uint8_t pool = s->deck[slot];
        const EgoQuest* q = ego_quest(pool);
        if(q->load_kind == EGO_LOAD_NONE || (s->deck_done & (1u << slot))) continue; // у выполненных рост считается при выполнении
        if(++s->load_miss[pool] >= 2) {
            s->load[pool] = s->load[pool] > q->base_load + q->load_step ? (uint8_t)(s->load[pool] - q->load_step) :
                                                                         q->base_load;
            s->load_miss[pool] = 0;
            s->load_prog[pool] = 0;
        }
    }
}

/* Бонусный пункт ротации для слота k в сутки day. */
static uint8_t bonus_pick(uint32_t day, int k) {
    return (uint8_t)(EGO_BONUS_FIRST + (day * EGO_BONUS_SLOTS + (uint32_t)k) % EGO_BONUS_POOL);
}

bool ego_daughter_day(const EgoState* s, uint32_t day) {
    return s->daughter_anchor != 0 && day >= s->daughter_anchor &&
           (day - s->daughter_anchor) % EGO_DAUGHTER_CYCLE == 0;
}

/* Колода дня: 14 ежедневных + 3 бонуса из ротации; в особые дни первый бонус - «время с близкими». */
static void deck_build(EgoState* s, uint32_t day) {
    for(int i = 0; i < EGO_CORE_QUESTS; i++) s->deck[i] = (uint8_t)i;
    for(int k = 0; k < EGO_BONUS_SLOTS; k++) s->deck[EGO_CORE_QUESTS + k] = bonus_pick(day, k);
    if(ego_daughter_day(s, day)) s->deck[EGO_CORE_QUESTS] = EGO_DAUGHTER_QUEST;
    s->deck_done = 0;
    s->deck_drop = 0;
    memset(s->deck_post, 0, sizeof(s->deck_post));
    memset(s->deck_ready, 0, sizeof(s->deck_ready));
}

bool ego_daughter_toggle(EgoState* s, uint32_t day) {
    bool on = !ego_daughter_day(s, day);
    s->daughter_anchor = on ? day : 0;
    s->deck[EGO_CORE_QUESTS] = on ? EGO_DAUGHTER_QUEST : bonus_pick(day, 0);
    s->deck_done &= ~(1u << EGO_CORE_QUESTS);
    s->deck_drop &= ~(1u << EGO_CORE_QUESTS);
    s->deck_post[EGO_CORE_QUESTS] = 0;
    s->deck_ready[EGO_CORE_QUESTS] = 0;
    return on;
}

static void hist_push8(uint8_t* hist, uint8_t value) {
    memmove(&hist[0], &hist[1], EGO_SLEEP_HIST - 1);
    hist[EGO_SLEEP_HIST - 1] = value;
}

/* Конец суток для привычек: бонус за сокращение срывов относительно вчера, статистика. */
static void habit_rollover(EgoState* s) {
    if(s->habit_prev_slips > s->habit_slips) {
        int bonus = (s->habit_prev_slips - s->habit_slips) * EGO_HABIT_REDUCE_BONUS;
        if(bonus > EGO_HABIT_REDUCE_MAX) bonus = EGO_HABIT_REDUCE_MAX;
        change(s, bonus);
        s->habit_bonus_notice = sat16((uint32_t)s->habit_bonus_notice + (uint32_t)bonus);
    }
    hist_push8(s->slips_hist, s->habit_slips);
    s->habit_prev_slips = s->habit_slips;
    s->habit_noticed = 0;
    s->habit_held = 0;
    s->habit_slips = 0;
    s->habit_notice_last = 0;
    s->habit_hold_last = 0;
}

static void sleep_rollover(EgoState* s) {
    uint16_t units = (uint16_t)(s->sleep_last_total / 6);
    hist_push8(s->sleep_hist, units > 255 ? 255 : (uint8_t)units);
    s->sleep_last_total = 0;
    s->sleep_adj = 0;
    s->sleep_awarded = 0;
}

uint8_t ego_update(EgoState* s, uint32_t day) {
    if(s->day == 0) { // первый запуск: прошлого нет, штрафовать не за что
        s->day = day;
        s->day_start_total = s->ego;
        deck_build(s, day);
        return 0;
    }
    if(day < s->day) return 0; // часы ушли назад

    uint16_t missed = 0;
    if(day - s->day > EGO_MAX_CATCHUP_DAYS) s->day = day - EGO_MAX_CATCHUP_DAYS; // давно не заходил
    while(s->day < day) {
        missed += min_penalty(s);
        consolidate(s);
        load_rollover(s);
        habit_rollover(s);
        sleep_rollover(s);
        history_push(s);
        s->day++;
        s->min_done = 0;
        s->day_gain = 0;
        s->day_loss = 0;
        s->day_start_total = s->ego;
        deck_build(s, s->day);
    }
    return (uint8_t)(missed > 255 ? 255 : missed);
}

/* ---------- уход и возврат ---------- */

bool ego_is_out(const EgoState* s) {
    return s->out_since != 0;
}

uint32_t ego_out_seconds(const EgoState* s, uint32_t now) {
    return (s->out_since && now > s->out_since) ? now - s->out_since : 0;
}

uint16_t ego_burn(EgoState* s, uint32_t now) {
    if(!s->out_since) return 0;
    uint32_t total = 0;
    int cap = (EGO_BURN_CAP * (int)s->out_mult + 50) / 100;
    if(cap < 1) cap = 1;

    while(s->burn_ts + EGO_BURN_STEP_SEC <= now) {
        uint32_t step_end = s->burn_ts + EGO_BURN_STEP_SEC;
        uint32_t out = step_end - s->out_since;
        int base = (out > EGO_BURN_FAST_AFTER_SEC) ? EGO_BURN_RATE_FAST : EGO_BURN_RATE;
        int rate = (base * (int)s->out_mult + 50) / 100;
        if(rate < 1) rate = 1;

        int amount = rate;
        if(amount > room(s)) amount = room(s); // ниже основания не горит
        int left = cap - (int)s->out_burned;
        if(amount > left) amount = left;

        if(amount <= 0) { // жечь нечего: пропускаем оставшееся время разом
            s->burn_ts += ((now - s->burn_ts) / EGO_BURN_STEP_SEC) * EGO_BURN_STEP_SEC;
            break;
        }
        change(s, -amount);
        s->out_burned = (uint16_t)(s->out_burned + amount);
        total += (uint32_t)amount;
        s->burn_ts = step_end;
    }
    return sat16(total);
}

void ego_depart(EgoState* s, uint32_t now) {
    if(s->out_since) return;
    if(now == 0) now = 1; // 0 означает «в EGO»
    s->out_since = now;
    s->burn_ts = now;
    s->out_burned = 0;
    s->out_mult = ego_penalty_mult(s->ego); // множитель фиксируется на момент ухода
}

int ego_return(EgoState* s, uint32_t now) {
    if(!s->out_since) return -1;
    ego_burn(s, now);

    uint32_t out = ego_out_seconds(s, now);
    int pct = out <= EGO_RETURN_FULL_SEC ? 100 : (out <= EGO_RETURN_HALF_SEC ? 50 : 25);
    int restored = (int)s->out_burned * pct / 100 + EGO_RETURN_BONUS;
    change(s, restored);

    if(s->returns < 65535) s->returns++;
    s->return_sec += out > 86400 ? 86400 : out;
    s->out_since = 0;
    s->burn_ts = 0;
    s->out_burned = 0;
    return restored;
}

EgoReaction ego_cause(EgoState* s, uint8_t index) {
    if(index >= EGO_CAUSE_COUNT) return EgoReactionNone;
    change(s, -ego_scaled(s->ego, EGO_CAUSE_COST)); // налог на анализ, с множителем
    if(s->cause_counts[index] < 65535) s->cause_counts[index]++;
    return EgoReactionPenalty;
}

/* ---------- задания дня ---------- */

const EgoQuest* ego_slot_quest(const EgoState* s, uint8_t slot) {
    return ego_quest(s->deck[slot < EGO_DECK_SIZE ? slot : 0]);
}

uint8_t ego_slot_reps(const EgoState* s, uint8_t slot) {
    if(slot >= EGO_DECK_SIZE) return 0;
    uint8_t pool = s->deck[slot];
    return ego_quest(pool)->load_kind == EGO_LOAD_REPS ? s->load[pool] : 0;
}

uint8_t ego_slot_minutes(const EgoState* s, uint8_t slot) {
    if(slot >= EGO_DECK_SIZE) return 0;
    uint8_t pool = s->deck[slot];
    const EgoQuest* q = ego_quest(pool);
    if(q->load_kind != EGO_LOAD_MINUTES) return 0;
    uint8_t degree = ego_peak_degree(s);
    if(degree > EGO_CIRCLES - 1) degree = EGO_CIRCLES - 1;
    uint16_t m = (uint16_t)(s->load[pool] + q->per_circle * degree); // сложность растёт по кругам
    return (uint8_t)(m > 240 ? 240 : m);
}

bool ego_load_raise(EgoState* s, uint8_t slot) {
    if(slot >= EGO_DECK_SIZE || (s->deck_done & (1u << slot))) return false;
    uint8_t pool = s->deck[slot];
    const EgoQuest* q = ego_quest(pool);
    if(q->load_kind == EGO_LOAD_NONE || s->load[pool] >= q->max_load) return false;
    int n = s->load[pool] + q->load_step;
    s->load[pool] = (uint8_t)(n > q->max_load ? q->max_load : n);
    s->load_prog[pool] = 0;
    return true;
}

bool ego_slot_is_bonus(uint8_t slot) {
    return slot >= EGO_CORE_QUESTS && slot < EGO_DECK_SIZE;
}

/* Пункт минимума закрыт, когда выполнены все задания с этим битом (отжимания и приседания вместе). */
static void min_update_bit(EgoState* s, int bit) {
    bool any = false, all = true;
    for(int i = 0; i < EGO_CORE_QUESTS; i++) {
        if(ego_quest((uint8_t)i)->min_bit != bit) continue;
        any = true;
        if(!(s->deck_done & (1u << i))) all = false;
    }
    if(any && all) s->min_done |= (uint8_t)(1 << bit);
}

EgoReaction ego_quest_done(EgoState* s, uint8_t slot) {
    if(slot >= EGO_DECK_SIZE || (s->deck_done & (1u << slot))) return EgoReactionNone;
    uint8_t pool = s->deck[slot];
    const EgoQuest* q = ego_quest(pool);

    change(s, q->points);
    s->deck_done |= (1u << slot);
    if(s->quest_count[pool] < 65535) s->quest_count[pool]++;
    if(q->min_bit >= 0) min_update_bit(s, q->min_bit);
    if(q->pillar < EGO_PILLARS && s->pillar_count[q->pillar] < 65535) s->pillar_count[q->pillar]++;
    if(q->load_kind != EGO_LOAD_NONE) { // нагрузка растёт на шаг за каждые 2 выполненных дня
        s->load_miss[pool] = 0;
        if(++s->load_prog[pool] >= EGO_LOAD_STEP_DAYS) {
            int n = s->load[pool] + q->load_step;
            s->load[pool] = (uint8_t)(n > q->max_load ? q->max_load : n);
            s->load_prog[pool] = 0;
        }
    }
    s->total_reports++;
    return EgoReactionGrowth;
}

uint8_t ego_quests_today(const EgoState* s) {
    uint8_t n = 0;
    for(int i = 0; i < EGO_CORE_QUESTS; i++)
        if(s->deck_done & (1u << i)) n++;
    return n;
}

uint8_t ego_bonus_today(const EgoState* s) {
    uint8_t n = 0;
    for(int i = EGO_CORE_QUESTS; i < EGO_DECK_SIZE; i++)
        if(s->deck_done & (1u << i)) n++;
    return n;
}

uint8_t ego_min_progress(const EgoState* s) {
    return (uint8_t)popcount8(s->min_done);
}

EgoReaction ego_check(EgoState* s, uint8_t kind, bool ok) {
    if(kind >= EGO_CHECK_COUNT) return EgoReactionNone;
    if(ok) {
        change(s, EGO_CHECK_OK);
        if(s->check_ok[kind] < 65535) s->check_ok[kind]++;
        return EgoReactionGrowth;
    }
    change(s, -ego_scaled(s->ego, EGO_CHECK_FAIL));
    if(s->check_fail[kind] < 65535) s->check_fail[kind]++;
    return EgoReactionPenalty;
}

int ego_postpone_cost(const EgoState* s, uint8_t slot) {
    if(slot >= EGO_DECK_SIZE || ego_slot_is_bonus(slot)) return 0;
    return s->deck_post[slot] > 0 ? 1 : 0;
}

int ego_quest_postpone(EgoState* s, uint8_t slot, uint32_t now) {
    if(slot >= EGO_DECK_SIZE) return -1;
    if(ego_slot_is_bonus(slot)) { // бонус пропускается бесплатно
        s->deck_drop |= (1u << slot);
        return 0;
    }
    int result = 0;
    if(s->deck_post[slot] > 0) { // первый перенос бесплатный, дальше 0.5
        result = 1;
        if(++s->half_debt >= 2) {
            s->half_debt = 0;
            change(s, -ego_scaled(s->ego, EGO_POSTPONE_HALF));
            result = 2;
        }
    }
    if(s->deck_post[slot] < 255) s->deck_post[slot]++;
    s->deck_ready[slot] = now + EGO_CARD_INTERVAL_SEC;
    if(s->deck_post[slot] >= EGO_POSTPONE_MAX) s->deck_drop |= (1u << slot);
    return result;
}

int ego_next_card(const EgoState* s, uint8_t window, uint32_t now) {
    int fallback = -1;
    for(int slot = 0; slot < EGO_CORE_QUESTS; slot++) {
        if((s->deck_done | s->deck_drop) & (1u << slot)) continue;
        if(s->deck_ready[slot] > now) continue;
        const EgoQuest* q = ego_quest(s->deck[slot]);
        if(q->window != 3 && q->window > window) continue; // окно ещё не наступило
        if(q->window == window) return slot; // задание текущего окна
        if(fallback < 0) fallback = slot; // просроченное или «в любое время»
    }
    if(fallback >= 0) return fallback;
    for(int slot = EGO_CORE_QUESTS; slot < EGO_DECK_SIZE; slot++) { // бонусы, когда ядро выдано
        if((s->deck_done | s->deck_drop) & (1u << slot)) continue;
        if(s->deck_ready[slot] > now) continue;
        return slot;
    }
    return -1;
}

/* ---------- контроль привычек ---------- */

EgoHabitResult ego_habit_notice(EgoState* s, uint32_t now) {
    if(s->habit_noticed >= EGO_HABIT_NOTICE_MAX) return EgoHabitMax;
    if(s->habit_notice_last && now < s->habit_notice_last + EGO_HABIT_GAP_SEC) return EgoHabitTooSoon;
    s->habit_noticed++;
    s->habit_notice_last = now;
    if(s->habit_total_noticed < 65535) s->habit_total_noticed++;
    change(s, EGO_HABIT_NOTICE_GAIN);
    return EgoHabitOk;
}

EgoHabitResult ego_habit_hold(EgoState* s, uint32_t now) {
    if(s->habit_held >= EGO_HABIT_HOLD_MAX) return EgoHabitMax;
    if(s->habit_hold_last && now < s->habit_hold_last + EGO_HABIT_GAP_SEC) return EgoHabitTooSoon;
    s->habit_held++;
    s->habit_hold_last = now;
    if(s->habit_total_held < 65535) s->habit_total_held++;
    change(s, EGO_HABIT_HOLD_GAIN);
    return EgoHabitOk;
}

int ego_habit_slip(EgoState* s) {
    int amount = ego_scaled(s->ego, EGO_HABIT_SLIP);
    change(s, -amount);
    if(s->habit_slips < 255) s->habit_slips++;
    if(s->habit_total_slips < 65535) s->habit_total_slips++;
    return amount;
}

/* ---------- сон ---------- */

bool ego_sleeping(const EgoState* s) {
    return s->sleep_since != 0;
}

uint16_t ego_sleep_minutes(const EgoState* s, uint32_t now) {
    int total = 0;
    uint32_t lo_bound = now > 86400UL ? now - 86400UL : 0;
    for(int i = 0; i < EGO_SLEEP_SEGMENTS; i++) {
        uint32_t lo = s->seg_start[i] > lo_bound ? s->seg_start[i] : lo_bound;
        uint32_t hi = s->seg_end[i] < now ? s->seg_end[i] : now;
        if(hi > lo) total += (int)((hi - lo) / 60);
    }
    if(s->sleep_since && now > s->sleep_since) {
        uint32_t cur = now - s->sleep_since;
        if(cur > EGO_SLEEP_SEG_MAX_SEC) cur = EGO_SLEEP_SEG_MAX_SEC;
        total += (int)(cur / 60);
    }
    total += s->sleep_adj;
    if(total < 0) total = 0;
    if(total > 24 * 60) total = 24 * 60;
    return (uint16_t)total;
}

static int sleep_points(uint16_t minutes) {
    if(minutes >= 420) return 8;
    if(minutes >= 360) return 5;
    if(minutes >= 300) return 3;
    return 0;
}

/* Очки и пункт минимума по итогу сна за 24 часа; повторный вызов добирает разницу. */
static void sleep_evaluate(EgoState* s, uint32_t now) {
    uint16_t total = ego_sleep_minutes(s, now);
    s->sleep_last_total = total;
    if(total >= EGO_SLEEP_FIXED_MIN)
        s->min_done |= (uint8_t)(1 << EGO_MIN_SLEEP);
    else
        s->min_done &= (uint8_t) ~(1 << EGO_MIN_SLEEP);
    int pts = sleep_points(total);
    if(pts > s->sleep_awarded) {
        change(s, pts - s->sleep_awarded);
        s->sleep_awarded = (uint8_t)pts;
    }
}

bool ego_sleep_toggle(EgoState* s, uint32_t now, uint32_t day) {
    if(now == 0) now = 1;
    if(!s->sleep_since) { // лёг
        s->sleep_since = now;
        return false;
    }
    uint32_t start = s->sleep_since, dur = now > start ? now - start : 0; // встал
    if(dur > EGO_SLEEP_SEG_MAX_SEC) dur = EGO_SLEEP_SEG_MAX_SEC;
    s->seg_start[s->seg_head] = start;
    s->seg_end[s->seg_head] = start + dur;
    s->seg_head = (uint8_t)((s->seg_head + 1) % EGO_SLEEP_SEGMENTS);
    s->sleep_since = 0;
    s->wake_ts = now;
    s->wake_day = day;
    sleep_evaluate(s, now);
    if(s->pillar_count[0] < 65535) s->pillar_count[0]++;
    if(!(s->deck_done & 1u)) ego_quest_done(s, 0); // «Подъём» закрывается подъёмом
    return true;
}

void ego_sleep_adjust(EgoState* s, int delta_min, uint32_t now) {
    int adj = s->sleep_adj + delta_min;
    if(adj > 600) adj = 600;
    if(adj < -600) adj = -600;
    s->sleep_adj = (int16_t)adj;
    if(!s->sleep_since && s->wake_ts) sleep_evaluate(s, now); // пересчитать очки и пункт минимума
}

/* ---------- копилка ---------- */

int ego_savings_add(EgoState* s, int delta_k, uint32_t day) {
    int n = (int)s->savings_k + delta_k;
    if(n < 0) n = 0;
    if(n > 999) n = 999;
    int real = n - (int)s->savings_k;
    s->savings_k = (uint16_t)n;
    if(real > 0 && s->savings_day != day) {
        s->savings_day = day;
        change(s, EGO_SAVINGS_DAY_BONUS);
        return EGO_SAVINGS_DAY_BONUS;
    }
    return 0;
}

/* ---------- динамика, пауза ---------- */

int ego_day_delta(const EgoState* s) {
    return (int)s->ego - (int)s->day_start_total;
}

void ego_sum(const EgoState* s, uint8_t days, uint16_t* gain, uint16_t* loss) {
    uint32_t g = s->day_gain, l = s->day_loss;
    for(uint8_t i = 1; i < days && i <= s->hist_n; i++) {
        const EgoDay* d = &s->hist[s->hist_n - i];
        g += d->gain;
        l += d->loss;
    }
    *gain = sat16(g);
    *loss = sat16(l);
}

void ego_set_pause(EgoState* s, uint32_t day, bool on) {
    s->pause_day = on ? day : 0;
}

bool ego_is_paused(const EgoState* s, uint32_t day) {
    return s->pause_day != 0 && s->pause_day == day;
}
