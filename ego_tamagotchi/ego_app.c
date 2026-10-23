#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <stdio.h>
#include <string.h>

#include "ego_core.h"
#include "ego_draw.h"
#include "ego_font.h"
#include "ego_storage.h"

#define TICK_MS 70
#define CLOCK_CHECK_TICKS 42 // ~раз в 3 с: сутки, сжигание
#define DUTY_CHECK_TICKS 14 // ~раз в секунду: карточки дежурного режима и таймер
#define MSG_TICKS 30 // сообщение держится ~2 с
#define BANNER_PERIOD 20 // мигание баннера критического порога (тики)
#define IDLE_CLOSE_TICKS (60000 / TICK_MS) // минута без нажатий - выход (не в дежурстве, не в таймере, не во сне)
#define PROFILE_PAGES 4
#define CONTROL_ROWS 4 // заметил, удержался, срыв, выход из EGO
#define CONTROL_ROW_SLIP 2
#define CONTROL_ROW_EXIT 3
#define MONEY_ROWS 4 // пополнил, снял, шаг, особый день
#define MONEY_ROW_ADD 0
#define MONEY_ROW_TAKE 1
#define MONEY_ROW_STEP 2
#define MONEY_ROW_DAUGHTER 3

/* Главный экран: Око, число EGO, ранг, шкала круга.
 *   ОК - задания, влево - ДЕНЬГИ (копилка, особый день), вниз - КОНТРОЛЬ (привычки),
 *   вверх из КОНТРОЛЯ - строка «ВЫХОД ИЗ EGO», удержание ОК вне EGO - возврат,
 *   вправо - сон, вверх - профиль. */
typedef enum {
    ScreenSplash, // заставка: глаз выжигает EGO
    ScreenStatus,
    ScreenTasks, // ежедневный список и бонусы
    ScreenCauses, // причины после возврата
    ScreenControl, // контроль: привычки и выход из EGO (таймер) в одном модуле
    ScreenMoney, // деньги: копилка и особый день
    ScreenSleep, // фиксация сна
    ScreenProfile, // профиль: логотип, герой и копилка, данные, динамика
    ScreenCard, // карточка дежурного режима
    ScreenTimer, // фокус-таймер задания
} Screen;

typedef enum {
    CardNone,
    CardQuest, // задание дня
    CardFlow, // проверка: потоковость (час без оценки действий)
    CardEgo, // проверка: быть в EGO
} CardKind;

typedef enum {
    EventInput,
    EventTick,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} EgoEvent;

typedef struct {
    EgoState state;
    Screen screen;
    uint32_t splash_tick;

    uint8_t quest_sel; // выбранное задание
    uint16_t cause_mask; // причины после возврата
    uint8_t cause_cur;
    uint8_t habit_sel;
    uint8_t money_sel;
    uint8_t profile_page;
    uint8_t savings_step; // 1..EGO_SAVINGS_STEP_MAX тысяч за нажатие

    /* дежурный режим */
    bool duty;
    CardKind card_kind;
    int card_slot; // слот задания, если card_kind == CardQuest, иначе -1
    uint32_t card_deadline;
    uint32_t next_deal; // когда выдавать следующее задание
    uint32_t check_due[EGO_CHECK_COUNT]; // когда спрашивать потоковость и «в EGO»

    /* фокус-таймер */
    int timer_slot; // -1, если не идёт
    uint32_t timer_end;
    uint32_t timer_total;

    EgoReaction reaction;
    uint8_t reaction_tick;
    uint32_t day; // текущие сутки
    uint8_t window; // окно дня от подъёма 0..2
    uint8_t prev_rank; // для сообщений о смене ранга
    uint8_t prev_degree; // для сообщения о сборке круга
    uint32_t tick;
    uint32_t idle_ticks;
    char msg[32];
    uint8_t msg_ticks;

    bool running;
    Gui* gui;
    NotificationApp* notifications;
    ViewPort* view_port;
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    FuriTimer* timer;
} EgoApp;

/* ---------- логика приложения ---------- */

static uint32_t now_ts(void) {
    return furi_hal_rtc_get_timestamp();
}

/* Запись события в CSV на SD: delta - изменение EGO после события. */
static void log_event(EgoApp* app, const char* event, int32_t id, int before) {
    ego_log(now_ts(), event, id, (int)app->state.ego - before, app->state.ego);
}

static void start_reaction(EgoApp* app, EgoReaction r) {
    app->reaction = r;
    app->reaction_tick = 0;
}

static void set_msg(EgoApp* app, const char* text) {
    snprintf(app->msg, sizeof(app->msg), "%s", text);
    app->msg_ticks = MSG_TICKS;
}

/* Ранг в игровом стиле: буква и номер круга, например "B" или "C II". */
static void rank_label(char* buf, size_t n, uint16_t ego) {
    uint8_t circle = ego_circle(ego);
    if(circle > 1)
        snprintf(buf, n, "%s %s", ego_band_letter(ego_band(ego)), ego_roman(circle));
    else
        snprintf(buf, n, "%s", ego_band_letter(ego_band(ego)));
}

/* Подпись задания с текущей нагрузкой: повторы или минуты (Отжимания 31, Код 47М). */
static void quest_label(const EgoState* s, uint8_t slot, char* buf, size_t n) {
    const char* label = ego_slot_quest(s, slot)->label;
    uint8_t reps = ego_slot_reps(s, slot), minutes = ego_slot_minutes(s, slot);
    if(reps)
        snprintf(buf, n, "%s %u", label, reps);
    else if(minutes)
        snprintf(buf, n, "%s %uМ", label, minutes);
    else
        snprintf(buf, n, "%s", label);
}

/* Смена ранга и сборка круга: короткие строгие сообщения. Ниже порога - ещё и баннер. */
static void track_ranks(EgoApp* app) {
    uint8_t cur = ego_rank_index(app->state.ego);
    uint8_t degree = ego_peak_degree(&app->state);
    char buf[32], label[16];

    if(degree > app->prev_degree) {
        snprintf(buf, sizeof(buf), "КРУГ СОБРАН: S %s", ego_roman(degree));
        set_msg(app, buf);
    } else if(cur != app->prev_rank) {
        rank_label(label, sizeof(label), app->state.ego);
        snprintf(buf, sizeof(buf), cur > app->prev_rank ? "РАНГ ВЫШЕ: %s" : "РАНГ НИЖЕ: %s", label);
        set_msg(app, buf);
    }
    app->prev_rank = cur;
    app->prev_degree = degree;
}

static void notify_card(EgoApp* app) {
    notification_message(app->notifications, &sequence_double_vibro);
    notification_message(app->notifications, &sequence_display_backlight_on);
}

static void save_and_track(EgoApp* app) {
    ego_storage_save(&app->state);
    track_ranks(app);
}

static void clear_card(EgoApp* app) {
    app->card_kind = CardNone;
    app->card_slot = -1;
}

