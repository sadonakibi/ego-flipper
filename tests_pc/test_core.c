/* Сборка на ПК (из папки tests_pc):
 *   gcc -Wall -Wextra -I../ego_tamagotchi ../ego_tamagotchi/ego_core.c ../ego_tamagotchi/ego_data.c test_core.c -o test_core && ./test_core */
#include "ego_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define T0 1000000UL
#define H 3600UL

/* Чистое состояние с EGO 600 (ранг B, круг 1, множитель штрафов x1.0), основание 300. */
static void fresh(EgoState* s) {
    ego_state_init(s);
    s->ego = 600;
    s->floor = 300;
    s->peak = 600;
    s->day_start_total = 600;
}

/* ego_update без штрафа за минимум (все пункты закрыты) - чтобы не смешивать проверки. */
static uint8_t upd(EgoState* s, uint32_t day) {
    if(s->day) s->min_done = 0xFF;
    return ego_update(s, day);
}

int main(void) {
    EgoState s;
    ego_state_init(&s);
    assert(s.ego == 300 && s.floor == 150 && s.peak == 300 && s.savings_k == 0);
    assert(s.load[4] == 30 && s.load[5] == 25); /* отжимания 30, приседания 25 */

    /* --- ранги, круги, границы --- */
    assert(ego_circle(0) == 1 && ego_circle(999) == 1 && ego_circle(1000) == 2 && ego_circle(11000) == 11);
    static const uint16_t starts[6] = {0, 150, 300, 500, 700, 900};
    for(int b = 0; b < 6; b++) {
        assert(ego_band(starts[b]) == b && ego_band(starts[b] + 1000) == b);
        assert(ego_band((uint16_t)(starts[b] == 0 ? 0 : starts[b] - 1)) == (b ? b - 1 : 0));
    }
    assert(ego_band(11000) == 5 && ego_rank_index(0) == 0 && ego_rank_index(1000) == 6);
    assert(ego_rank_index(11000) == 65);
    assert(ego_next_boundary(0) == 150 && ego_next_boundary(299) == 300 && ego_next_boundary(300) == 500);
    assert(ego_next_boundary(899) == 900 && ego_next_boundary(999) == 1000 && ego_next_boundary(1000) == 1150);
    assert(ego_next_boundary(11000) == 0);
    assert(ego_level(0) == EgoLevelCritical && ego_level(150) == EgoLevelWeak && ego_level(500) == EgoLevelStable);
    assert(ego_level(700) == EgoLevelStrong && ego_level(1000) == EgoLevelStable);
    s.peak = 2500;
    assert(ego_peak_degree(&s) == 2);

    /* --- множитель штрафов: +25% за круг, полоса от 0.5 до 1.5 --- */
    assert(ego_penalty_mult(0) == 50 && ego_penalty_mult(300) == 75 && ego_penalty_mult(500) == 100);
    assert(ego_penalty_mult(700) == 125 && ego_penalty_mult(900) == 150);
    assert(ego_penalty_mult(2700) == 187 && ego_penalty_mult(11000) == 525);
    assert(ego_scaled(500, 4) == 4 && ego_scaled(700, 4) == 5 && ego_scaled(2700, 4) == 7);
    assert(ego_scaled(0, 1) == 1 && ego_scaled(11000, 20) == 105);

    /* --- время: границы суток и окна от подъёма --- */
    uint8_t w;
    uint32_t d = ego_clock(100 * 86400UL + 5 * 3600UL, &w);
    assert(w == 0);
    assert(ego_clock(100 * 86400UL + 11 * 3600UL, &w) == d && w == 1);
    assert(ego_clock(100 * 86400UL + 17 * 3600UL, &w) == d && w == 2);
    assert(ego_clock(101 * 86400UL + 5 * 3600UL, &w) == d + 1 && w == 0);

    /* --- пул: очки, таймеры, минимум дня --- */
    assert(ego_quest(0)->points == 3 && ego_quest(6)->points == 11);
    assert(ego_quest(6)->load_kind == EGO_LOAD_MINUTES && ego_quest(6)->base_load == 45);
    assert(ego_quest(1)->base_load == 20 && ego_quest(10)->base_load == 30 && ego_quest(11)->base_load == 15);
    assert(ego_quest(4)->load_kind == EGO_LOAD_REPS && ego_quest(5)->load_kind == EGO_LOAD_REPS);
    assert(ego_quest(EGO_DAUGHTER_QUEST)->points == 6);
    int mask = 0, sport_items = 0;
    for(int i = 0; i < EGO_CORE_QUESTS; i++) {
        if(ego_quest((uint8_t)i)->min_bit >= 0) mask |= 1 << ego_quest((uint8_t)i)->min_bit;
        if(ego_quest((uint8_t)i)->min_bit == 3) sport_items++;
    }
    assert(mask == 0x7F && sport_items == 2); /* седьмой бит и сон: восьмой пункт минимума */
    assert(strcmp(ego_cause_label(0), "Анализ") == 0 && strcmp(ego_cause_label(1), "Выход из EGO") == 0);

    /* --- первый запуск: колода, ядро раз в сутки, минимум --- */
    fresh(&s);
    assert(upd(&s, d) == 0);
    for(int i = 0; i < EGO_CORE_QUESTS; i++) assert(s.deck[i] == i);
    for(int k = 0; k < EGO_BONUS_SLOTS; k++)
        assert(s.deck[EGO_CORE_QUESTS + k] >= EGO_BONUS_FIRST && s.deck[EGO_CORE_QUESTS + k] < EGO_POOL_COUNT);
    assert(s.deck[EGO_CORE_QUESTS] != s.deck[EGO_CORE_QUESTS + 1] && s.deck[EGO_CORE_QUESTS + 1] != s.deck[EGO_CORE_QUESTS + 2] && s.min_done == 0);
    assert(ego_quest_done(&s, 0) == EgoReactionGrowth && s.ego == 603); /* Подъём +3 */
    assert(ego_quest_done(&s, 0) == EgoReactionNone && s.ego == 603);
    assert(ego_quest_done(&s, 6) == EgoReactionGrowth && s.ego == 614); /* Код +11 */
    assert(s.min_done == 0 && ego_quests_today(&s) == 2); /* код в минимум не входит */
    ego_quest_done(&s, 3); /* Завтрак */
    ego_quest_done(&s, 4); /* Отжимания: спорт закрыт только вместе с приседаниями */
    assert(s.min_done == (1 << 0));
    ego_quest_done(&s, 5); /* Приседания */
    assert(s.min_done == ((1 << 0) | (1 << 3)) && ego_min_progress(&s) == 2);
    assert(s.pillar_count[1] == 1 && s.pillar_count[0] == 4); /* код: дела; подъём, завтрак, отжимания, приседания: тело */

    /* --- минимум дня: незакрытые пункты штрафуются вдвое строже окна --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_update(&s, d + 1) == 8 && s.ego == 536); /* 8 пунктов x 8 */
    assert(s.min_done == 0);
    fresh(&s);
    upd(&s, d);
    s.min_done = 0x7F; /* закрыто 7 из 8, не хватает сна */
    assert(ego_update(&s, d + 1) == 1 && s.ego == 592);
    assert(s.floor > 300); /* закрыто >= 5: основание подтягивается */

    /* пауза: сутки без штрафа */
    fresh(&s);
    upd(&s, d);
    ego_set_pause(&s, d, true);
    assert(ego_is_paused(&s, d) && !ego_is_paused(&s, d + 1));
    assert(ego_update(&s, d + 1) == 0 && s.ego == 600);
    assert(ego_update(&s, d + 2) == 8 && s.ego == 536);

    /* штраф пробивает основание */
    fresh(&s);
    upd(&s, d);
    s.ego = 301;
    s.floor = 300;
    s.min_done = 0x7F;
    assert(ego_update(&s, d + 1) == 1); /* ранг C: x0.75, штраф 6 */
    assert(s.ego == 295 && s.floor == 295);

    /* основание подтягивается к EGO (до 8 за круг в сутки); история суток */
    fresh(&s);
    upd(&s, d);
    ego_quest_done(&s, 6);
    assert(s.ego == 611);
    assert(upd(&s, d + 1) == 0 && s.floor == 308);
    assert(s.hist_n == 1 && s.hist[0].total == 611 && s.hist[0].gain == 11 && s.hist[0].loss == 0);
    assert(ego_day_delta(&s) == 0);
    uint16_t gain, loss;
    ego_sum(&s, 7, &gain, &loss);
    assert(gain == 11 && loss == 0);

    /* --- сон: куски за 24 часа складываются --- */
    fresh(&s);
    upd(&s, d);
    assert(!ego_sleep_toggle(&s, T0, d) && ego_sleeping(&s)); /* лёг */
    assert(ego_sleep_minutes(&s, T0 + H) == 60); /* текущий кусок считается */
    assert(ego_sleep_toggle(&s, T0 + 7 * H, d) && !ego_sleeping(&s)); /* встал: 7 часов */
    assert(ego_sleep_minutes(&s, T0 + 7 * H) == 420);
    assert(s.ego == 611); /* 7ч = 8 очков + Подъём 3 */
    assert((s.min_done & (1 << EGO_MIN_SLEEP)) && (s.deck_done & 1u) && s.wake_ts == T0 + 7 * H);

    fresh(&s); /* рваная ночь: 3 часа + 3 часа */
    upd(&s, d);
    ego_sleep_toggle(&s, T0, d);
    ego_sleep_toggle(&s, T0 + 3 * H, d);
    assert(ego_sleep_minutes(&s, T0 + 3 * H) == 180 && (s.min_done & (1 << EGO_MIN_SLEEP)) && s.ego == 603);
    ego_sleep_toggle(&s, T0 + 4 * H, d);
    ego_sleep_toggle(&s, T0 + 7 * H, d);
    assert(ego_sleep_minutes(&s, T0 + 7 * H) == 360 && s.ego == 608); /* 6 часов: +5 */
    ego_sleep_adjust(&s, -60, T0 + 7 * H); /* поправка вниз очков не отбирает */
    assert(ego_sleep_minutes(&s, T0 + 7 * H) == 300 && s.ego == 608);
    assert(ego_sleep_minutes(&s, T0 + 7 * H + 25 * H) == 0); /* через 24 часа куски устаревают */

    /* окна дня считаются от подъёма */
    s.wake_ts = T0;
    s.wake_day = d;
    s.sleep_since = 0;
    assert(ego_window(&s, T0 + H, d, 2) == 0 && ego_window(&s, T0 + 7 * H, d, 2) == 1);
    assert(ego_window(&s, T0 + 13 * H, d, 0) == 2 && ego_window(&s, T0 + H, d + 1, 1) == 1);

    /* --- привычки: заметил +1, удержался +3, срыв, бонус за сокращение --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_habit_notice(&s, T0) == EgoHabitOk && s.ego == 601 && s.habit_noticed == 1);
    assert(ego_habit_notice(&s, T0 + 10) == EgoHabitTooSoon); /* раньше 10 минут */
    for(int i = 1; i < EGO_HABIT_NOTICE_MAX; i++) assert(ego_habit_notice(&s, T0 + i * 600UL) == EgoHabitOk);
    assert(ego_habit_notice(&s, T0 + 20000) == EgoHabitMax && s.ego == 610);
    assert(ego_habit_hold(&s, T0) == EgoHabitOk && s.ego == 613 && s.habit_held == 1);
    assert(ego_habit_slip(&s) == 5 && s.ego == 608 && s.habit_slips == 1);

    fresh(&s);
    upd(&s, d);
    for(int i = 0; i < 3; i++) ego_habit_slip(&s); /* вчера 3 срыва */
    assert(s.ego == 585);
    upd(&s, d + 1);
    assert(s.habit_prev_slips == 3 && s.habit_slips == 0);
    ego_habit_slip(&s); /* сегодня 1: меньше на 2 */
    assert(s.ego == 580);
    upd(&s, d + 2);
    assert(s.habit_bonus_notice == 4 && s.ego == 584 && s.slips_hist[EGO_SLEEP_HIST - 1] == 1);

    /* --- копилка: шаг 1..5К, первое пополнение дня даёт очки --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_savings_add(&s, 3, d) == 5 && s.savings_k == 3 && s.ego == 605);
    assert(ego_savings_add(&s, 5, d) == 0 && s.savings_k == 8 && s.ego == 605);
    assert(ego_savings_add(&s, -5, d) == 0 && s.savings_k == 3);
    assert(ego_savings_add(&s, -999, d) == 0 && s.savings_k == 0);
    assert(ego_savings_add(&s, 5, d + 1) == 5);

    /* --- особые дни: раз в 3 дня --- */
    fresh(&s);
    upd(&s, d);
    assert(!ego_daughter_day(&s, d));
    assert(ego_daughter_toggle(&s, d) && s.deck[EGO_CORE_QUESTS] == EGO_DAUGHTER_QUEST && ego_daughter_day(&s, d));
    assert(!ego_daughter_day(&s, d + 1) && !ego_daughter_day(&s, d + 2) && ego_daughter_day(&s, d + 3));
    upd(&s, d + 1);
    assert(s.deck[EGO_CORE_QUESTS] != EGO_DAUGHTER_QUEST);
    upd(&s, d + 3);
    assert(s.deck[EGO_CORE_QUESTS] == EGO_DAUGHTER_QUEST);
    assert(!ego_daughter_toggle(&s, d + 3) && s.daughter_anchor == 0 && s.deck[EGO_CORE_QUESTS] != EGO_DAUGHTER_QUEST);

    /* --- уход: без удара и причин; -2 каждые 5 минут; множитель на момент ухода --- */
    fresh(&s);
    upd(&s, d);
    ego_depart(&s, T0);
    assert(ego_is_out(&s) && s.ego == 600 && s.out_mult == 100);
    ego_depart(&s, T0 + 100);
    assert(s.out_since == T0);
    assert(ego_burn(&s, T0 + 299) == 0);
    assert(ego_burn(&s, T0 + 300) == 2 && s.ego == 598);
    assert(ego_burn(&s, T0 + 600) == 2 && s.ego == 596 && s.out_burned == 4);

    /* возврат через 10 минут: половина сгоревшего (2) + бонус 1 */
    assert(ego_return(&s, T0 + 600) == 3);
    assert(!ego_is_out(&s) && s.ego == 599 && s.returns == 1);
    assert(ego_return(&s, T0 + 700) == -1);

    /* причины после возврата: анализ и выход из EGO, каждая со штрафом */
    assert(ego_cause(&s, 0) == EgoReactionPenalty && ego_cause(&s, 1) == EgoReactionPenalty);
    assert(s.ego == 597 && s.cause_counts[0] == 1 && s.cause_counts[1] == 1);
    assert(ego_cause(&s, 5) == EgoReactionNone);

    /* быстрый возврат (до 5 минут): ничего не сгорело, +1 */
    fresh(&s);
    upd(&s, d);
    ego_depart(&s, T0);
    assert(ego_return(&s, T0 + 200) == 1 && s.ego == 601);

    /* после 30 минут сжигание ускоряется; потолок эпизода */
    fresh(&s);
    upd(&s, d);
    ego_depart(&s, T0);
    assert(ego_burn(&s, T0 + 1800) == 12 && s.ego == 588);
    assert(ego_burn(&s, T0 + 2100) == 5 && s.ego == 583);
    ego_burn(&s, T0 + 100000UL);
    assert(s.ego == 575 && s.out_burned == EGO_BURN_CAP);

    /* ниже основания не горит */
    fresh(&s);
    upd(&s, d);
    s.ego = 310;
    s.floor = 300;
    ego_depart(&s, T0);
    ego_burn(&s, T0 + 100000UL);
    assert(s.ego == 300 && s.out_burned == 10);

    /* штрафы растут с рангом и кругом: сжигание на ранге A круга 3 (x1.87) */
    fresh(&s);
    upd(&s, d);
    s.ego = 2700;
    s.floor = 2000;
    ego_depart(&s, T0);
    assert(s.out_mult == 187);
    assert(ego_burn(&s, T0 + 300) == 4 && s.ego == 2696);

    /* потолок шкалы: 11000, степень S = 11; пик не падает */
    fresh(&s);
    upd(&s, d);
    s.ego = 10990;
    s.floor = 10000;
    for(int i = 0; i < 6; i++) ego_check(&s, EGO_CHECK_FLOW, true);
    assert(s.ego == EGO_MAX && s.peak == EGO_MAX && ego_peak_degree(&s) == 11);
    s.ego = 9000;
    assert(s.peak == EGO_MAX && ego_peak_degree(&s) == 11);

    /* --- нагрузка: шаг за каждые 2 выполненных дня; два пропуска подряд - откат --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_quest_done(&s, 4) == EgoReactionGrowth && s.load_prog[4] == 1);
    upd(&s, d + 1);
    assert(s.deck_done == 0 && s.quest_count[4] == 1);
    assert(ego_quest_done(&s, 4) == EgoReactionGrowth);
    assert(s.load[4] == 31 && s.load_prog[4] == 0);
    assert(ego_slot_reps(&s, 4) == 31 && ego_slot_reps(&s, 6) == 0);
    fresh(&s);
    upd(&s, d);
    s.load[4] = 35;
    upd(&s, d + 3);
    assert(s.load[4] == 34 && s.load_miss[4] == 1);
    fresh(&s);
    upd(&s, d);
    s.load[4] = 30;
    upd(&s, d + 2);
    upd(&s, d + 4);
    assert(s.load[4] == 30); /* ниже стартовой нагрузки не падает */

    /* взять нагрузку сразу: шаг, потолок, не после выполнения */
    fresh(&s);
    upd(&s, d);
    assert(ego_load_raise(&s, 4) && s.load[4] == 31);
    assert(ego_load_raise(&s, 6) && s.load[6] == 47); /* код: шаг 2 минуты */
    s.load[4] = 100;
    assert(!ego_load_raise(&s, 4)); /* потолок */
    assert(!ego_load_raise(&s, 0)); /* у «Подъёма» нагрузки нет */
    ego_quest_done(&s, 5);
    assert(!ego_load_raise(&s, 5)); /* после выполнения нельзя */

    /* --- сложность растёт по кругам: код +5 минут за собранный круг --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_slot_minutes(&s, 6) == 45 && ego_slot_minutes(&s, 1) == 20 && ego_slot_minutes(&s, 0) == 0);
    s.peak = 2500;
    assert(ego_slot_minutes(&s, 6) == 55 && ego_slot_minutes(&s, 10) == 30);
    s.peak = 11000;
    assert(ego_slot_minutes(&s, 6) == 95); /* потолок роста: 10 кругов */

    /* --- проверки: +2 за «да», вылет со штрафом --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_check(&s, EGO_CHECK_FLOW, true) == EgoReactionGrowth && s.ego == 602);
    assert(ego_check(&s, EGO_CHECK_EGO, false) == EgoReactionPenalty && s.ego == 601);
    assert(s.check_ok[EGO_CHECK_FLOW] == 1 && s.check_fail[EGO_CHECK_EGO] == 1);
    assert(ego_check(&s, 5, true) == EgoReactionNone);

    /* --- переносы: первый бесплатный, дальше -0.5; бонус пропускается бесплатно --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_postpone_cost(&s, 3) == 0);
    assert(ego_quest_postpone(&s, 3, T0) == 0 && s.ego == 600);
    assert(ego_postpone_cost(&s, 3) == 1);
    assert(ego_quest_postpone(&s, 3, T0) == 1 && s.ego == 600 && s.half_debt == 1);
    assert(ego_quest_postpone(&s, 3, T0) == 2 && s.ego == 599 && s.half_debt == 0);
    assert(s.deck_drop & (1u << 3));
    assert(ego_quest_postpone(&s, 99, T0) == -1);
    assert(ego_slot_is_bonus(16) && !ego_slot_is_bonus(15));
    assert(ego_postpone_cost(&s, 16) == 0 && ego_quest_postpone(&s, 16, T0) == 0);
    assert((s.deck_drop & (1u << 16)) && s.ego == 599);

    /* --- выдача карточек: ядро (текущее окно, потом остальное), затем бонусы --- */
    fresh(&s);
    upd(&s, d);
    assert(ego_next_card(&s, 1, T0) == 6); /* день: «Код» */
    assert(ego_next_card(&s, 0, T0) == 0); /* утро: «Подъём» */
    assert(ego_next_card(&s, 2, T0) == 12); /* вечер: «Внимание на себе» */
    ego_quest_done(&s, 0);
    assert(ego_next_card(&s, 0, T0) == 1);
    s.deck_done = (1u << EGO_CORE_QUESTS) - 1;
    assert(ego_next_card(&s, 1, T0) == 16); /* ядро закрыто: первый бонус */
    s.deck_drop |= 1u << 16;
    assert(ego_next_card(&s, 1, T0) == 17);
    assert(ego_quest_done(&s, 17) == EgoReactionGrowth && ego_bonus_today(&s) == 1);

    /* подписи рангов */
    assert(ego_band_letter(3)[0] == 'B' && ego_band_letter(5)[0] == 'S');
    assert(ego_roman(1)[0] == 'I' && ego_roman(11)[1] == 'I' && ego_roman(0)[0] == 0);

    puts("ok");
    return 0;
}
