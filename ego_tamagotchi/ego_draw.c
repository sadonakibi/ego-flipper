#include "ego_draw.h"

/* Око Ра (Гора): миндалевидный глаз с бровью, меткой под глазом и хвостом.
 * Рисуется аналитически, чтобы веки могли моргать, а взгляд двигаться.
 * Все размеры в пикселях относительно центра глаза. */
#define EW 26 // полуширина глаза
#define H_UP 12 // высота верхнего века при полном раскрытии
#define H_LO 7 // высота нижнего века
#define R_IRIS 9

typedef enum {
    FillNone,
    FillDots,
    FillLines,
    FillSolid,
} FillMode;

typedef struct {
    Canvas* c;
    int cx, cy;
    int burn; // 0..100 доля рассыпанных пикселей
    int gap; // разрывы контура: 0 - нет, иначе каждый gap-й пиксель
    uint32_t tick;
    uint32_t tw; // фаза мерцания разрывов
} Pen;

static uint32_t hash2(int x, int y) {
    uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static void put(const Pen* p, int x, int y) {
    if(p->burn > 0 && (int)(hash2(x * 5 + 7, y + (int)(p->tick / 2)) % 100) < p->burn) return;
    canvas_draw_dot(p->c, p->cx + x, p->cy + y);
}

static void put_edge(const Pen* p, int x, int y) {
    if(p->gap && hash2(x * 3 + 1, y + (int)p->tw) % (uint32_t)p->gap == 0) return;
    put(p, x, y);
}

/* Профиль глаза: 256 в центре, 0 на уголках. */
static int shape_t(int x) {
    if(x < -EW || x > EW) return 0;
    return (EW * EW - x * x) * 256 / (EW * EW);
}

static int hu(int x, int op) {
    return H_UP * op * shape_t(x) / 25600;
}

static int hl(int x, int op) {
    return H_LO * op * shape_t(x) / 25600;
}

static int brow_h(int x) {
    return (H_UP + 6) * shape_t(x) / 256;
}

static int max3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}

static int min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

/* Моргание: раз в 70 тиков в случайный момент, 6 тиков на закрытие и открытие. */
static int blink_factor(uint32_t tick) {
    static const uint8_t seq[6] = {100, 55, 12, 12, 55, 100};
    uint32_t cycle = tick / 70, local = tick % 70;
    uint32_t start = 20 + hash2((int)cycle, 1) % 40;
    if(local >= start && local < start + 6) return seq[local - start];
    return 100;
}

/* Взгляд плавно переходит между точками каждые 40 тиков. */
static void gaze(uint32_t tick, EgoLevel lvl, int* gx, int* gy) {
    int seg = (int)(tick / 40), f = (int)(tick % 40);
    int ax = (int)(hash2(seg, 3) % 13) - 6, bx = (int)(hash2(seg + 1, 3) % 13) - 6;
    int ay = (int)(hash2(seg, 4) % 5) - 2, by = (int)(hash2(seg + 1, 4) % 5) - 2;
    int s = f * f * (120 - 2 * f) / 64; // сглаживание 0..1000
    *gx = ax + (bx - ax) * s / 1000;
    *gy = ay + (by - ay) * s / 1000;
    if(lvl == EgoLevelCritical) {
        *gx /= 3;
        *gy = 2; // смотрит вниз
    } else if(lvl == EgoLevelWeak) {
        *gx /= 2;
        *gy += 1;
    }
}

/* Пепел поднимается от глаза: чем сильнее горит, тем его больше. */
static void embers(Canvas* c, int cx, int cy, uint32_t tick, int n) {
    for(int i = 0; i < n; i++) {
        uint32_t t = tick * 2 + (uint32_t)i * 13;
        int age = (int)(t % 40);
        int x = cx - 22 + (int)(hash2(i, (int)(t / 40)) % 44);
        int y = cy + 8 - age;
        if(y >= 8 && y <= 50) canvas_draw_dot(c, x, y);
    }
}