/* Сверка с часами: закрывает сутки (штраф за минимум, привычки, сон), жжёт EGO вне ухода. */
static void sync_clock(EgoApp* app) {
    uint8_t clock_window;
    uint32_t ts = now_ts();
    uint32_t day = ego_clock(ts, &clock_window);
    app->day = day;
    bool changed = false;
    char buf[64];

    int before = app->state.ego;
    uint8_t missed = ego_update(&app->state, day);
    if(missed) {
        snprintf(buf, sizeof(buf), "Минимум: -%d", before - (int)app->state.ego);
        set_msg(app, buf);
        start_reaction(app, EgoReactionPenalty);
        log_event(app, "miss", missed, before);
        changed = true;
    }
    if(app->state.habit_bonus_notice) { // бонус за сокращение срывов
        snprintf(buf, sizeof(buf), "Срывов меньше: +%u", app->state.habit_bonus_notice);
        set_msg(app, buf);
        start_reaction(app, EgoReactionGrowth);
        ego_log(ts, "habit_reduce", app->state.habit_bonus_notice, app->state.habit_bonus_notice, app->state.ego);
        app->state.habit_bonus_notice = 0;
        changed = true;
    }
    app->window = ego_window(&app->state, ts, day, clock_window);

    before = app->state.ego;
    uint16_t burned = ego_burn(&app->state, ts);
    if(burned) {
        snprintf(buf, sizeof(buf), "Горит: -%u", burned);
        set_msg(app, buf);
        log_event(app, "burn", burned, before);
        changed = true;
    }
    if(changed) save_and_track(app);
}

/* ---------- выход из EGO, возврат, причины ---------- */

static void do_depart(EgoApp* app) {
    sync_clock(app);
    if(ego_is_out(&app->state)) return;
    ego_depart(&app->state, now_ts());
    clear_card(app);
    app->timer_slot = -1;
    app->screen = ScreenStatus;
    start_reaction(app, EgoReactionPenalty);
    set_msg(app, "ВНЕ EGO");
    ego_storage_save(&app->state);
    ego_log(now_ts(), "out", 0, 0, app->state.ego);
}

static void do_return(EgoApp* app) {
    sync_clock(app);
    int before = app->state.ego;
    uint32_t out_sec = ego_out_seconds(&app->state, now_ts());
    int restored = ego_return(&app->state, now_ts());
    if(restored < 0) return;
    log_event(app, "return", (int32_t)out_sec, before);

    char buf[32];
    snprintf(buf, sizeof(buf), "Возврат: +%d", restored);
    set_msg(app, buf);
    start_reaction(app, EgoReactionReturn);
    save_and_track(app);

    app->cause_mask = 0; // теперь можно отметить причины: анализ, выход из EGO
    app->cause_cur = 0;
    app->screen = ScreenCauses;
}

static void apply_causes(EgoApp* app) {
    int start = app->state.ego;
    for(int i = 0; i < EGO_CAUSE_COUNT; i++) {
        if(!(app->cause_mask & (1 << i))) continue;
        int before = app->state.ego;
        ego_cause(&app->state, (uint8_t)i);
        log_event(app, "cause", i, before);
    }
    if((int)app->state.ego != start) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%+d EGO", (int)app->state.ego - start);
        set_msg(app, buf);
        save_and_track(app);
    }
    app->cause_mask = 0;
    app->screen = ScreenStatus;
}

/* ---------- контроль привычек ---------- */

/* what: 0 заметил (+1), 1 удержался (+3), 2 срыв (небольшой штраф). */
static void do_habit(EgoApp* app, int what) {
    sync_clock(app);
    int before = app->state.ego;
    char buf[32];
    if(what == 2) {
        int amount = ego_habit_slip(&app->state);
        snprintf(buf, sizeof(buf), "СРЫВ: -%d", amount);
        set_msg(app, buf);
        start_reaction(app, EgoReactionPenalty);
        log_event(app, "habit_slip", app->state.habit_slips, before);
        save_and_track(app);
        return;
    }
    EgoHabitResult r = what == 0 ? ego_habit_notice(&app->state, now_ts()) :
                                   ego_habit_hold(&app->state, now_ts());
    if(r == EgoHabitTooSoon) {
        set_msg(app, "РАНО: ЕЩЁ 10 МИН");
        return;
    }
    if(r == EgoHabitMax) {
        set_msg(app, "НА СЕГОДНЯ ХВАТИТ");
        return;
    }
    snprintf(buf, sizeof(buf), what == 0 ? "ЗАМЕТИЛ +%d" : "УДЕРЖАЛСЯ +%d",
             what == 0 ? EGO_HABIT_NOTICE_GAIN : EGO_HABIT_HOLD_GAIN);
    set_msg(app, buf);
    start_reaction(app, EgoReactionReset);
    log_event(app, what == 0 ? "habit_notice" : "habit_hold", what == 0 ? app->state.habit_noticed : app->state.habit_held, before);
    save_and_track(app);
}

/* Модуль «Контроль»: удержание ОК на срыве записывает срыв и сразу запускает таймер ухода,
 * на строке «Выход из EGO» просто запускает таймер; на остальных строках как короткое ОК. */
static void do_control_hold(EgoApp* app) {
    if(app->habit_sel == CONTROL_ROW_SLIP) {
        do_habit(app, CONTROL_ROW_SLIP);
        do_depart(app);
    } else if(app->habit_sel == CONTROL_ROW_EXIT) {
        do_depart(app);
    } else {
        do_habit(app, app->habit_sel);
    }
}

/* Взять шаг нагрузки сразу (иначе она растёт сама: шаг за 2 выполненных дня). */
static void do_load_raise(EgoApp* app, uint8_t slot) {
    char buf[32];
    if(!ego_load_raise(&app->state, slot)) {
        set_msg(app, "Нагрузка: нельзя");
        return;
    }
    uint8_t reps = ego_slot_reps(&app->state, slot), minutes = ego_slot_minutes(&app->state, slot);
    snprintf(buf, sizeof(buf), reps ? "НАГРУЗКА %u" : "НАГРУЗКА %uМ", reps ? reps : minutes);
    set_msg(app, buf);
    ego_log(now_ts(), "load", app->state.deck[slot], 0, app->state.ego);
    ego_storage_save(&app->state);
}

/* ---------- сон ---------- */

static void do_sleep_toggle(EgoApp* app) {
    if(ego_is_out(&app->state)) return;
    sync_clock(app);
    int before = app->state.ego;
    bool awake = ego_sleep_toggle(&app->state, now_ts(), app->day);
    sync_clock(app); // окно дня от подъёма
    char buf[32];
    if(awake) {
        snprintf(buf, sizeof(buf), "Встал: +%d", (int)app->state.ego - before);
        start_reaction(app, EgoReactionGrowth);
        log_event(app, "sleep_end", ego_sleep_minutes(&app->state, now_ts()), before);
    } else {
        snprintf(buf, sizeof(buf), "Сон");
        clear_card(app);
        app->timer_slot = -1;
        ego_log(now_ts(), "sleep_start", 0, 0, app->state.ego);
    }
    set_msg(app, buf);
    app->screen = ScreenStatus;
    save_and_track(app);
}

static void do_sleep_adjust(EgoApp* app, int delta_min) {
    int before = app->state.ego;
    ego_sleep_adjust(&app->state, delta_min, now_ts());
    log_event(app, "sleep_adjust", delta_min, before);
    save_and_track(app);
}

/* ---------- копилка и особый день ---------- */

