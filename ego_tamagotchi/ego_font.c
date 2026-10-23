#include "ego_font.h"

#define GLYPH_SPACE 42

/* Следующий символ UTF-8 (ASCII и двухбайтовые, т.е. вся кириллица). */
static const char* next_cp(const char* s, uint32_t* cp) {
    uint8_t b0 = (uint8_t)s[0];
    if(b0 < 0x80) {
        *cp = b0;
        return s + 1;
    }
    if((b0 & 0xE0) == 0xC0 && s[1]) {
        *cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
        return s + 2;
    }
    *cp = '?';
    if((b0 & 0xF0) == 0xE0) return s[1] && s[2] ? s + 3 : s + 1;
    if((b0 & 0xF8) == 0xF0) return s[1] && s[2] && s[3] ? s + 4 : s + 1;
    return s + 1;
}

static int glyph_index(uint32_t cp) {
    if(cp >= '0' && cp <= '9') return (int)(cp - '0');
    if(cp >= 'a' && cp <= 'z') cp -= 0x20; // строчные латинские -> заглавные
    if(cp == 0x401 || cp == 0x451) cp = 0x415; // Ё, ё -> Е
    if(cp >= 0x430 && cp <= 0x44F) cp -= 0x20; // строчные -> заглавные
    if(cp >= 0x410 && cp <= 0x42F) return 10 + (int)(cp - 0x410);
    switch(cp) {
    case '.':
        return 43;
    case ',':
        return 44;
    case '-':
        return 45;
    case '+':
        return 46;
    case '/':
        return 47;
    case '%':
        return 48;
    case ':':
        return 49;
    case '?':
        return 50;
    case '!':
        return 51;
    case 'D':
        return 52;
    case 'S':
        return 53;
    case 'G':
        return 54;
    case 'I':
        return 55;
    case 'V':
        return 56;
    /* латиница, похожая на кириллицу: A B C E O X H K M P T */
    case 'A':
        return 10;
    case 'B':
        return 12;
    case 'C':
        return 27;
    case 'E':
        return 15;
    case 'H':
        return 23;
    case 'K':
        return 20;
    case 'M':
        return 22;
    case 'O':
        return 24;
    case 'P':
        return 26;
    case 'T':
        return 28;
    case 'X':
        return 31;
    default:
        return GLYPH_SPACE;
    }
}

int ego_text_len(const char* s) {
    int n = 0;
    uint32_t cp;
    while(*s) {
        s = next_cp(s, &cp);
        n++;
    }
    return n;
}

int ego_text_width(const char* s) {
    int n = ego_text_len(s);
    return n ? n * EGO_GLYPH_ADV - 1 : 0;
}

void ego_text_n(Canvas* c, int x, int y, const char* s, int max_chars) {
    uint32_t cp;
    for(int i = 0; *s && i < max_chars; i++) {
        s = next_cp(s, &cp);
        const uint8_t* g = ego_glyphs[glyph_index(cp)];
        for(int row = 0; row < 7; row++) {
            for(int col = 0; col < 5; col++)
                if(g[row] & (1 << (4 - col))) canvas_draw_dot(c, x + col, y + row);
        }
        x += EGO_GLYPH_ADV;
    }
}

void ego_text(Canvas* c, int x, int y, const char* s) {
    ego_text_n(c, x, y, s, 1000);
}

void ego_text_center(Canvas* c, int cx, int y, const char* s) {
    ego_text(c, cx - ego_text_width(s) / 2, y, s);
}

void ego_text_right(Canvas* c, int x_right, int y, const char* s) {
    ego_text(c, x_right - ego_text_width(s) + 1, y, s);
}

void ego_text_scaled(Canvas* c, int x, int y, const char* s, int scale) {
    uint32_t cp;
    while(*s) {
        s = next_cp(s, &cp);
        const uint8_t* g = ego_glyphs[glyph_index(cp)];
        for(int row = 0; row < 7; row++) {
            for(int col = 0; col < 5; col++)
                if(g[row] & (1 << (4 - col)))
                    canvas_draw_box(c, x + col * scale, y + row * scale, scale, scale);
        }
        x += EGO_GLYPH_ADV * scale;
    }
}

int ego_text_scaled_width(const char* s, int scale) {
    int n = ego_text_len(s);
    return n ? n * EGO_GLYPH_ADV * scale - scale : 0;
}