void ego_draw_eye(
    Canvas* c,
    int cx,
    int cy,
    const EgoLook* look,
    EgoReaction reaction,
    uint8_t rt,
    uint32_t tick) {
    /* --- состояние из уровней --- */
    int thick = 1, gap = 0, burn = look->burn, ring = 0, rays = 0, pr = 3, droop = 0;
    bool tremor = false, tear = false;
    FillMode fill = FillNone;

    switch(look->contour) {
    case EgoLevelCritical:
        gap = 3;
        tremor = true;
        break;
    case EgoLevelWeak:
        gap = 9;
        break;
    case EgoLevelStable:
        break;
    default:
        thick = 2;
        break;
    }
    switch(look->density) {
    case EgoLevelCritical:
        fill = FillNone;
        droop = 4;
        break;
    case EgoLevelWeak:
        fill = FillDots;
        droop = 2;
        break;
    case EgoLevelStable:
        fill = FillLines;
        break;
    default:
        fill = FillSolid;
        pr = 4;
        break;
    }

    static const uint8_t base_open[4] = {35, 65, 90, 100};
    int base = base_open[look->total];
    if(look->total == EgoLevelCritical) {
        base += (int)(hash2((int)(tick / 3), 9) % 20); // веко дрожит
        tear = true;
    }
    if(look->total == EgoLevelStrong) rays = 5;

    int gx, gy;
    gaze(tick, look->total, &gx, &gy);
    if(look->flags & EGO_LOOK_GAZE) {
        gx = look->gaze_x;
        gy = look->gaze_y;
    }
    bool no_mark = look->flags & EGO_LOOK_NO_MARK;
    int op_react = 100;

    /* --- реакции (12 тиков) --- */
    if(reaction != EgoReactionNone && rt < EGO_REACTION_TICKS) {
        switch(reaction) {
        case EgoReactionGrowth: { // глаз раскрывается, зрачок расширяется, расходится волна
            static const uint8_t k[10] = {110, 120, 126, 126, 120, 114, 108, 104, 100, 100};
            if(rt < 10) op_react = k[rt];
            if(rt >= 1 && rt <= 8) fill = FillSolid;
            pr += 1;
            if(rt < 9) ring = 6 + rt * 3;
            break;
        }
        case EgoReactionReset: { // поймал: зрачок собирается, веки сужаются и возвращаются
            static const uint8_t k[7] = {100, 80, 62, 62, 72, 90, 100};
            if(rt < 7) op_react = k[rt];
            pr = 2;
            gx = 0;
            gy = 0;
            break;
        }
        case EgoReactionPenalty: { // уход: глаз закрывается, контур рвётся, слеза
            static const uint8_t k[10] = {100, 50, 15, 15, 30, 50, 70, 80, 90, 100};
            if(rt < 10) op_react = k[rt];
            fill = FillNone;
            gap = (rt % 2) ? 3 : 2;
            tremor = true;
            tear = true;
            if(rt < 6 && burn < 30) burn = 30;
            break;
        }
        case EgoReactionReturn: // сборка из пепла, глаз открывается
            burn = 85 * (EGO_REACTION_TICKS - rt) / EGO_REACTION_TICKS;
            op_react = 20 + 80 * rt / (EGO_REACTION_TICKS - 1);
            if(rt >= 6 && rt <= 9) thick = 2;
            if(rt >= 6) ring = (rt - 5) * 5;
            break;
        default:
            break;
        }
    }

    if(look->burn > 0) { // вне ЭГО: веко тяжелеет, взгляд вниз, слеза
        base = base * 70 / 100;
        gy = 2;
        tear = true;
    }

    int op = base * blink_factor(tick) / 100 * op_react / 100;
    if(look->open_force) op = look->open_force;
    if(op > 130) op = 130;

    Pen pen = {
        .c = c,
        .cx = cx,
        .cy = cy,
        .burn = burn,
        .gap = gap,
        .tick = tick,
        .tw = tremor ? tick : tick / 6,
    };
    const Pen* p = &pen;

    /* --- веки --- */
    for(int x = -EW; x <= EW; x++) {
        int amax = max3(hu(x - 1, op), hu(x, op), hu(x + 1, op));
        int amin = min3(hu(x - 1, op), hu(x, op), hu(x + 1, op));
        for(int y = -amax; y <= -amin + thick - 1; y++) put_edge(p, x, y);
        int bmax = max3(hl(x - 1, op), hl(x, op), hl(x + 1, op));
        int bmin = min3(hl(x - 1, op), hl(x, op), hl(x + 1, op));
        for(int y = bmin - thick + 1; y <= bmax; y++) put_edge(p, x, y);
    }

    /* --- бровь --- */
    int bt = thick == 2 ? 3 : 2;
    for(int x = -EW; x <= EW; x++) {
        int amax = max3(brow_h(x - 1), brow_h(x), brow_h(x + 1));
        int amin = min3(brow_h(x - 1), brow_h(x), brow_h(x + 1));
        for(int y = -amax + droop; y <= -amin + droop + bt - 2; y++) put_edge(p, x, y);
    }

    /* --- радужка и зрачок (внутри век) --- */
    int lim = EW - R_IRIS - 2;
    int px = gx > lim ? lim : (gx < -lim ? -lim : gx), py = gy;
    for(int x = -EW; x <= EW; x++) {
        int top = -hu(x, op) + thick, bot = hl(x, op) - thick + 1;
        for(int y = top; y < bot; y++) {
            int d2 = (x - px) * (x - px) + (y - py) * (y - py);
            if(d2 > R_IRIS * R_IRIS) continue;
            if(d2 >= (R_IRIS - 1) * (R_IRIS - 1)) {
                put(p, x, y);
            } else if(d2 <= pr * pr) {
                if(!(x == px - 1 && y == py - 1)) put(p, x, y); // блик
            } else {
                bool on = false;
                switch(fill) {
                case FillDots:
                    on = hash2(x, y + (int)(tick / 6)) % 9 == 0;
                    break;
                case FillLines:
                    on = ((y + 100 + (int)(tick / 5)) % 3) == 0;
                    break;
                case FillSolid:
                    on = ((y + 100 + (int)(tick / 3)) % 5) != 0;
                    break;
                default:
                    break;
                }
                if(on) put(p, x, y);
            }
        }
    }

    /* --- метка под глазом: линия и завиток --- */
    if(!no_mark) {
    int y0 = hl(-3, 100) + 2;
    for(int y = y0; y < y0 + 10; y++) {
        for(int dx = 0; dx < thick; dx++) put_edge(p, -3 + dx, y);
    }
    for(int x = -7; x < 5; x++) {
        for(int y = y0 + 6; y < y0 + 16; y++) {
            int d2 = x * x + (y - (y0 + 9)) * (y - (y0 + 9));
            if(d2 >= 6 && d2 <= 11) put_edge(p, x, y);
        }
    }

    /* --- хвост с завитком --- */
    for(int dx = 0; dx <= 12; dx++) {
        int y = dx * dx / 14;
        for(int t = 0; t < thick; t++) put_edge(p, EW + dx, y + t);
    }
    for(int x = EW + 6; x < EW + 16; x++) {
        for(int y = 4; y < 14; y++) {
            int d2 = (x - (EW + 11)) * (x - (EW + 11)) + (y - 9) * (y - 9);
            if(d2 >= 3 && d2 <= 6) put_edge(p, x, y);
        }
    }

    /* --- слеза падает из-под нижнего века --- */
    if(tear) {
        int ty = hl(8, 100) + 1 + (int)((tick / 2) % 16);
        put(p, 8, ty);
        put(p, 8, ty + 1);
    }

    }

    /* --- лучи Ра над бровью (высокий ранг; пульсируют) --- */
    for(int i = 0; i < rays; i++) {
        int x = (2 * i - (rays - 1)) * 9 / 2;
        int bas = -brow_h(x) + droop - 4;
        int ln = 3 + (int)((tick / 3 + (uint32_t)i) % 3);
        for(int y = bas - ln; y < bas; y++) put(p, x, y);
    }

    /* --- волна --- */
    if(ring > 0) {
        for(int x = -EW - 12; x <= EW + 12; x++) {
            for(int y = -22; y <= 23; y++) {
                int d = (x * x) / 2 + y * y * 2;
                int dr = d - ring * ring;
                if(dr < 0) dr = -dr;
                if(dr < ring * 2) put(p, x, y);
            }
        }
    }

    if(burn > 0) embers(c, cx, cy, tick, burn / 8 + 1);
}