static void do_savings(EgoApp* app, int sign) {
    sync_clock(app);
    int before = app->state.ego;
    int award = ego_savings_add(&app->state, sign * app->savings_step, app->day);
    char buf[32];
    if(award)
        snprintf(buf, sizeof(buf), "КОПИЛКА %uК +%d", app->state.savings_k, award);
    else
        snprintf(buf, sizeof(buf), "КОПИЛКА %uК", app->state.savings_k);
    set_msg(app, buf);
    if(award) start_reaction(app, EgoReactionGrowth);
    log_event(app, "savings", app->state.savings_k, before);
    save_and_track(app);
}

static void do_daughter(EgoApp* app);

/* Модуль «Деньги»: ОК на строке делает то, что в ней написано. */
static void do_money(EgoApp* app) {
    switch(app->money_sel) {
    case MONEY_ROW_ADD:
        do_savings(app, 1);
        break;
    case MONEY_ROW_TAKE:
        do_savings(app, -1);
        break;
    case MONEY_ROW_STEP:
        app->savings_step = app->savings_step % EGO_SAVINGS_STEP_MAX + 1;
        break;
    default:
        do_daughter(app);
        break;
    }
}

static void do_daughter(EgoApp* app) {
    sync_clock(app);
    bool on = ego_daughter_toggle(&app->state, app->day);
    set_msg(app, on ? "ОСОБЫЙ ДЕНЬ" : "ОСОБЫЙ ДЕНЬ: НЕТ");
    ego_log(now_ts(), "daughter", on ? 1 : 0, 0, app->state.ego);
    ego_storage_save(&app->state);
}

/* ---------- задания ---------- */

static void do_quest(EgoApp* app, uint8_t slot) {
    sync_clock(app);
    int before = app->state.ego;
    uint8_t pool = app->state.deck[slot];
    EgoReaction r = ego_quest_done(&app->state, slot);
    if(r == EgoReactionNone) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "+%d EGO", ego_quest(pool)->points);
    set_msg(app, buf);
    start_reaction(app, r);
    log_event(app, "quest", pool, before);
    save_and_track(app);
    if(app->card_kind == CardQuest && app->card_slot == slot) clear_card(app);
}

static void do_postpone(EgoApp* app, uint8_t slot) {
    sync_clock(app);
    if(app->state.deck_done & (1u << slot)) return;
    int before = app->state.ego;
    uint8_t pool = app->state.deck[slot];
    int cost = ego_quest_postpone(&app->state, slot, now_ts());
    if(cost < 0) return;

    if(ego_slot_is_bonus(slot))
        set_msg(app, "Бонус пропущен");
    else
        set_msg(app, cost == 0 ? "Перенос: бесплатно" : (cost == 1 ? "Перенос: -0.5" : "Перенос: -1"));
    log_event(app, "postpone", pool, before);
    save_and_track(app);
    if(app->card_kind == CardQuest && app->card_slot == slot) {
        clear_card(app);
        if(app->screen == ScreenCard) app->screen = ScreenStatus;
    }
}

/* Ответ на проверку: ДА +2, вылет со штрафом. auto_miss = ответа не было за EGO_CHECK_TIMEOUT_SEC. */
static void do_check(EgoApp* app, bool ok, bool auto_miss) {
    if(app->card_kind != CardFlow && app->card_kind != CardEgo) return;
    uint8_t kind = app->card_kind == CardFlow ? EGO_CHECK_FLOW : EGO_CHECK_EGO;
    sync_clock(app);
    int before = app->state.ego;
    EgoReaction r = ego_check(&app->state, kind, ok);
    log_event(app, ok ? "check" : (auto_miss ? "check_miss" : "check_fail"), kind, before);

    char buf[32];
    const char* what = kind == EGO_CHECK_FLOW ? "ПОТОК" : "В EGO";
    if(ok)
        snprintf(buf, sizeof(buf), "+%d %s", EGO_CHECK_OK, what);
    else
        snprintf(buf, sizeof(buf), "-%d %s", before - (int)app->state.ego, what);
    set_msg(app, buf);
    start_reaction(app, r);
    save_and_track(app);
    clear_card(app);
    if(app->screen == ScreenCard) app->screen = ScreenStatus;
}

static void toggle_pause(EgoApp* app) {
    sync_clock(app);
    ego_set_pause(&app->state, app->day, !ego_is_paused(&app->state, app->day));
    ego_storage_save(&app->state);
    log_event(app, "pause", ego_is_paused(&app->state, app->day) ? 1 : 0, app->state.ego);
}

static void toggle_duty(EgoApp* app) {
    app->duty = !app->duty;
    clear_card(app);
    uint32_t t = now_ts();
    app->next_deal = t;
    app->check_due[EGO_CHECK_FLOW] = t + EGO_CHECK_FLOW_SEC;
    app->check_due[EGO_CHECK_EGO] = t + EGO_CHECK_EGO_SEC;
    ego_log(t, "duty", app->duty ? 1 : 0, 0, app->state.ego);
    set_msg(app, app->duty ? "Дежурство вкл" : "Дежурство выкл");
}

/* ---------- фокус-таймеры ---------- */

static void start_timer(EgoApp* app, uint8_t slot) {
    uint8_t minutes = ego_slot_minutes(&app->state, slot);
    if(!minutes || (app->state.deck_done & (1u << slot))) return;
    sync_clock(app);
    app->timer_slot = slot;
    app->timer_total = (uint32_t)minutes * 60;
    app->timer_end = now_ts() + app->timer_total;
    app->screen = ScreenTimer;
    ego_log(now_ts(), "timer", app->state.deck[slot], 0, app->state.ego);
}

static void finish_timer(EgoApp* app) {
    uint8_t slot = (uint8_t)app->timer_slot;
    app->timer_slot = -1;
    do_quest(app, slot); // таймер дошёл до конца: задание засчитано само
    notify_card(app);
    app->card_kind = CardFlow; // и сразу вопрос про потоковость
    app->card_slot = -1;
    app->card_deadline = now_ts() + EGO_CHECK_TIMEOUT_SEC;
    app->screen = ScreenCard;
}

/* ---------- дежурный режим ---------- */

/* Выдать следующее задание. vibrate = false, когда карточка идёт сразу за проверкой. */
static bool deal_quest(EgoApp* app, uint32_t ts, bool vibrate) {
    int slot = ego_next_card(&app->state, app->window, ts);
    if(slot < 0) {
        app->next_deal = ts + 60; // карточек нет: проверим через минуту
        return false;
    }
    app->card_kind = CardQuest;
    app->card_slot = slot;
    app->card_deadline = ts + EGO_CARD_INTERVAL_SEC;
    app->next_deal = ts + EGO_CARD_INTERVAL_SEC;
    app->screen = ScreenCard;
    if(vibrate) notify_card(app);
    return true;
}

