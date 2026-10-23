#pragma once
/* Свой шрифт 5x7 с кириллицей: стандартные шрифты Flipper её не содержат.
 * Только заглавные: строчные буквы переводятся в заглавные автоматически.
 * Строки - UTF-8. y - верхняя строка текста (высота 7). */
#include <gui/gui.h>

#define EGO_GLYPH_COUNT 57
#define EGO_GLYPH_ADV 6 // 5 пикселей буква + 1 пробел

extern const uint8_t ego_glyphs[EGO_GLYPH_COUNT][7];

int ego_text_len(const char* utf8); // число символов
int ego_text_width(const char* utf8); // ширина в пикселях

void ego_text(Canvas* canvas, int x, int y, const char* utf8);
void ego_text_n(Canvas* canvas, int x, int y, const char* utf8, int max_chars);
void ego_text_center(Canvas* canvas, int cx, int y, const char* utf8);
void ego_text_right(Canvas* canvas, int x_right, int y, const char* utf8);

/* Крупный текст: каждый пиксель шрифта - квадрат scale x scale (для таймера). Ширина = len*6*scale - scale. */
void ego_text_scaled(Canvas* canvas, int x, int y, const char* utf8, int scale);
int ego_text_scaled_width(const char* utf8, int scale);
