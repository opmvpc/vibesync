#include "ui.h"

#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------- primitives ---

typedef struct {
    i32 x, y, w, h;
} Rect;

static Rect rect(i32 x, i32 y, i32 w, i32 h) {
    Rect r = {x, y, w, h};
    return r;
}
static Rect rect_inset(Rect r, i32 d) { return rect(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }
static b32 rect_hit(Rect r, i32 x, i32 y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static COLORREF cr(u32 rgb) { return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

// mix mélange deux couleurs (t sur 256) — sert aux états hover/press.
static u32 mix(u32 a, u32 b, i32 t) {
    i32 ar = (i32)((a >> 16) & 0xff), ag = (i32)((a >> 8) & 0xff), ab = (i32)(a & 0xff);
    i32 br = (i32)((b >> 16) & 0xff), bg = (i32)((b >> 8) & 0xff), bb = (i32)(b & 0xff);
    i32 r = ar + (br - ar) * t / 256, g = ag + (bg - ag) * t / 256, bl = ab + (bb - ab) * t / 256;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

// S convertit des unités logiques (96 ppp) en pixels physiques.
static i32 S(const UiApp *a, i32 v) { return MulDiv(v, a->dpi, 96); }

static void fill_rect(HDC dc, Rect r, u32 color) {
    RECT rc = {r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH b = CreateSolidBrush(cr(color));
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

static void fill_round(HDC dc, Rect r, i32 radius, u32 color) {
    if (radius <= 0) {
        fill_rect(dc, r, color);
        return;
    }
    HBRUSH b = CreateSolidBrush(cr(color));
    HPEN p = CreatePen(PS_SOLID, 1, cr(color));
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.x, r.y, r.x + r.w, r.y + r.h, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void stroke_round(HDC dc, Rect r, i32 radius, u32 color, i32 width) {
    HPEN p = CreatePen(PS_SOLID, width, cr(color));
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

// wide convertit une tranche UTF-8 en UTF-16 dans l'arène de frame.
static const wchar_t *wide(UiApp *a, Str8 s, isize *out_len) {
    return (const wchar_t *)utf8_to_utf16(a->scratch, s, out_len);
}

static void draw_text_rect(UiApp *a, HDC dc, Rect r, Str8 s, u32 color, HFONT font, UINT flags) {
    if (s.len == 0) return;
    isize n = 0;
    const wchar_t *w = wide(a, s, &n);
    HGDIOBJ of = SelectObject(dc, font);
    SetTextColor(dc, cr(color));
    SetBkMode(dc, TRANSPARENT);
    RECT rc = {r.x, r.y, r.x + r.w, r.y + r.h};
    DrawTextW(dc, w, (int)n, &rc, flags);
    SelectObject(dc, of);
}

static void draw_text(UiApp *a, HDC dc, Rect r, const char *s, u32 color, HFONT font, UINT flags) {
    draw_text_rect(a, dc, r, str8_from_cstr(s), color, font, flags);
}

static i32 text_width(UiApp *a, HDC dc, Str8 s, HFONT font) {
    isize n = 0;
    const wchar_t *w = wide(a, s, &n);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, w, (int)n, &sz);
    SelectObject(dc, of);
    return sz.cx;
}

static i32 text_height(UiApp *a, HDC dc, HFONT font) {
    VS_UNUSED(a);
    HGDIOBJ of = SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    return tm.tmHeight;
}

// ------------------------------------------------------------------ texte ---

void ui_text_set(UiText *t, Str8 s) {
    isize n = VS_MIN(s.len, (isize)UI_TEXT_CAP - 1);
    // Ne pas couper au milieu d'un caractère UTF-8.
    while (n > 0 && (s.data[n] & 0xc0) == 0x80) n--;
    if (n > 0) memcpy(t->data, s.data, (size_t)n);
    t->data[n] = 0;
    t->len = n;
    t->caret = n;
}

Str8 ui_text_str(const UiText *t) {
    Str8 s = {(u8 *)t->data, t->len};
    return s;
}

static isize cp_prev(const UiText *t, isize i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && (t->data[i] & 0xc0) == 0x80) i--;
    return i;
}

static isize cp_next(const UiText *t, isize i) {
    if (i >= t->len) return t->len;
    i++;
    while (i < t->len && (t->data[i] & 0xc0) == 0x80) i++;
    return i;
}

static void text_insert(UiText *t, u32 cp) {
    u8 buf[4];
    isize n = utf8_encode(cp, buf);
    if (t->len + n >= UI_TEXT_CAP) return;
    memmove(t->data + t->caret + n, t->data + t->caret, (size_t)(t->len - t->caret));
    memcpy(t->data + t->caret, buf, (size_t)n);
    t->len += n;
    t->caret += n;
    t->data[t->len] = 0;
}

static void text_erase(UiText *t, isize from, isize to) {
    if (to <= from) return;
    memmove(t->data + from, t->data + to, (size_t)(t->len - to));
    t->len -= to - from;
    t->caret = from;
    t->data[t->len] = 0;
}

static void text_paste(UiApp *a, UiText *t) {
    if (!OpenClipboard(a->hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const u16 *w = (const u16 *)GlobalLock(h);
        if (w) {
            Str8 s = utf16_to_utf8(a->scratch, w);
            for (isize i = 0; i < s.len;) {
                u8 c = s.data[i];
                if (c == '\r' || c == '\n' || c == '\t') {  // mono-ligne
                    i++;
                    continue;
                }
                isize n = 1;
                if ((c & 0xe0) == 0xc0) n = 2;
                else if ((c & 0xf0) == 0xe0) n = 3;
                else if ((c & 0xf8) == 0xf0) n = 4;
                if (i + n > s.len) break;
                if (t->len + n >= UI_TEXT_CAP) break;
                memmove(t->data + t->caret + n, t->data + t->caret, (size_t)(t->len - t->caret));
                memcpy(t->data + t->caret, s.data + i, (size_t)n);
                t->len += n;
                t->caret += n;
                i += n;
            }
            t->data[t->len] = 0;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

// --------------------------------------------------------------- entrées ---

void ui_on_mouse_move(UiApp *app, i32 x, i32 y) {
    app->mouse_x = x;
    app->mouse_y = y;
}

void ui_on_mouse_down(UiApp *app, i32 x, i32 y) {
    app->mouse_x = x;
    app->mouse_y = y;
    app->mouse_down = 1;
    app->mouse_pressed = 1;
}

void ui_on_mouse_up(UiApp *app, i32 x, i32 y) {
    app->mouse_x = x;
    app->mouse_y = y;
    app->mouse_down = 0;
    app->mouse_released = 1;
}

void ui_on_wheel(UiApp *app, i32 delta) { app->wheel += delta; }

void ui_on_char(UiApp *app, u32 cp) {
    if (cp < 0x20 || cp == 0x7f) return;  // les touches de contrôle passent par ui_on_key
    if (app->char_count < VS_ARRAY_COUNT(app->chars)) app->chars[app->char_count++] = cp;
}

void ui_on_key(UiApp *app, u32 vk, b32 ctrl) {
    app->ctrl_down = ctrl;
    if (app->key_count < VS_ARRAY_COUNT(app->keys)) app->keys[app->key_count++] = vk;
}

static b32 key_pressed(UiApp *app, u32 vk) {
    for (isize i = 0; i < app->key_count; i++) {
        if (app->keys[i] == vk) return 1;
    }
    return 0;
}

// ------------------------------------------------------------- widgets ---

typedef enum {
    BTN_PRIMARY,
    BTN_GHOST,
    BTN_SUBTLE,
    BTN_DANGER,
    BTN_ON,
} BtnStyle;

static b32 button(UiApp *a, HDC dc, Rect r, const char *label, u64 id, BtnStyle style, b32 enabled) {
    b32 hover = enabled && rect_hit(r, a->mouse_x, a->mouse_y);
    b32 clicked = 0;
    if (hover) a->hot = id;
    if (hover && a->mouse_pressed) {
        a->active = id;
        a->focus = id;
    }
    b32 pressed = enabled && a->active == id && a->mouse_down && hover;
    if (a->active == id && a->mouse_released) {
        if (hover && enabled) clicked = 1;
        a->active = 0;
    }

    u32 bg, fg, border = 0;
    b32 has_border = 0;
    switch (style) {
        case BTN_PRIMARY:
            bg = UI_ACCENT;
            fg = 0xffffffu;
            if (hover) bg = UI_ACCENT_HI;
            if (pressed) bg = mix(UI_ACCENT, 0x000000u, 40);
            break;
        case BTN_ON:
            bg = UI_OK;
            fg = 0x0b1f12u;
            if (hover) bg = mix(UI_OK, 0xffffffu, 30);
            if (pressed) bg = mix(UI_OK, 0x000000u, 40);
            break;
        case BTN_DANGER:
            bg = UI_PANEL_HI;
            fg = UI_DANGER;
            has_border = 1;
            border = mix(UI_DANGER, UI_PANEL, 150);
            if (hover) bg = mix(UI_DANGER, UI_PANEL, 210);
            if (pressed) bg = mix(UI_DANGER, UI_PANEL, 170);
            break;
        case BTN_SUBTLE:
            bg = UI_PANEL_HI;
            fg = UI_TEXT;
            if (hover) bg = mix(UI_PANEL_HI, 0xffffffu, 18);
            if (pressed) bg = mix(UI_PANEL_HI, 0x000000u, 30);
            break;
        case BTN_GHOST:
        default:
            bg = UI_PANEL;
            fg = UI_TEXT;
            has_border = 1;
            border = UI_LINE;
            if (hover) bg = UI_PANEL_HI;
            if (pressed) bg = mix(UI_PANEL_HI, 0x000000u, 30);
            break;
    }
    if (!enabled) {
        bg = mix(UI_PANEL, UI_BG, 128);
        fg = UI_FAINT;
        has_border = 1;
        border = UI_LINE;
    }
    i32 radius = S(a, 8);
    fill_round(dc, r, radius, bg);
    if (has_border) stroke_round(dc, r, radius, border, 1);
    if (a->focus == id && enabled) stroke_round(dc, rect_inset(r, -2), radius + 2, UI_ACCENT_HI, 1);
    draw_text(a, dc, r, label, fg, a->f_bold, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return clicked;
}

// glyph_play dessine ▶ ou ❚❚ à la main : aucune dépendance à une police
// d'icônes, un rendu net à tous les DPI.
static void glyph_play(HDC dc, Rect r, u32 color, b32 show_play) {
    HBRUSH br = CreateSolidBrush(cr(color));
    HPEN pen = CreatePen(PS_SOLID, 1, cr(color));
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    if (show_play) {
        i32 w = r.w, h = r.h;
        POINT p[3] = {
            {r.x + w / 6, r.y},
            {r.x + w - w / 8, r.y + h / 2},
            {r.x + w / 6, r.y + h},
        };
        Polygon(dc, p, 3);
    } else {
        i32 bw = r.w / 3;
        Rect a = {r.x, r.y, bw, r.h};
        Rect b = {r.x + r.w - bw, r.y, bw, r.h};
        Rectangle(dc, a.x, a.y, a.x + a.w, a.y + a.h);
        Rectangle(dc, b.x, b.y, b.x + b.w, b.y + b.h);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// field dessine un champ de saisie. Renvoie 1 si Entrée a été pressée dedans.
static b32 field(UiApp *a, HDC dc, Rect r, UiText *t, const char *placeholder, u64 id, b32 password,
                 i64 now_ms) {
    if (a->take_next_focus) {
        a->focus = id;
        a->take_next_focus = 0;
        a->caret_blink_ms = now_ms;
    }
    b32 hover = rect_hit(r, a->mouse_x, a->mouse_y);
    if (hover) a->hot = id;
    if (a->mouse_pressed) {
        if (hover) {
            a->focus = id;
            a->caret_blink_ms = now_ms;
        } else if (a->focus == id) {
            a->focus = 0;
        }
    }

    b32 focused = a->focus == id;
    b32 submitted = 0;
    if (focused) {
        for (isize i = 0; i < a->char_count; i++) text_insert(t, a->chars[i]);
        if (a->char_count > 0) a->caret_blink_ms = now_ms;
        a->char_count = 0;
        for (isize i = 0; i < a->key_count; i++) {
            u32 vk = a->keys[i];
            switch (vk) {
                case VK_BACK:
                    if (a->ctrl_down) text_erase(t, 0, t->caret);
                    else text_erase(t, cp_prev(t, t->caret), t->caret);
                    break;
                case VK_DELETE: text_erase(t, t->caret, cp_next(t, t->caret)); break;
                case VK_LEFT: t->caret = cp_prev(t, t->caret); break;
                case VK_RIGHT: t->caret = cp_next(t, t->caret); break;
                case VK_HOME: t->caret = 0; break;
                case VK_END: t->caret = t->len; break;
                case VK_RETURN: submitted = 1; break;
                case VK_TAB:
                    a->focus = 0;
                    a->take_next_focus = 1;
                    break;
                case 'V':
                    if (a->ctrl_down) text_paste(a, t);
                    break;
                case 'A':
                    if (a->ctrl_down) t->caret = t->len;
                    break;
                default: break;
            }
            a->caret_blink_ms = now_ms;
        }
        a->key_count = 0;
    }

    i32 radius = S(a, 8);
    fill_round(dc, r, radius, focused ? UI_PANEL_HI : mix(UI_PANEL, UI_BG, 90));
    stroke_round(dc, r, radius, focused ? UI_ACCENT : UI_LINE, 1);

    i32 pad = S(a, 10);
    Rect inner = rect(r.x + pad, r.y, r.w - 2 * pad, r.h);

    // Contenu affiché : les mots de passe sont masqués par des points.
    Str8 shown;
    if (password && t->len > 0) {
        isize dots = 0;
        for (isize i = 0; i < t->len; i++) {
            if ((t->data[i] & 0xc0) != 0x80) dots++;
        }
        u8 *buf = arena_push_array(a->scratch, u8, dots * 3 + 1);
        isize n = 0;
        for (isize i = 0; i < dots; i++) n += utf8_encode(0x2022, buf + n);
        shown = str8(buf, n);
    } else {
        shown = ui_text_str(t);
    }

    if (shown.len == 0 && !focused) {
        draw_text(a, dc, inner, placeholder, UI_FAINT, a->f_body, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        // Défilement : garder le caret visible dans le champ.
        Str8 before = password ? shown : str8_sub(shown, 0, t->caret);
        i32 caret_x = text_width(a, dc, before, a->f_body);
        i32 total = text_width(a, dc, shown, a->f_body);
        i32 off = 0;
        if (total > inner.w && caret_x > inner.w - S(a, 8)) off = caret_x - (inner.w - S(a, 8));
        HRGN clip = CreateRectRgn(inner.x, inner.y, inner.x + inner.w, inner.y + inner.h);
        SelectClipRgn(dc, clip);
        Rect tr = rect(inner.x - off, inner.y, inner.w + off + S(a, 200), inner.h);
        draw_text_rect(a, dc, tr, shown, UI_TEXT, a->f_body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (focused && ((now_ms - a->caret_blink_ms) / 500) % 2 == 0) {
            i32 cx = inner.x - off + caret_x;
            i32 ch = text_height(a, dc, a->f_body);
            fill_rect(dc, rect(cx, r.y + (r.h - ch) / 2, VS_MAX(1, S(a, 1)), ch), UI_ACCENT_HI);
        }
        SelectClipRgn(dc, NULL);
        DeleteObject(clip);
    }
    return submitted;
}

static void label(UiApp *a, HDC dc, Rect r, const char *text) {
    draw_text(a, dc, r, text, UI_MUTED, a->f_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void panel(UiApp *a, HDC dc, Rect r) {
    fill_round(dc, r, S(a, 12), UI_PANEL);
    stroke_round(dc, r, S(a, 12), UI_LINE, 1);
}

// badge dessine une pastille colorée (Prêt, latence, dérive…).
static void badge(UiApp *a, HDC dc, Rect r, const char *text, u32 fg, u32 bg) {
    fill_round(dc, r, r.h / 2, bg);
    draw_text(a, dc, r, text, fg, a->f_small, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ui_format_time(f64 sec, char *buf, isize cap) {
    if (cap <= 0) return;
    if (!f64_is_finite(sec) || sec < 0) sec = 0;
    if (sec > 359999) sec = 359999;  // 99:59:59
    i64 total = (i64)sec;
    i64 h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) snprintf(buf, (size_t)cap, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    else snprintf(buf, (size_t)cap, "%lld:%02lld", (long long)m, (long long)s);
}

// seekbar dessine la barre de position. Renvoie 1 quand l'utilisateur relâche
// après un déplacement (une seule commande envoyée, pas un flot).
static b32 seekbar(UiApp *a, HDC dc, Rect r, f64 pos, f64 dur, u64 id, f64 *out_pos) {
    i32 track_h = S(a, 6);
    Rect track = rect(r.x, r.y + (r.h - track_h) / 2, r.w, track_h);
    b32 hover = rect_hit(rect(r.x, r.y, r.w, r.h), a->mouse_x, a->mouse_y);
    b32 enabled = dur > 0;
    if (hover && enabled) a->hot = id;
    if (hover && enabled && a->mouse_pressed) {
        a->active = id;
        a->dragging_seek = 1;
    }

    f64 frac = (dur > 0) ? pos / dur : 0;
    if (a->active == id && a->dragging_seek) {
        f64 f = (f64)(a->mouse_x - track.x) / (f64)VS_MAX(1, track.w);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        frac = f;
    }
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    b32 done = 0;
    if (a->active == id && a->mouse_released) {
        a->active = 0;
        a->dragging_seek = 0;
        if (enabled) {
            *out_pos = frac * dur;
            done = 1;
        }
    }

    fill_round(dc, track, track_h / 2, UI_LINE);
    i32 fill_w = (i32)(frac * (f64)track.w);
    if (fill_w > 0) {
        Rect done_r = rect(track.x, track.y, fill_w, track.h);
        fill_round(dc, done_r, track_h / 2, enabled ? UI_ACCENT : UI_FAINT);
    }
    if (enabled && (hover || a->active == id)) {
        i32 kr = S(a, 7);
        i32 kx = track.x + fill_w;
        fill_round(dc, rect(kx - kr, track.y + track_h / 2 - kr, kr * 2, kr * 2), kr, 0xffffffu);
    }
    return done;
}

// ------------------------------------------------------------ alimentation ---

void ui_toast(UiApp *app, const char *text, int level, i64 now_ms) {
    snprintf(app->toast, sizeof(app->toast), "%s", text);
    app->toast_level = level;
    app->toast_until_ms = now_ms + 4000;
}

void ui_set_status(UiApp *app, const char *text, b32 is_error) {
    snprintf(app->status, sizeof(app->status), "%s", text);
    app->status_error = is_error;
}

void ui_chat_add(UiApp *app, Str8 from, Str8 text, b32 system) {
    if (app->chat_count >= UI_MAX_CHAT) {
        memmove(app->chat, app->chat + 1, sizeof(UiChatLine) * (UI_MAX_CHAT - 1));
        app->chat_count = UI_MAX_CHAT - 1;
    }
    UiChatLine *l = &app->chat[app->chat_count++];
    memset(l, 0, sizeof(*l));
    isize n = VS_MIN(from.len, (isize)sizeof(l->from) - 1);
    memcpy(l->from, from.data, (size_t)n);
    n = VS_MIN(text.len, (isize)sizeof(l->text) - 1);
    memcpy(l->text, text.data, (size_t)n);
    l->system = system;
    app->chat_scroll = 0;
}

// ------------------------------------------------------------ cycle de vie ---

void ui_init(UiApp *app) {
    memset(app, 0, sizeof(*app));
    app->dpi = 96;
    app->screen = UI_SCREEN_CONNECT;
    app->scratch = arena_create(VS_MB(2));
    ui_text_set(&app->f_server, str8_lit("ws://127.0.0.1:8080/ws"));
    ui_text_set(&app->f_room, str8_lit("salon"));
}

static void free_fonts(UiApp *app) {
    if (app->f_body) DeleteObject(app->f_body);
    if (app->f_small) DeleteObject(app->f_small);
    if (app->f_bold) DeleteObject(app->f_bold);
    if (app->f_title) DeleteObject(app->f_title);
    if (app->f_huge) DeleteObject(app->f_huge);
    app->f_body = app->f_small = app->f_bold = app->f_title = app->f_huge = NULL;
}

void ui_release(UiApp *app) {
    free_fonts(app);
    if (app->scratch) {
        arena_destroy(app->scratch);
        app->scratch = NULL;
    }
}

static HFONT make_font(i32 dpi, i32 pt, i32 weight) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS,
                       L"Segoe UI");
}

void ui_set_dpi(UiApp *app, i32 dpi) {
    if (dpi < 72) dpi = 96;
    free_fonts(app);
    app->dpi = dpi;
    app->f_small = make_font(dpi, 9, FW_NORMAL);
    app->f_body = make_font(dpi, 11, FW_NORMAL);
    app->f_bold = make_font(dpi, 11, FW_SEMIBOLD);
    app->f_title = make_font(dpi, 15, FW_SEMIBOLD);
    app->f_huge = make_font(dpi, 26, FW_SEMIBOLD);
}

// ------------------------------------------------------------ écran connexion ---

enum {
    ID_NONE = 0,
    ID_SERVER,
    ID_NAME,
    ID_ROOM,
    ID_PASSWORD,
    ID_CONNECT,
    ID_QUIT,
    ID_READY,
    ID_PLAYPAUSE,
    ID_SEEK,
    ID_FILE,
    ID_CHAT,
    ID_SEND,
    ID_LEAVE,
};

static void screen_connect(UiApp *a, HDC dc, i64 now_ms) {
    i32 card_w = VS_MIN(S(a, 420), a->width - S(a, 48));
    i32 pad = S(a, 24);
    i32 fh = S(a, 38);   // hauteur d'un champ
    i32 gap = S(a, 14);
    i32 card_h = S(a, 118) + 4 * (S(a, 18) + fh + gap) + S(a, 46) + S(a, 46);
    Rect card = rect((a->width - card_w) / 2, VS_MAX(S(a, 24), (a->height - card_h) / 2), card_w, card_h);
    panel(a, dc, card);

    i32 x = card.x + pad, w = card.w - 2 * pad;
    i32 y = card.y + pad;

    // Marque : une pastille d'accent puis le nom.
    i32 dot = S(a, 12);
    fill_round(dc, rect(x, y + S(a, 14), dot, dot), dot / 2, UI_ACCENT);
    draw_text(a, dc, rect(x + dot + S(a, 10), y, w, S(a, 40)), "vibesync", UI_TEXT, a->f_huge,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += S(a, 46);
    draw_text(a, dc, rect(x, y, w, S(a, 20)), "Regarder ensemble, chacun chez soi.", UI_MUTED, a->f_small,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += S(a, 34);

    struct {
        const char *lab;
        UiText *t;
        u64 id;
        const char *ph;
        b32 pwd;
    } fields[] = {
        {"Serveur", &a->f_server, ID_SERVER, "wss://vibesync.exemple.fr/ws", 0},
        {"Pseudo", &a->f_name, ID_NAME, "votre pseudo", 0},
        {"Salle", &a->f_room, ID_ROOM, "salon", 0},
        {"Mot de passe (optionnel)", &a->f_password, ID_PASSWORD, "aucun", 1},
    };
    b32 submit = 0;
    for (isize i = 0; i < VS_ARRAY_COUNT(fields); i++) {
        label(a, dc, rect(x, y, w, S(a, 18)), fields[i].lab);
        y += S(a, 18);
        if (field(a, dc, rect(x, y, w, fh), fields[i].t, fields[i].ph, fields[i].id, fields[i].pwd, now_ms)) {
            submit = 1;
        }
        y += fh + gap;
    }

    b32 busy = a->connecting;
    if (button(a, dc, rect(x, y, w, S(a, 44)), busy ? "Connexion…" : "Se connecter", ID_CONNECT, BTN_PRIMARY,
               !busy)) {
        submit = 1;
    }
    y += S(a, 44) + S(a, 12);

    if (a->status[0]) {
        draw_text(a, dc, rect(x, y, w, S(a, 40)), a->status, a->status_error ? UI_DANGER : UI_MUTED, a->f_small,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    // Entrée valide le formulaire depuis n'importe quel champ.
    if (submit && !busy) a->act_connect = 1;

    Rect foot = rect(card.x, card.y + card.h + S(a, 12), card.w, S(a, 18));
    draw_text(a, dc, foot, "Réglages mémorisés dans %APPDATA%\\vibesync.ini", UI_FAINT, a->f_small,
              DT_CENTER | DT_TOP | DT_SINGLELINE);
}

// ---------------------------------------------------------------- écran salle ---

static void draw_users(UiApp *a, HDC dc, Rect r) {
    panel(a, dc, r);
    i32 pad = S(a, 14);
    i32 x = r.x + pad, w = r.w - 2 * pad;
    i32 y = r.y + pad;
    char head[64];
    snprintf(head, sizeof(head), "Participants · %lld", (long long)a->user_count);
    draw_text(a, dc, rect(x, y, w, S(a, 20)), head, UI_TEXT, a->f_bold, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += S(a, 26);

    i32 row_h = S(a, 46);
    for (isize i = 0; i < a->user_count && y + row_h < r.y + r.h - pad; i++) {
        UiUser *u = &a->users[i];
        Rect row = rect(x, y, w, row_h - S(a, 6));
        if (u->is_self) fill_round(dc, row, S(a, 8), mix(UI_ACCENT, UI_PANEL, 210));
        i32 tx = row.x + S(a, 8);
        draw_text(a, dc, rect(tx, row.y + S(a, 4), w - S(a, 90), S(a, 18)), u->name, UI_TEXT,
                  u->is_self ? a->f_bold : a->f_body, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        const char *sub = u->has_file ? u->file : "aucun fichier";
        draw_text(a, dc, rect(tx, row.y + S(a, 21), w - S(a, 90), S(a, 16)), sub, UI_FAINT, a->f_small,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        // Badge prêt + latence, alignés à droite.
        i32 bw = S(a, 54), bh = S(a, 18);
        Rect br = rect(row.x + row.w - bw - S(a, 6), row.y + S(a, 4), bw, bh);
        if (u->ready) badge(a, dc, br, "Prêt", 0x0b1f12u, UI_OK);
        else badge(a, dc, br, "Pas prêt", UI_MUTED, UI_PANEL_HI);
        if (u->latency_ms > 0) {
            char lat[24];
            snprintf(lat, sizeof(lat), "%lld ms", (long long)u->latency_ms);
            draw_text(a, dc, rect(br.x, br.y + bh + S(a, 2), bw, S(a, 14)), lat, UI_FAINT, a->f_small,
                      DT_RIGHT | DT_TOP | DT_SINGLELINE);
        }
        y += row_h;
    }
    if (a->user_count == 0) {
        draw_text(a, dc, rect(x, y, w, S(a, 20)), "Personne d'autre pour l'instant.", UI_FAINT, a->f_small,
                  DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
}

static void draw_chat(UiApp *a, HDC dc, Rect r, i64 now_ms) {
    panel(a, dc, r);
    i32 pad = S(a, 14);
    i32 x = r.x + pad, w = r.w - 2 * pad;
    draw_text(a, dc, rect(x, r.y + pad, w, S(a, 20)), "Chat", UI_TEXT, a->f_bold,
              DT_LEFT | DT_TOP | DT_SINGLELINE);

    i32 input_h = S(a, 34);
    Rect hist = rect(x, r.y + pad + S(a, 26), w, r.h - pad * 2 - S(a, 26) - input_h - S(a, 10));

    // Molette : défilement de l'historique.
    if (rect_hit(hist, a->mouse_x, a->mouse_y) && a->wheel != 0) {
        a->chat_scroll += a->wheel / 120;
        if (a->chat_scroll < 0) a->chat_scroll = 0;
        if (a->chat_scroll > a->chat_count) a->chat_scroll = a->chat_count;
    }

    // Les lignes sont mesurées puis empilées du bas vers le haut.
    HGDIOBJ of = SelectObject(dc, a->f_body);
    i32 y = hist.y + hist.h;
    isize last = a->chat_count - a->chat_scroll;
    for (isize i = last - 1; i >= 0 && y > hist.y; i--) {
        UiChatLine *l = &a->chat[i];
        Str8 line;
        if (l->system) {
            line = str8_from_cstr(l->text);
        } else {
            Builder b;
            builder_init(&b, a->scratch, 256);
            builder_cstr(&b, l->from);
            builder_cstr(&b, " · ");
            builder_cstr(&b, l->text);
            line = builder_result(&b);
        }
        isize n = 0;
        const wchar_t *wtext = wide(a, line, &n);
        RECT calc = {0, 0, hist.w, 0};
        DrawTextW(dc, wtext, (int)n, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        i32 lh = calc.bottom - calc.top + S(a, 6);
        y -= lh;
        if (y + lh < hist.y) break;
        Rect lr = rect(hist.x, y, hist.w, lh);
        if (l->system) {
            draw_text_rect(a, dc, lr, line, UI_FAINT, a->f_small, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        } else {
            i32 name_w = text_width(a, dc, str8_from_cstr(l->from), a->f_bold);
            draw_text(a, dc, rect(lr.x, lr.y, name_w, lh), l->from, UI_ACCENT_HI, a->f_bold,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            draw_text(a, dc, rect(lr.x + name_w + S(a, 6), lr.y, lr.w - name_w - S(a, 6), lh), l->text, UI_TEXT,
                      a->f_body, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
    }
    SelectObject(dc, of);
    if (a->chat_count == 0) {
        draw_text(a, dc, hist, "Dites bonjour !", UI_FAINT, a->f_small, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    i32 send_w = S(a, 76);
    Rect input = rect(x, r.y + r.h - pad - input_h, w - send_w - S(a, 8), input_h);
    b32 sent = field(a, dc, input, &a->f_chat, "Écrire un message…", ID_CHAT, 0, now_ms);
    if (button(a, dc, rect(input.x + input.w + S(a, 8), input.y, send_w, input_h), "Envoyer", ID_SEND,
               BTN_SUBTLE, a->f_chat.len > 0)) {
        sent = 1;
    }
    if (sent && a->f_chat.len > 0) a->act_chat_send = 1;
}

static void draw_transport(UiApp *a, HDC dc, Rect r, i64 now_ms) {
    VS_UNUSED(now_ms);
    panel(a, dc, r);
    i32 pad = S(a, 16);
    i32 x = r.x + pad, w = r.w - 2 * pad;
    i32 y = r.y + pad;

    // Ligne fichier + bouton de sélection.
    i32 btn_w = S(a, 150);
    const char *fname = a->file_name[0] ? a->file_name : "Aucun fichier ouvert";
    draw_text(a, dc, rect(x, y, w - btn_w - S(a, 12), S(a, 20)), fname, a->file_name[0] ? UI_TEXT : UI_FAINT,
              a->f_bold, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    char sub[96];
    if (a->vlc_running) {
        snprintf(sub, sizeof(sub), "VLC piloté · %s", a->buffering ? "mise en cache…" : "prêt");
    } else {
        snprintf(sub, sizeof(sub), "Ouvrez votre copie du média pour lancer VLC");
    }
    draw_text(a, dc, rect(x, y + S(a, 20), w - btn_w - S(a, 12), S(a, 16)), sub, UI_FAINT, a->f_small,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (button(a, dc, rect(x + w - btn_w, y, btn_w, S(a, 34)), "Choisir un fichier…", ID_FILE, BTN_GHOST, 1)) {
        a->act_open_file = 1;
    }
    y += S(a, 48);

    // Transport : bouton lecture/pause + barre de position.
    i32 pb = S(a, 44);
    Rect play = rect(x, y, pb, pb);
    b32 can_play = a->vlc_running;
    if (button(a, dc, play, "", ID_PLAYPAUSE, BTN_PRIMARY, can_play)) {
        if (a->paused) a->act_play = 1;
        else a->act_pause = 1;
    }
    i32 gs = S(a, 16);
    glyph_play(dc, rect(play.x + (pb - gs) / 2 + (a->paused ? S(a, 2) : 0), play.y + (pb - gs) / 2, gs, gs),
               can_play ? 0xffffffu : UI_FAINT, a->paused);

    char cur[24], tot[24];
    ui_format_time(a->position_sec, cur, sizeof(cur));
    ui_format_time(a->duration_sec, tot, sizeof(tot));
    i32 time_w = S(a, 116);
    Rect bar = rect(play.x + pb + S(a, 14), y, w - pb - S(a, 14) - time_w, pb);
    f64 seek_to = 0;
    if (seekbar(a, dc, bar, a->position_sec, a->duration_sec, ID_SEEK, &seek_to)) {
        a->act_seek = 1;
        a->act_seek_pos = seek_to;
    }
    char times[56];
    snprintf(times, sizeof(times), "%s / %s", cur, tot);
    draw_text(a, dc, rect(bar.x + bar.w + S(a, 10), y, time_w - S(a, 10), pb), times, UI_MUTED, a->f_small,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_header(UiApp *a, HDC dc, Rect r) {
    fill_rect(dc, r, UI_BG);
    i32 pad = S(a, 20);
    i32 y = r.y;
    draw_text(a, dc, rect(r.x + pad, y, S(a, 200), r.h), "vibesync", UI_TEXT, a->f_title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    i32 name_w = text_width(a, dc, str8_lit("vibesync"), a->f_title);
    char room[96];
    snprintf(room, sizeof(room), "· %s", a->room);
    draw_text(a, dc, rect(r.x + pad + name_w + S(a, 10), y, S(a, 260), r.h), room, UI_MUTED, a->f_body,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // À droite : état de connexion, latence, dérive, bouton quitter.
    i32 bw = S(a, 92), bh = S(a, 32);
    Rect leave = rect(r.x + r.w - pad - bw, y + (r.h - bh) / 2, bw, bh);
    if (button(a, dc, leave, "Quitter", ID_LEAVE, BTN_DANGER, 1)) a->act_disconnect = 1;

    i32 rx = leave.x - S(a, 12);
    if (a->drift_sec != 0 || a->phase == VS_PHASE_CONNECTED) {
        char drift[40];
        f64 d = a->drift_sec;
        snprintf(drift, sizeof(drift), "dérive %s%.2f s", d >= 0 ? "+" : "−", d < 0 ? -d : d);
        u32 col = UI_FAINT;
        f64 ad = d < 0 ? -d : d;
        if (ad > 2) col = UI_DANGER;
        else if (ad > 0.1) col = UI_WARN;
        else col = UI_OK;
        i32 tw = text_width(a, dc, str8_from_cstr(drift), a->f_small) + S(a, 20);
        Rect br = rect(rx - tw, y + (r.h - S(a, 22)) / 2, tw, S(a, 22));
        badge(a, dc, br, drift, col, UI_PANEL);
        rx = br.x - S(a, 8);
    }
    if (a->latency_ms > 0) {
        char lat[32];
        snprintf(lat, sizeof(lat), "%lld ms", (long long)a->latency_ms);
        i32 tw = text_width(a, dc, str8_from_cstr(lat), a->f_small) + S(a, 20);
        Rect br = rect(rx - tw, y + (r.h - S(a, 22)) / 2, tw, S(a, 22));
        badge(a, dc, br, lat, UI_MUTED, UI_PANEL);
        rx = br.x - S(a, 8);
    }
    {
        const char *state = "Connecté";
        u32 fg = UI_OK, bg = mix(UI_OK, UI_BG, 210);
        if (a->phase != VS_PHASE_CONNECTED) {
            state = a->retrying ? "Reconnexion…" : "Connexion…";
            fg = UI_WARN;
            bg = mix(UI_WARN, UI_BG, 210);
        }
        i32 tw = text_width(a, dc, str8_from_cstr(state), a->f_small) + S(a, 20);
        Rect br = rect(rx - tw, y + (r.h - S(a, 22)) / 2, tw, S(a, 22));
        badge(a, dc, br, state, fg, bg);
    }
    fill_rect(dc, rect(r.x, r.y + r.h - 1, r.w, 1), UI_LINE);
}

static void screen_room(UiApp *a, HDC dc, i64 now_ms) {
    i32 pad = S(a, 16);
    i32 header_h = S(a, 60);
    draw_header(a, dc, rect(0, 0, a->width, header_h));

    i32 left_w = VS_MIN(S(a, 300), a->width / 3);
    i32 top = header_h + pad;
    i32 bottom = a->height - pad;

    // Colonne gauche : participants + gros bouton Prêt.
    i32 ready_h = S(a, 52);
    Rect users = rect(pad, top, left_w, bottom - top - ready_h - S(a, 12));
    draw_users(a, dc, users);
    Rect ready = rect(pad, users.y + users.h + S(a, 12), left_w, ready_h);
    if (button(a, dc, ready, a->ready ? "✓ Prêt" : "Je suis prêt", ID_READY, a->ready ? BTN_ON : BTN_GHOST, 1)) {
        a->act_ready = 1;
    }

    // Colonne droite : média + transport, puis chat.
    i32 rx = pad + left_w + pad;
    i32 rw = a->width - rx - pad;
    i32 transport_h = S(a, 148);
    draw_transport(a, dc, rect(rx, top, rw, transport_h), now_ms);
    draw_chat(a, dc, rect(rx, top + transport_h + pad, rw, bottom - top - transport_h - pad), now_ms);
}

// ------------------------------------------------------------------- frame ---

static void draw_toast(UiApp *a, HDC dc, i64 now_ms) {
    if (a->toast[0] == 0 || now_ms >= a->toast_until_ms) return;
    i32 tw = text_width(a, dc, str8_from_cstr(a->toast), a->f_body) + S(a, 40);
    tw = VS_MIN(tw, a->width - S(a, 40));
    i32 th = S(a, 40);
    i32 top = S(a, 14);
    if (a->screen == UI_SCREEN_ROOM) {
        // En salle, le bandeau se loge dans l'espace libre de l'en-tête : il
        // ne masque jamais le contenu.
        th = S(a, 26);
        top = (S(a, 60) - th) / 2;
        tw = VS_MIN(tw, a->width * 2 / 5);
    }
    Rect r = rect((a->width - tw) / 2, top, tw, th);
    u32 bg = UI_PANEL_HI, fg = UI_TEXT;
    if (a->toast_level == 1) {
        bg = mix(UI_WARN, UI_PANEL, 200);
        fg = UI_WARN;
    } else if (a->toast_level >= 2) {
        bg = mix(UI_DANGER, UI_PANEL, 200);
        fg = UI_DANGER;
    }
    i32 radius = VS_MIN(S(a, 10), r.h / 2);
    if (a->screen != UI_SCREEN_ROOM) {
        // Ombre portée légère : le bandeau flotte au-dessus du contenu.
        fill_round(dc, rect(r.x + S(a, 2), r.y + S(a, 3), r.w, r.h), radius, mix(UI_BG, 0x000000u, 120));
    }
    fill_round(dc, r, radius, bg);
    stroke_round(dc, r, radius, mix(fg, bg, 160), 1);
    draw_text(a, dc, r, a->toast, fg, a->f_small, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void ui_frame(UiApp *app, HDC dc, i32 w, i32 h, i64 now_ms) {
    arena_reset(app->scratch);
    app->width = w;
    app->height = h;
    app->hot = 0;
    if (!app->f_body) ui_set_dpi(app, app->dpi);

    fill_rect(dc, rect(0, 0, w, h), UI_BG);

    // Échap : on lâche le champ courant et on efface le bandeau.
    if (key_pressed(app, VK_ESCAPE)) {
        app->focus = 0;
        app->toast_until_ms = 0;
        app->key_count = 0;
    }

    if (app->screen == UI_SCREEN_CONNECT) screen_connect(app, dc, now_ms);
    else screen_room(app, dc, now_ms);

    draw_toast(app, dc, now_ms);

    // Le curseur main sur les zones cliquables : petit détail, grand effet.
    SetCursor(LoadCursorW(NULL, app->hot ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));

    // Rafraîchissement périodique seulement si quelque chose bouge.
    app->need_timer = (app->screen == UI_SCREEN_ROOM && !app->paused && app->vlc_running) || app->focus != 0 ||
                      (app->toast[0] != 0 && now_ms < app->toast_until_ms) || app->connecting;

    // Fin de frame : les entrées non consommées sont oubliées.
    app->mouse_pressed = 0;
    app->mouse_released = 0;
    app->wheel = 0;
    app->char_count = 0;
    app->key_count = 0;
}