static void handle_duty(EgoApp* app) {
    /* тихие часы - это сон: пока вы спите или вне EGO, карточек нет */
    if(!app->duty || ego_is_out(&app->state) || ego_sleeping(&app->state) || app->timer_slot >= 0) return;
    uint32_t ts = now_ts();

    /* дедлайн: ядро переносится само, бонус пропускается, проверка без ответа - вылет */
    if(app->card_kind != CardNone && ts >= app->card_deadline) {
        if(app->card_kind == CardQuest)
            do_postpone(app, (uint8_t)app->card_slot);
        else
            do_check(app, false, true);
        clear_card(app);
        if(app->screen == ScreenCard) app->screen = ScreenStatus;
    }

    /* проверки важнее заданий: потоковость раз в час, «в EGO» раз в 71 минуту */
    static const uint32_t interval[EGO_CHECK_COUNT] = {EGO_CHECK_FLOW_SEC, EGO_CHECK_EGO_SEC};
    if(app->card_kind == CardNone || app->card_kind == CardQuest) {
        for(int k = 0; k < EGO_CHECK_COUNT; k++) {
            if(ts < app->check_due[k]) continue;
            app->check_due[k] = ts + interval[k];
            app->card_kind = k == EGO_CHECK_FLOW ? CardFlow : CardEgo; // карточка задания откладывается без платы
            app->card_slot = -1;
            app->card_deadline = ts + EGO_CHECK_TIMEOUT_SEC;
            app->next_deal = ts; // сразу за проверкой пойдёт задание, без второй вибрации
            app->screen = ScreenCard;
            notify_card(app);
            return;
        }
    }

    if(app->card_kind == CardNone && ts >= app->next_deal) deal_quest(app, ts, true);
}

/* ---------- ввод ---------- */

static void handle_input(EgoApp* app, const InputEvent* e) {
    app->idle_ticks = 0;
    bool out = ego_is_out(&app->state);
    bool asleep = ego_sleeping(&app->state);

    if(app->screen == ScreenSplash) { // любая клавиша пропускает заставку
        if(e->type == InputTypeShort || e->type == InputTypeLong) app->screen = ScreenStatus;
        return;
    }

    /* удержание ОК */
    if(e->type == InputTypeLong && e->key == InputKeyOk) {
        switch(app->screen) {
        case ScreenStatus:
            if(out) do_return(app); // выход - клавиша «влево», возврат - удержание ОК
            break;
        case ScreenCauses:
            apply_causes(app);
            break;
        case ScreenControl:
            do_control_hold(app);
            break;
        case ScreenTasks:
            do_quest(app, app->quest_sel);
            break;
        case ScreenCard:
            if(app->card_kind == CardQuest)
                do_quest(app, (uint8_t)app->card_slot);
            else if(app->card_kind != CardNone)
                do_check(app, true, false);
            app->screen = ScreenStatus;
            break;
        case ScreenProfile:
            if(app->profile_page == 2) toggle_pause(app);
            break;
        default:
            break;
        }
        return;
    }
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return;

    if(out || asleep) { // вне EGO и во сне ничего не открывается
        if(e->key == InputKeyBack)
            app->running = false;
        else if(asleep && e->key == InputKeyOk && e->type == InputTypeShort)
            do_sleep_toggle(app); // «встал»
        return;
    }

    if(e->key == InputKeyBack) {
        if(app->screen == ScreenTimer) {
            app->timer_slot = -1; // отмена таймера: задание не засчитано
            app->screen = ScreenStatus;
            return;
        }
        if(app->screen == ScreenCard && app->card_kind != CardQuest && app->card_kind != CardNone) return; // проверку не свернуть
        if(app->screen == ScreenStatus)
            app->running = false;
        else
            app->screen = ScreenStatus; // для причин это «без причины»
        return;
    }

    switch(app->screen) {
    case ScreenStatus:
        if(e->key == InputKeyOk) app->screen = ScreenTasks;
        else if(e->key == InputKeyUp) app->screen = ScreenProfile;
        else if(e->key == InputKeyDown) {
            app->habit_sel = 0; // привычки; вверх от первой строки - «выход из EGO»
            app->screen = ScreenControl;
        } else if(e->key == InputKeyRight) app->screen = ScreenSleep;
        else if(e->key == InputKeyLeft) {
            app->money_sel = MONEY_ROW_ADD;
            app->screen = ScreenMoney;
        }
        break;

    case ScreenMoney:
        if(e->key == InputKeyUp)
            app->money_sel = (app->money_sel + MONEY_ROWS - 1) % MONEY_ROWS;
        else if(e->key == InputKeyDown)
            app->money_sel = (app->money_sel + 1) % MONEY_ROWS;
        else if(e->key == InputKeyOk && e->type == InputTypeShort)
            do_money(app);
        break;

    case ScreenTasks:
        if(e->key == InputKeyUp)
            app->quest_sel = (app->quest_sel + EGO_DECK_SIZE - 1) % EGO_DECK_SIZE;
        else if(e->key == InputKeyDown)
            app->quest_sel = (app->quest_sel + 1) % EGO_DECK_SIZE;
        else if(e->key == InputKeyRight)
            do_postpone(app, app->quest_sel);
        else if(e->key == InputKeyLeft)
            do_load_raise(app, app->quest_sel); // взять шаг нагрузки сразу
        else if(e->key == InputKeyOk && e->type == InputTypeShort)
            start_timer(app, app->quest_sel); // у заданий с таймером ОК запускает отсчёт
        break;

    case ScreenCauses:
        if(e->key == InputKeyUp || e->key == InputKeyDown)
            app->cause_cur = (app->cause_cur + 1) % EGO_CAUSE_COUNT;
        else if(e->key == InputKeyOk && e->type == InputTypeShort)
            app->cause_mask ^= (uint16_t)(1 << app->cause_cur);
        break;

    case ScreenControl:
        if(e->key == InputKeyUp)
            app->habit_sel = (app->habit_sel + CONTROL_ROWS - 1) % CONTROL_ROWS;
        else if(e->key == InputKeyDown)
            app->habit_sel = (app->habit_sel + 1) % CONTROL_ROWS;
        else if(e->key == InputKeyOk && e->type == InputTypeShort) {
            if(app->habit_sel == CONTROL_ROW_EXIT)
                set_msg(app, "ДЕРЖИ ОК = УХОД");
            else
                do_habit(app, app->habit_sel);
        }
        break;

    case ScreenSleep:
        if(e->key == InputKeyOk && e->type == InputTypeShort)
            do_sleep_toggle(app); // «лёг»
        else if(e->key == InputKeyUp)
            do_sleep_adjust(app, 30);
        else if(e->key == InputKeyDown)
            do_sleep_adjust(app, -30);
        break;

    case ScreenProfile:
        if(e->key == InputKeyLeft)
            app->profile_page = (app->profile_page + PROFILE_PAGES - 1) % PROFILE_PAGES;
        else if(e->key == InputKeyRight)
            app->profile_page = (app->profile_page + 1) % PROFILE_PAGES;
        else if(e->key == InputKeyUp)
            app->screen = ScreenStatus;
        else if(e->key == InputKeyOk && e->type == InputTypeShort && app->profile_page == 2)
            toggle_duty(app);
        break;

    case ScreenCard:
        if(e->key == InputKeyRight) {
            if(app->card_kind == CardQuest)
                do_postpone(app, (uint8_t)app->card_slot);
            else if(app->card_kind != CardNone)
                do_check(app, false, false);
        } else if(e->key == InputKeyOk && e->type == InputTypeShort && app->card_kind == CardQuest) {
            uint8_t slot = (uint8_t)app->card_slot;
            if(ego_slot_minutes(&app->state, slot)) {
                clear_card(app);
                start_timer(app, slot);
            }
        }
        break;

    case ScreenTimer:
    case ScreenSplash:
        break;
    }
}

