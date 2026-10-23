#pragma once
#include <gui/gui.h>
#include "ego_core.h"

#define EGO_REACTION_TICKS 12

/* Око Ра: ЭГО как инструмент. Уровни меняют разные части:
 *   contour - обводка век и брови по основанию (разорвана / тонкая / чёткая / двойная)
 *   density - радужка по текущему EGO (пусто / точки / линии / почти сплошная), бровь
 *   total   - раскрытие века, лучи (ранги B и выше), слеза на низком ранге
 */
typedef struct {
    EgoLevel contour; // от основания
    EgoLevel density; // от текущего EGO
    EgoLevel total;
    uint8_t burn; // 0..100: доля глаза, рассыпавшаяся в пепел (уход из EGO)
    uint8_t flags; // EGO_LOOK_*
    uint8_t open_force; // 1..130: принудительное раскрытие век (0 - само, с морганием)
    int8_t gaze_x, gaze_y; // взгляд, если задан EGO_LOOK_GAZE
} EgoLook;

#define EGO_LOOK_NO_MARK 1 // без метки под глазом, хвоста и слезы (заставка)
#define EGO_LOOK_GAZE 2 // взгляд задан gaze_x / gaze_y

/* Око по центру (cx, cy). rt - тики с начала реакции, tick - общий счётчик анимации. */
void ego_draw_eye(
    Canvas* canvas,
    int cx,
    int cy,
    const EgoLook* look,
    EgoReaction reaction,
    uint8_t rt,
    uint32_t tick);

/* Заставка: глаз раскрывается и лучом выжигает на экране надпись EGO. t - тики с начала. */
#define EGO_SPLASH_TICKS 54
void ego_draw_splash(Canvas* canvas, uint32_t t);