/* ---------- заставка ---------- */

#define SPLASH_X0 35
#define SPLASH_Y0 41
#define SPLASH_BLOCK 3
#define SPLASH_LETTER_STEP 21 // 5 блоков по 3 пикселя + зазор 6

static const uint8_t k_splash_glyph[3][7] = {
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, // E
    {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0E}, // G
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // O
};

/* k-й блок надписи в порядке выжигания: буква за буквой, столбец за столбцом. */
static bool splash_block(int k, int* bx, int* by) {
    int n = 0;
    for(int l = 0; l < 3; l++) {
        for(int col = 0; col < 5; col++) {
            for(int row = 0; row < 7; row++) {
                if(!(k_splash_glyph[l][row] & (1 << (4 - col)))) continue;
                if(n++ == k) {
                    *bx = SPLASH_X0 + l * SPLASH_LETTER_STEP + col * SPLASH_BLOCK;
                    *by = SPLASH_Y0 + row * SPLASH_BLOCK;
                    return true;
                }
            }
        }
    }
    return false;
}

static int splash_total(void) {
    int bx, by, n = 0;
    while(splash_block(n, &bx, &by)) n++;
    return n;
}

void ego_draw_splash(Canvas* c, uint32_t t) {
    const int total = splash_total();
    const int burn_start = 16;
    const int per_tick = 2;
    const int end = burn_start + total / per_tick + 2;

    int p = t >= (uint32_t)burn_start ? ((int)t - burn_start) * per_tick : 0; // сожжено блоков
    int shown = p > total ? total : p;
    bool burning = shown > 0 && p <= total + 2;

    /* веки: щель -> раскрытие -> взгляд на выжигаемый блок -> моргание в конце */
    int open;
    if(t < 8)
        open = 2 + (int)t;
    else if(t < (uint32_t)burn_start)
        open = 10 + ((int)t - 8) * 12;
    else
        open = 106;
    if(t >= (uint32_t)end && t < (uint32_t)end + 4) {
        static const uint8_t blink[4] = {60, 15, 60, 106};
        open = blink[t - (uint32_t)end];
    }

    int tx = 64, ty = 50;
    if(burning) {
        int bx, by;
        if(splash_block(shown - 1, &bx, &by)) {
            tx = bx + 1;
            ty = by + 1;
        }
    }
    int gx = (tx - 64) / 5;
    if(gx > 6) gx = 6;
    if(gx < -6) gx = -6;

    EgoLook look = {
        .contour = EgoLevelStrong,
        .density = EgoLevelStrong,
        .total = EgoLevelStrong,
        .flags = EGO_LOOK_NO_MARK | EGO_LOOK_GAZE,
        .open_force = (uint8_t)open,
        .gaze_x = (int8_t)gx,
        .gaze_y = 3,
    };
    ego_draw_eye(c, 64, 19, &look, EgoReactionNone, 0, t);

    /* выжженные блоки; два последних ещё горячие (контуром) */
    for(int k = 0; k < shown; k++) {
        int bx, by;
        if(!splash_block(k, &bx, &by)) break;
        if(burning && k >= shown - 2)
            canvas_draw_frame(c, bx, by, SPLASH_BLOCK, SPLASH_BLOCK);
        else
            canvas_draw_box(c, bx, by, SPLASH_BLOCK, SPLASH_BLOCK);
    }

    /* луч из зрачка к месту выжигания и искры */
    if(burning) {
        int sx = 64 + gx, sy = 19 + 3 + 6;
        for(int i = 0; i <= 12; i++) {
            if(i % 2) continue;
            canvas_draw_dot(c, sx + (tx - sx) * i / 12, sy + (ty - sy) * i / 12);
        }
        for(int i = 0; i < 4; i++) {
            int jx = (int)(hash2((int)t, i) % 7) - 3;
            int jy = (int)(hash2((int)t, i + 9) % 7) - 3;
            canvas_draw_dot(c, tx + jx, ty + jy);
        }
    }

    /* вспышка по контуру надписи, когда всё выжжено */
    if(t >= (uint32_t)end && t < (uint32_t)end + 6 && ((t - (uint32_t)end) % 2) == 0)
        canvas_draw_frame(
            c, SPLASH_X0 - 4, SPLASH_Y0 - 4, 2 * SPLASH_LETTER_STEP + 15 + 8, 7 * SPLASH_BLOCK + 8);
}