static void handle_tick(EgoApp* app) {
    app->tick++;
    if(app->screen == ScreenSplash) {
        if(++app->splash_tick >= EGO_SPLASH_TICKS) app->screen = ScreenStatus;
        return;
    }
    if(app->reaction != EgoReactionNone && ++app->reaction_tick >= EGO_REACTION_TICKS)
        app->reaction = EgoReactionNone;
    if(app->msg_ticks) app->msg_ticks--;
    if(app->tick % CLOCK_CHECK_TICKS == 0) sync_clock(app);
    if(app->tick % DUTY_CHECK_TICKS == 0) {
        if(app->timer_slot >= 0 && now_ts() >= app->timer_end) finish_timer(app);
        handle_duty(app);
    }
    bool keep = app->duty || app->timer_slot >= 0 || ego_sleeping(&app->state);
    if(!keep && ++app->idle_ticks >= IDLE_CLOSE_TICKS) app->running = false;
}

/* ---------- отрисовка (свой кириллический шрифт, см. ego_font.h) ---------- */

/* Шкала круга: EGO внутри текущей тысячи, отметка основания. */
static void draw_ego_bar(Canvas* c, int x, int y, int w, int h, const EgoState* s) {
    uint16_t base = ego_circle_base(s->ego);
    canvas_draw_frame(c, x, y, w, h);
    canvas_draw_box(c, x + 1, y + 1, (uint8_t)((s->ego - base) * (w - 2) / EGO_CIRCLE), h - 2);
    if(s->floor >= base) {
        int fx = x + 1 + (s->floor - base) * (w - 2) / EGO_CIRCLE;
        canvas_draw_line(c, fx, y - 1, fx, y + h); // основание
    }
}

static void draw_progress(Canvas* c, int x, int y, int w, int h, int value, int max) {
    canvas_draw_frame(c, x, y, w, h);
    if(max > 0) {
        if(value > max) value = max;
        canvas_draw_box(c, x + 1, y + 1, (uint8_t)(value * (w - 2) / max), h - 2);
    }
}

static void format_clock(char* buf, size_t n, uint32_t sec) {
    snprintf(buf, n, "%lu:%02lu", sec / 60, sec % 60);
}

static void format_hours(char* buf, size_t n, uint32_t minutes) {
    snprintf(buf, n, "%lu.%luЧ", minutes / 60, (minutes % 60) / 6);
}

static void draw_banner(Canvas* c) {
    canvas_draw_box(c, 0, 49, 128, 15);
    canvas_set_color(c, ColorWhite);
    ego_text_center(c, 64, 50, "EGO КРИТИЧЕСКИ");
    ego_text_center(c, 64, 57, "ОСЛАБЕЛО");
    canvas_set_color(c, ColorBlack);
}

static void draw_checkbox(Canvas* c, int x, int y, bool on) {
    if(on)
        canvas_draw_box(c, x, y + 1, 5, 5);
    else
        canvas_draw_frame(c, x, y + 1, 5, 5);
}

static int first_visible(int cur, int rows) {
    return cur >= rows ? cur - rows + 1 : 0;
}

static EgoLook make_look(const EgoState* s, uint8_t burn) {
    EgoLook look = {
        .contour = ego_level(s->floor),
        .density = ego_level(s->ego),
        .total = ego_level(s->ego),
        .burn = burn,
    };
    return look;
}

static void draw_status(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    bool out = ego_is_out(s);
    bool asleep = ego_sleeping(s);
    uint32_t ts = now_ts();
    uint16_t burn_v = out ? (uint16_t)(20 + s->out_burned * 4) : 0;
    EgoLook look = make_look(s, (uint8_t)(burn_v > 85 ? 85 : burn_v));
    if(asleep) look.open_force = 3; // глаз закрыт
    ego_draw_eye(c, 64, 29, &look, app->reaction, app->reaction_tick, app->tick);

    char num[32];
    snprintf(num, sizeof(num), "EGO %u", s->ego);
    ego_text(c, 2, 2, num);
    draw_ego_bar(c, 84, 3, 42, 6, s);
    snprintf(num, sizeof(num), "МИН %u/%u", ego_min_progress(s), EGO_MIN_ITEMS);
    ego_text_right(c, 126, 11, num);
    snprintf(num, sizeof(num), "СР %u", s->habit_slips);
    ego_text_right(c, 126, 20, num);

    if(out || asleep) {
        ego_text(c, 2, 11, out ? "ВНЕ EGO" : "СОН");
        format_clock(num, sizeof(num), (out ? ego_out_seconds(s, ts) : ts - s->sleep_since) / (out ? 1 : 60));
        ego_text(c, 2, 20, num);
        if(out) {
            snprintf(num, sizeof(num), "-%u", s->out_burned);
            ego_text(c, 2, 29, num);
        }
    } else {
        rank_label(num, sizeof(num), s->ego);
        ego_text(c, 2, 11, num);
        int y = 20;
        int delta = ego_day_delta(s);
        if(delta) {
            snprintf(num, sizeof(num), "%+d", delta);
            ego_text(c, 2, y, num);
            y += 9;
        }
        if(app->duty) {
            ego_text(c, 2, y, "ДЕЖ");
            y += 9;
        }
        if(ego_is_paused(s, app->day)) {
            ego_text(c, 2, y, "ПАУЗА");
            y += 9;
        }
        if(app->card_kind != CardNone && (app->tick / 10) % 2 == 0)
            ego_text(c, 2, y, app->card_kind == CardQuest ? "ЗАД!" : "ПРОВ!");
    }

    /* критический порог: баннер мигает с обычным низом экрана */
    if(s->ego < EGO_CRITICAL_BELOW && !asleep && (app->tick / BANNER_PERIOD) % 2 == 0) {
        draw_banner(c);
        return;
    }

    if(out) {
        ego_text(c, 2, 57, app->msg_ticks ? app->msg : "ДЕРЖИ ОК = ВОЗВРАТ");
        return;
    }
    if(asleep) {
        ego_text(c, 2, 57, "ОК = ВСТАЛ");
        return;
    }

    /* низ: сообщение, затем цель развития (до следующего ранга) */
    if(app->msg_ticks) {
        ego_text(c, 2, 57, app->msg);
    } else {
        char buf[32];
        uint16_t next = ego_next_boundary(s->ego);
        if(!next)
            snprintf(buf, sizeof(buf), "РАНГ S %s", ego_roman(EGO_CIRCLES));
        else if(ego_band(next) == EGO_BAND_E && next > ego_circle_base(s->ego))
            snprintf(buf, sizeof(buf), "ДО КРУГА +%u", next - s->ego);
        else
            snprintf(buf, sizeof(buf), "ДО %s РАНГА +%u", ego_band_letter(ego_band(next)), next - s->ego);
        ego_text(c, 2, 57, buf);
    }
}

/* Задания: 14 ежедневных и 3 бонуса. Минимум дня помечен «!». Видимое окно из 6 строк. */
static void draw_tasks(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char buf[48];
    snprintf(buf, sizeof(buf), "ЗАДАНИЯ %u/%u", ego_quests_today(s), EGO_CORE_QUESTS);
    ego_text(c, 0, 0, buf);
    snprintf(buf, sizeof(buf), "МИН %u/%u", ego_min_progress(s), EGO_MIN_ITEMS);
    ego_text_right(c, 127, 0, buf);

    const int rows = 6;
    int first = first_visible(app->quest_sel, rows);
    for(int i = 0; i < rows && first + i < EGO_DECK_SIZE; i++) {
        int idx = first + i, y = 9 + i * 9;
        bool done = s->deck_done & (1u << idx);
        bool dropped = s->deck_drop & (1u << idx);
        bool sel = idx == app->quest_sel;
        const EgoQuest* q = ego_slot_quest(s, (uint8_t)idx);
        if(sel) {
            canvas_draw_box(c, 0, y - 1, 128, 9);
            canvas_set_color(c, ColorWhite);
        }
        if(dropped && !done) { // снято после переносов или бонус пропущен
            canvas_draw_line(c, 2, y + 1, 6, y + 5);
            canvas_draw_line(c, 2, y + 5, 6, y + 1);
        } else {
            draw_checkbox(c, 2, y, done);
        }
        if(q->min_bit >= 0) ego_text(c, 9, y, "!");
        quest_label(s, (uint8_t)idx, buf, sizeof(buf));
        ego_text_n(c, 15, y, buf, 16);
        snprintf(buf, sizeof(buf), "%s%d", ego_slot_is_bonus((uint8_t)idx) ? "Б+" : "+", q->points);
        ego_text_right(c, 126, y, buf);
        canvas_set_color(c, ColorBlack);
    }
}

/* Причины ухода после возврата: анализ и выход из EGO, каждая со штрафом. Назад = без причины. */
static void draw_causes(Canvas* c, EgoApp* app) {
    char buf[32];
    ego_text(c, 0, 0, "ПРИЧИНЫ: НАЗАД=БЕЗ");

    int selected = 0;
    for(int i = 0; i < EGO_CAUSE_COUNT; i++) {
        int y = 14 + i * 12;
        bool sel = i == app->cause_cur, on = app->cause_mask & (1 << i);
        if(on) selected++;
        if(sel) {
            canvas_draw_box(c, 0, y - 2, 128, 11);
            canvas_set_color(c, ColorWhite);
        }
        draw_checkbox(c, 4, y, on);
        ego_text(c, 14, y, ego_cause_label((uint8_t)i));
        canvas_set_color(c, ColorBlack);
    }
    snprintf(buf, sizeof(buf), "ИТОГО -%d", selected * ego_scaled(app->state.ego, EGO_CAUSE_COST));
    ego_text(c, 2, 57, buf);
    ego_text_right(c, 126, 57, "ДЕРЖИ ОК");
}

/* Деньги: шкала копилки и четыре строки; ОК делает то, что написано в строке. */
static void draw_money(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char buf[40];
    ego_text(c, 0, 0, "ДЕНЬГИ");
    snprintf(buf, sizeof(buf), "КОПИЛКА %uК/%uК", s->savings_k, EGO_SAVINGS_GOAL);
    ego_text_right(c, 127, 0, buf);
    draw_progress(c, 0, 9, 128, 6, s->savings_k, EGO_SAVINGS_GOAL);

    const char* labels[MONEY_ROWS] = {"ПОПОЛНИЛ", "СНЯЛ", "ШАГ", "ОСОБЫЙ ДЕНЬ"};
    char rights[MONEY_ROWS][16];
    snprintf(rights[MONEY_ROW_ADD], sizeof(rights[0]), "+%uК", app->savings_step);
    snprintf(rights[MONEY_ROW_TAKE], sizeof(rights[0]), "-%uК", app->savings_step);
    snprintf(rights[MONEY_ROW_STEP], sizeof(rights[0]), "%uК", app->savings_step);
    snprintf(rights[MONEY_ROW_DAUGHTER], sizeof(rights[0]), "%s", ego_daughter_day(s, app->day) ? "ДА" : "НЕТ");
    for(int i = 0; i < MONEY_ROWS; i++) {
        int y = 18 + i * 9;
        if(i == app->money_sel) {
            canvas_draw_box(c, 0, y - 1, 128, 9);
            canvas_set_color(c, ColorWhite);
        }
        ego_text(c, 3, y, labels[i]);
        ego_text_right(c, 125, y, rights[i]);
        canvas_set_color(c, ColorBlack);
    }
    static const char* const hints[MONEY_ROWS] = {
        "ОК = ПОЛОЖИЛ В КОПИЛКУ",
        "ОК = ВЗЯЛ ИЗ КОПИЛКИ",
        "ОК = ШАГ 1..5К",
        "ОК = ДА / НЕТ",
    };
    ego_text(c, 2, 56, app->msg_ticks ? app->msg : hints[app->money_sel]);
}

/* Контроль: привычки и выход из EGO (таймер) в одном модуле. Сообщение внизу - подсказка по строке. */
static void draw_control(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char buf[40];
    ego_text(c, 0, 0, "КОНТРОЛЬ");
    snprintf(buf, sizeof(buf), "ВЧЕРА СР %u", s->habit_prev_slips);
    ego_text_right(c, 127, 0, buf);

    const char* labels[CONTROL_ROWS] = {"ЗАМЕТИЛ +1", "УДЕРЖАЛСЯ +3", "СРЫВ", "ВЫХОД ИЗ EGO"};
    char rights[CONTROL_ROWS][20];
    snprintf(rights[0], sizeof(rights[0]), "%u/%u", s->habit_noticed, EGO_HABIT_NOTICE_MAX);
    snprintf(rights[1], sizeof(rights[1]), "%u/%u", s->habit_held, EGO_HABIT_HOLD_MAX);
    snprintf(rights[2], sizeof(rights[2]), "-%d  %u", ego_scaled(s->ego, EGO_HABIT_SLIP), s->habit_slips);
    snprintf(rights[3], sizeof(rights[3]), "ТАЙМЕР");
    for(int i = 0; i < CONTROL_ROWS; i++) {
        int y = 10 + i * 9;
        if(i == app->habit_sel) {
            canvas_draw_box(c, 0, y - 1, 128, 9);
            canvas_set_color(c, ColorWhite);
        }
        ego_text(c, 3, y, labels[i]);
        ego_text_right(c, 125, y, rights[i]);
        canvas_set_color(c, ColorBlack);
    }

    if(app->msg_ticks) {
        ego_text(c, 2, 49, app->msg);
    } else if(app->habit_sel == CONTROL_ROW_EXIT) {
        ego_text(c, 2, 49, "ДЕРЖИ ОК = УХОД");
        ego_text(c, 2, 57, "ТАЙМЕР ВНЕ EGO");
    } else if(app->habit_sel == CONTROL_ROW_SLIP) {
        ego_text(c, 2, 49, "ОК = ЗАПИСАТЬ СРЫВ");
        ego_text(c, 2, 57, "ДЕРЖИ = СРЫВ И УХОД");
    } else {
        ego_text(c, 2, 49, "ОК = ЗАПИСАТЬ");
        ego_text(c, 2, 57, "ЛИМИТ В СУТКИ");
    }
}

/* Сон: часы за 24 часа складываются из кусков. */
static void draw_sleep(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char hrs[16];
    uint32_t ts = now_ts();
    uint16_t minutes = ego_sleep_minutes(s, ts);

    ego_text(c, 0, 0, "СОН");
    ego_text_right(c, 127, 0, ego_sleeping(s) ? "СПИТ" : "БОДРСТВУЕТ");

    format_hours(hrs, sizeof(hrs), minutes);
    char big[48];
    snprintf(big, sizeof(big), "%s ЗА 24Ч", hrs);
    ego_text(c, 2, 11, big);
    draw_progress(c, 2, 21, 124, 6, minutes, 480); // 8 часов
    canvas_draw_line(c, 2 + 1 + 420 * 122 / 480, 20, 2 + 1 + 420 * 122 / 480, 27); // отметка 7 часов

    ego_text(c, 2, 30, "ОК = ЛЁГ");
    ego_text(c, 2, 39, "ПОПРАВКА ВВЕРХ/ВНИЗ");

    for(int i = 0; i < EGO_SLEEP_HIST; i++) { // сон по суткам, до 8 часов
        int h = s->sleep_hist[i] * 11 / 80;
        if(h > 11) h = 11;
        canvas_draw_box(c, 2 + i * 10, 62 - h, 7, h ? h : 1);
    }
    ego_text_right(c, 126, 50, "7 ДНЕЙ");
    ego_text_right(c, 126, 57, "7Ч=8 6Ч=5");
}

/* Профиль, страница 0: логотип смотрит прямо, ранг, круг, пик. */
static void draw_profile_logo(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char buf[48];
    EgoLook look = make_look(s, 0);
    look.flags = EGO_LOOK_NO_MARK | EGO_LOOK_GAZE; // смотрит прямо
    look.gaze_x = 0;
    look.gaze_y = 0;
    ego_draw_eye(c, 64, 22, &look, app->reaction, app->reaction_tick, app->tick);

    snprintf(buf, sizeof(buf), "EGO %u", s->ego);
    ego_text(c, 2, 2, buf);
    draw_ego_bar(c, 84, 3, 42, 6, s);

    snprintf(buf, sizeof(buf), "%s (%s РАНГ)", ego_band_name(ego_band(s->ego)), ego_band_letter(ego_band(s->ego)));
    ego_text_center(c, 64, 38, buf);
    uint8_t degree = ego_peak_degree(s);
    if(degree)
        snprintf(buf, sizeof(buf), "КРУГ %u/%u  ПИК S %s", ego_circle(s->ego), EGO_CIRCLES, ego_roman(degree));
    else
        snprintf(buf, sizeof(buf), "КРУГ %u/%u  ПИК -", ego_circle(s->ego), EGO_CIRCLES);
    ego_text_center(c, 64, 47, buf);
    uint16_t next = ego_next_boundary(s->ego);
    uint16_t m = ego_penalty_mult(s->ego);
    if(next)
        snprintf(buf, sizeof(buf), "ДО СЛЕД. +%u  ШТРАФ Х%u.%u", next - s->ego, m / 100, (m % 100) / 10);
    else
        snprintf(buf, sizeof(buf), "ВЫСШИЙ РАНГ");
    ego_text_center(c, 64, 56, buf);
}

/* Профиль, страница 1: направления героя и копилка. */
static void draw_profile_hero(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char buf[40];
    for(int i = 0; i < EGO_PILLARS; i++) {
        int y = 1 + i * 8;
        uint16_t count = s->pillar_count[i];
        ego_text(c, 2, y, ego_pillar_name((uint8_t)i));
        snprintf(buf, sizeof(buf), "УР %u", count / EGO_PILLAR_STEP);
        ego_text(c, 50, y, buf);
        draw_progress(c, 82, y + 1, 44, 5, count % EGO_PILLAR_STEP, EGO_PILLAR_STEP);
    }
    snprintf(buf, sizeof(buf), "КОПИЛКА %uК/%uК", s->savings_k, EGO_SAVINGS_GOAL);
    ego_text(c, 2, 43, buf);
    draw_progress(c, 2, 53, 124, 7, s->savings_k, EGO_SAVINGS_GOAL);
}

/* Профиль, страница 2: данные, режимы. */
static void draw_profile_data(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    char buf[64];

    snprintf(
        buf,
        sizeof(buf),
        "ПОТОК %u/%u EGO %u/%u",
        s->check_ok[EGO_CHECK_FLOW],
        s->check_ok[EGO_CHECK_FLOW] + s->check_fail[EGO_CHECK_FLOW],
        s->check_ok[EGO_CHECK_EGO],
        s->check_ok[EGO_CHECK_EGO] + s->check_fail[EGO_CHECK_EGO]);
    ego_text(c, 2, 1, buf);

    if(s->returns)
        snprintf(buf, sizeof(buf), "ВОЗВРАТОВ %u СР %luМ", s->returns, s->return_sec / s->returns / 60);
    else
        snprintf(buf, sizeof(buf), "ВОЗВРАТОВ 0");
    ego_text(c, 2, 10, buf);

    snprintf(buf, sizeof(buf), "ПРИВ З%u У%u С%u", s->habit_total_noticed, s->habit_total_held, s->habit_total_slips);
    ego_text(c, 2, 19, buf);

    uint32_t sum = 0;
    for(int i = 0; i < EGO_SLEEP_HIST; i++) sum += s->sleep_hist[i];
    char h24[16], h7[16];
    format_hours(h24, sizeof(h24), ego_sleep_minutes(s, now_ts()));
    format_hours(h7, sizeof(h7), sum * 6 / EGO_SLEEP_HIST);
    snprintf(buf, sizeof(buf), "СОН %s  7Д СР %s", h24, h7);
    ego_text(c, 2, 28, buf);

    ego_text(c, 2, 37, app->duty ? "ДЕЖУРСТВО: ВКЛ" : "ДЕЖУРСТВО: ВЫКЛ");
    ego_text_right(c, 126, 37, "ОК");
    ego_text(c, 2, 46, ego_is_paused(s, app->day) ? "ПАУЗА: СЕГОДНЯ" : "ПАУЗА: НЕТ");
    ego_text_right(c, 126, 46, "ДЕРЖИ ОК");
    ego_text(c, 2, 55, ego_daughter_day(s, app->day) ? "ОСОБЫЙ ДЕНЬ: ДА" : "ОСОБЫЙ ДЕНЬ: НЕТ");
    ego_text_right(c, 126, 55, "ВЛЕВО");
}

/* Профиль, страница 3: динамика внутри текущего круга, прирост и слив. */
static void draw_profile_dynamics(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    const int base_y = 40, height = 28, bar_w = 7, step = 8, x0 = 4;
    static const uint16_t thresholds[4] = {300, 500, 700, 900};
    uint16_t base = ego_circle_base(s->ego);

    for(int t = 0; t < 4; t++) { // пунктир на границах рангов круга
        int y = base_y - thresholds[t] * height / EGO_CIRCLE;
        for(int x = 0; x < 128; x += 4) canvas_draw_dot(c, x, y);
    }
    canvas_draw_line(c, 0, base_y + 1, 127, base_y + 1);

    int n = s->hist_n;
    for(int i = 0; i <= n; i++) { // последний столбец - сегодня
        int total = (i < n) ? s->hist[i].total : s->ego;
        int rel = total - base;
        if(rel < 0) rel = 0;
        if(rel > EGO_CIRCLE) rel = EGO_CIRCLE;
        int h = rel * height / EGO_CIRCLE;
        int x = x0 + (EGO_HISTORY_DAYS - n + i) * step;
        if(i < n)
            canvas_draw_box(c, x, base_y - h, bar_w, h ? h : 1);
        else
            canvas_draw_frame(c, x, base_y - h, bar_w, h ? h + 1 : 1);
    }

    uint16_t g1, l1, g7, l7;
    char buf[40];
    ego_sum(s, 1, &g1, &l1);
    ego_sum(s, 7, &g7, &l7);
    snprintf(buf, sizeof(buf), "СЕГОДНЯ +%u -%u", g1, l1);
    ego_text(c, 2, 46, buf);
    snprintf(buf, sizeof(buf), "7 ДНЕЙ +%u -%u", g7, l7);
    ego_text(c, 2, 55, buf);
}

static void draw_profile(Canvas* c, EgoApp* app) {
    switch(app->profile_page) {
    case 0:
        draw_profile_logo(c, app);
        break;
    case 1:
        draw_profile_hero(c, app);
        break;
    case 2:
        draw_profile_data(c, app);
        break;
    default:
        draw_profile_dynamics(c, app);
        break;
    }
    for(int i = 0; i < PROFILE_PAGES; i++) { // точки страниц
        if(i == app->profile_page)
            canvas_draw_disc(c, 104 + i * 7, 62, 1);
        else
            canvas_draw_dot(c, 104 + i * 7, 62);
    }
}

/* Карточка дежурного режима: задание или проверка. */
static void draw_card(Canvas* c, EgoApp* app) {
    const EgoState* s = &app->state;
    if(app->card_kind == CardNone) return;
    char buf[48];

    ego_text(c, 2, 2, app->card_kind == CardQuest ? "ЗАДАНИЕ" : "ПРОВЕРКА");
    uint32_t now = now_ts();
    uint32_t left = app->card_deadline > now ? (app->card_deadline - now) / 60 : 0;
    snprintf(buf, sizeof(buf), "ЕЩЁ %luМ", left);
    ego_text_right(c, 126, 2, buf);
    canvas_draw_line(c, 0, 11, 127, 11);

    if(app->card_kind == CardQuest) {
        uint8_t slot = (uint8_t)app->card_slot;
        bool bonus = ego_slot_is_bonus(slot);
        uint8_t minutes = ego_slot_minutes(s, slot);
        quest_label(s, slot, buf, sizeof(buf));
        ego_text(c, 2, 15, buf);
        snprintf(buf, sizeof(buf), "%s+%d EGO", bonus ? "БОНУС " : "", ego_slot_quest(s, slot)->points);
        ego_text(c, 2, 24, buf);

        int y = 35;
        if(minutes) {
            snprintf(buf, sizeof(buf), "ОК = ТАЙМЕР %uМ", minutes);
            ego_text(c, 2, y, buf);
            y += 9;
        }
        ego_text(c, 2, y, "ДЕРЖИ ОК = СДЕЛАНО");
        y += 9;
        if(bonus)
            ego_text(c, 2, y, "ВПРАВО: ПРОПУСК");
        else
            ego_text(c, 2, y, ego_postpone_cost(s, slot) ? "ВПРАВО: ПЕРЕНОС -0.5" : "ВПРАВО: ПЕРЕНОС 0");
        if(!minutes) ego_text(c, 2, 57, "НАЗАД = СВЕРНУТЬ");
    } else {
        if(app->card_kind == CardFlow) {
            ego_text(c, 2, 16, "ПОТОКОВОСТЬ");
            ego_text(c, 2, 26, "БЕЗ ОЦЕНКИ ДЕЙСТВИЙ?");
        } else {
            ego_text(c, 2, 16, "БЫТЬ В EGO");
            ego_text(c, 2, 26, "Я СЕЙЧАС В EGO?");
        }
        snprintf(buf, sizeof(buf), "ДЕРЖИ ОК = ДА +%d", EGO_CHECK_OK);
        ego_text(c, 2, 38, buf);
        snprintf(buf, sizeof(buf), "ВПРАВО = ВЫЛЕТ -%d", ego_scaled(s->ego, EGO_CHECK_FAIL));
        ego_text(c, 2, 47, buf);
        snprintf(buf, sizeof(buf), "БЕЗ ОТВЕТА = -%d", ego_scaled(s->ego, EGO_CHECK_FAIL));
        ego_text(c, 2, 57, buf);
    }
}

/* Фокус-таймер: крупный отсчёт и полоса прогресса. */
static void draw_timer(Canvas* c, EgoApp* app) {
    char buf[40];
    uint32_t now = now_ts();
    uint32_t left = app->timer_end > now ? app->timer_end - now : 0;

    quest_label(&app->state, (uint8_t)app->timer_slot, buf, sizeof(buf));
    ego_text_center(c, 64, 2, buf);

    format_clock(buf, sizeof(buf), left);
    ego_text_scaled(c, 64 - ego_text_scaled_width(buf, 3) / 2, 18, buf, 3);

    uint32_t elapsed = app->timer_total > left ? app->timer_total - left : 0;
    draw_progress(c, 4, 46, 120, 7, app->timer_total ? (int)(elapsed * 120 / app->timer_total) : 120, 120);
    ego_text_center(c, 64, 57, "НАЗАД = ОТМЕНА");
}

static void draw_callback(Canvas* canvas, void* ctx) {
    EgoApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    switch(app->screen) {
    case ScreenSplash:
        ego_draw_splash(canvas, app->splash_tick);
        break;
    case ScreenStatus:
        draw_status(canvas, app);
        break;
    case ScreenTasks:
        draw_tasks(canvas, app);
        break;
    case ScreenCauses:
        draw_causes(canvas, app);
        break;
    case ScreenControl:
        draw_control(canvas, app);
        break;
    case ScreenMoney:
        draw_money(canvas, app);
        break;
    case ScreenSleep:
        draw_sleep(canvas, app);
        break;
    case ScreenProfile:
        draw_profile(canvas, app);
        break;
    case ScreenCard:
        draw_card(canvas, app);
        break;
    case ScreenTimer:
        draw_timer(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

/* ---------- каркас приложения ---------- */

static void input_callback(InputEvent* input, void* ctx) {
    EgoApp* app = ctx;
    EgoEvent ev = {.type = EventInput, .input = *input};
    furi_message_queue_put(app->queue, &ev, 0);
}

static void timer_callback(void* ctx) {
    EgoApp* app = ctx;
    EgoEvent ev = {.type = EventTick};
    furi_message_queue_put(app->queue, &ev, 0);
}

int32_t ego_app(void* p) {
    UNUSED(p);

    EgoApp* app = malloc(sizeof(EgoApp));
    memset(app, 0, sizeof(EgoApp));
    app->running = true;
    app->screen = ScreenSplash;
    app->card_slot = -1;
    app->timer_slot = -1;
    app->savings_step = 1;
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(EgoEvent));

    ego_state_init(&app->state);
    ego_storage_load(&app->state);
    app->prev_rank = ego_rank_index(app->state.ego);
    app->prev_degree = ego_peak_degree(&app->state);
    sync_clock(app); // сутки, закрытые пока приложение было закрыто, и сожжённое за это время

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(TICK_MS));

    EgoEvent ev;
    while(app->running) {
        if(furi_message_queue_get(app->queue, &ev, FuriWaitForever) != FuriStatusOk) continue;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(ev.type == EventInput)
            handle_input(app, &ev.input);
        else
            handle_tick(app);
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
