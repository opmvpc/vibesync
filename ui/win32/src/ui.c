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

// ------------------------------------------------------- anticrénelage ---
//
// GDI ne sait pas anticréneler : un engrenage de 20 px tracé au Polygon donne
// des dents en escalier, une coche au Polyline donne un zigzag de marches.
// Recette maison, sans la moindre bibliothèque : le glyphe est tracé UI_AA_SS
// fois plus grand, en blanc sur noir, dans un DIB de masque ; la moyenne des
// UI_AA_SS² échantillons d'un pixel destination donne sa couverture, dont on
// fait le canal alpha d'une image prémultipliée que GdiAlphaBlend compose sur le
// fond réel (donc quel que soit ce fond, y compris à travers un évidement).
//
// Coût : deux DIB éphémères et un balayage de quelques centaines de pixels par
// glyphe — négligeable pour une interface qui ne redessine qu'à l'événement.
#define UI_AA_SS 4
#define UI_AA_MAX 128  // côté maximal, en pixels destination, d'un glyphe anticrénelé

typedef struct {
    HDC dc;        // DC du masque supersamplé
    HBITMAP bmp;
    HGDIOBJ old;
    u8 *px;        // BGRX du masque ; seul l'octet bleu est lu
    i32 w, h;      // dimensions du masque = destination × UI_AA_SS
    Rect dst;
} AaMask;

// dib_create alloue un DIB 32 bits descendant (origine en haut à gauche).
static HBITMAP dib_create(HDC ref, i32 w, i32 h, void **bits) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(ref, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

static b32 aa_begin(AaMask *m, HDC dc, Rect dst) {
    memset(m, 0, sizeof(*m));
    if (dst.w <= 0 || dst.h <= 0 || dst.w > UI_AA_MAX || dst.h > UI_AA_MAX) return 0;
    void *bits = NULL;
    m->dst = dst;
    m->w = dst.w * UI_AA_SS;
    m->h = dst.h * UI_AA_SS;
    m->bmp = dib_create(dc, m->w, m->h, &bits);
    if (!m->bmp) return 0;
    m->dc = CreateCompatibleDC(dc);
    if (!m->dc) {
        DeleteObject(m->bmp);
        m->bmp = NULL;
        return 0;
    }
    m->px = (u8 *)bits;  // CreateDIBSection rend la surface déjà à zéro : fond noir
    m->old = SelectObject(m->dc, m->bmp);
    return 1;
}

// aa_shape trace une forme pleine dans le masque : `on` ajoute de la couverture,
// sinon il en retire (évidement — le moyeu de l'engrenage, par exemple).
static void aa_shape(AaMask *m, const POINT *poly, int n, const Rect *disc, b32 on) {
    COLORREF ink = on ? RGB(255, 255, 255) : RGB(0, 0, 0);
    HBRUSH br = CreateSolidBrush(ink);
    HPEN pen = CreatePen(PS_SOLID, 1, ink);
    HGDIOBJ ob = SelectObject(m->dc, br);
    HGDIOBJ op = SelectObject(m->dc, pen);
    if (poly) Polygon(m->dc, poly, n);
    if (disc) Ellipse(m->dc, disc->x, disc->y, disc->x + disc->w, disc->y + disc->h);
    SelectObject(m->dc, ob);
    SelectObject(m->dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// aa_stroke trace une polyligne épaisse, bouts et coudes arrondis.
static void aa_stroke(AaMask *m, const POINT *pts, int n, i32 width) {
    LOGBRUSH lb;
    lb.lbStyle = BS_SOLID;
    lb.lbColor = RGB(255, 255, 255);
    lb.lbHatch = 0;
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)width, &lb, 0,
                            NULL);
    if (!pen) return;
    HGDIOBJ op = SelectObject(m->dc, pen);
    Polyline(m->dc, pts, n);
    SelectObject(m->dc, op);
    DeleteObject(pen);
}

// aa_end réduit le masque (moyenne de blocs UI_AA_SS × UI_AA_SS), compose la
// couleur demandée sur `dc`, puis libère tout.
static void aa_end(AaMask *m, HDC dc, u32 color) {
    if (!m->bmp) return;
    GdiFlush();  // les tracés doivent être visibles dans le DIB avant relecture
    void *bits = NULL;
    HBITMAP out = dib_create(dc, m->dst.w, m->dst.h, &bits);
    HDC odc = out ? CreateCompatibleDC(dc) : NULL;
    if (odc) {
        HGDIOBJ oo = SelectObject(odc, out);
        u8 *o = (u8 *)bits;
        i32 sr = (i32)((color >> 16) & 0xff), sg = (i32)((color >> 8) & 0xff), sb = (i32)(color & 0xff);
        for (i32 y = 0; y < m->dst.h; y++) {
            for (i32 x = 0; x < m->dst.w; x++) {
                i32 sum = 0;
                for (i32 j = 0; j < UI_AA_SS; j++) {
                    const u8 *row = m->px + ((isize)(y * UI_AA_SS + j) * m->w + (isize)x * UI_AA_SS) * 4;
                    for (i32 i = 0; i < UI_AA_SS; i++) sum += row[i * 4];
                }
                i32 cov = sum / (UI_AA_SS * UI_AA_SS);
                u8 *d = o + ((isize)y * m->dst.w + x) * 4;
                d[0] = (u8)(sb * cov / 255);  // prémultiplié, comme l'exige AC_SRC_ALPHA
                d[1] = (u8)(sg * cov / 255);
                d[2] = (u8)(sr * cov / 255);
                d[3] = (u8)cov;
            }
        }
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        GdiAlphaBlend(dc, m->dst.x, m->dst.y, m->dst.w, m->dst.h, odc, 0, 0, m->dst.w, m->dst.h, bf);
        SelectObject(odc, oo);
        DeleteDC(odc);
    }
    if (out) DeleteObject(out);
    SelectObject(m->dc, m->old);
    DeleteDC(m->dc);
    DeleteObject(m->bmp);
    memset(m, 0, sizeof(*m));
}

// sin_128 : sinus en millièmes, sur un tour découpé en 128 pas. Table du premier
// quadrant plus symétries — de l'entier partout, aucune dépendance à math.h.
static i32 sin_128(i32 k) {
    static const i16 Q[33] = {0,   49,  98,  147, 195, 243, 290, 337, 383, 428, 471,
                              514, 556, 596, 634, 672, 707, 741, 773, 803, 831, 858,
                              882, 904, 924, 942, 957, 970, 981, 989, 995, 999, 1000};
    k &= 127;
    if (k <= 32) return Q[k];
    if (k <= 64) return Q[64 - k];
    if (k <= 96) return -Q[k - 64];
    return -Q[128 - k];
}

static i32 cos_128(i32 k) { return sin_128(k + 32); }

// fill_circle : disque anticrénelé (pastilles d'état). Repli sur RoundRect si le
// masque n'a pas pu être alloué — mieux vaut créneler que ne rien dessiner.
static void fill_circle(HDC dc, Rect r, u32 color) {
    AaMask m;
    if (!aa_begin(&m, dc, r)) {
        fill_round(dc, r, VS_MIN(r.w, r.h) / 2, color);
        return;
    }
    i32 d = VS_MIN(m.w, m.h);
    Rect disc = rect((m.w - d) / 2, (m.h - d) / 2, d, d);
    aa_shape(&m, NULL, 0, &disc, 1);
    aa_end(&m, dc, color);
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
    t->anchor = n;
    t->scroll = 0;
}

Str8 ui_text_str(const UiText *t) {
    Str8 s = {(u8 *)t->data, t->len};
    return s;
}

static isize clampi(isize v, isize lo, isize hi) { return v < lo ? lo : (v > hi ? hi : v); }

isize ui_text_prev_cp(const UiText *t, isize i) {
    i = clampi(i, 0, t->len);
    if (i <= 0) return 0;
    i--;
    while (i > 0 && (t->data[i] & 0xc0) == 0x80) i--;
    return i;
}

isize ui_text_next_cp(const UiText *t, isize i) {
    i = clampi(i, 0, t->len);
    if (i >= t->len) return t->len;
    i++;
    while (i < t->len && (t->data[i] & 0xc0) == 0x80) i++;
    return i;
}

// align ramène un offset arbitraire sur la frontière de caractère précédente :
// aucune opération ne peut couper un caractère en deux, même sur un clic.
static isize align_cp(const UiText *t, isize i) {
    i = clampi(i, 0, t->len);
    while (i > 0 && i < t->len && (t->data[i] & 0xc0) == 0x80) i--;
    return i;
}

isize ui_text_sel_lo(const UiText *t) { return t->caret < t->anchor ? t->caret : t->anchor; }
isize ui_text_sel_hi(const UiText *t) { return t->caret > t->anchor ? t->caret : t->anchor; }
b32 ui_text_has_sel(const UiText *t) { return t->caret != t->anchor; }

Str8 ui_text_selection(const UiText *t) {
    isize lo = ui_text_sel_lo(t), hi = ui_text_sel_hi(t);
    Str8 s = {(u8 *)t->data + lo, hi - lo};
    return s;
}

void ui_text_move(UiText *t, isize pos, b32 extend) {
    t->caret = align_cp(t, pos);
    if (!extend) t->anchor = t->caret;
}

void ui_text_select_range(UiText *t, isize lo, isize hi) {
    t->anchor = align_cp(t, lo);
    t->caret = align_cp(t, hi);
}

void ui_text_select_all(UiText *t) {
    t->anchor = 0;
    t->caret = t->len;
}

void ui_text_clear_sel(UiText *t) { t->anchor = t->caret; }

b32 ui_text_delete_sel(UiText *t) {
    isize lo = ui_text_sel_lo(t), hi = ui_text_sel_hi(t);
    if (hi <= lo) return 0;
    memmove(t->data + lo, t->data + hi, (size_t)(t->len - hi));
    t->len -= hi - lo;
    t->caret = t->anchor = lo;
    t->data[t->len] = 0;
    return 1;
}

// text_put_bytes insère des octets déjà validés à la position du caret.
static b32 text_put_bytes(UiText *t, const u8 *bytes, isize n) {
    if (n <= 0 || t->len + n >= UI_TEXT_CAP) return 0;
    memmove(t->data + t->caret + n, t->data + t->caret, (size_t)(t->len - t->caret));
    memcpy(t->data + t->caret, bytes, (size_t)n);
    t->len += n;
    t->caret += n;
    t->anchor = t->caret;
    t->data[t->len] = 0;
    return 1;
}

void ui_text_insert_cp(UiText *t, u32 cp) {
    ui_text_delete_sel(t);
    u8 buf[4];
    isize n = utf8_encode(cp, buf);
    text_put_bytes(t, buf, n);
}

void ui_text_insert_str(UiText *t, Str8 s) {
    ui_text_delete_sel(t);
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
        if (!text_put_bytes(t, s.data + i, n)) break;  // champ plein : on s'arrête net
        i += n;
    }
}

// --- mots : trois classes, comme les champs de saisie de Windows ---

static int word_class(u8 c) {
    if (c == ' ' || c == '\t') return 0;
    if (c >= 0x80) return 1;  // lettres accentuées et autres caractères non ASCII
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return 1;
    return 2;
}

void ui_text_word_bounds(const UiText *t, isize pos, isize *lo, isize *hi) {
    isize p = align_cp(t, pos);
    if (t->len == 0) {
        *lo = *hi = 0;
        return;
    }
    // Un double-clic après le dernier caractère sélectionne le dernier mot.
    if (p >= t->len) p = ui_text_prev_cp(t, t->len);
    int cls = word_class(t->data[p]);
    isize a = p;
    while (a > 0) {
        isize prev = ui_text_prev_cp(t, a);
        if (word_class(t->data[prev]) != cls) break;
        a = prev;
    }
    isize b = ui_text_next_cp(t, p);
    while (b < t->len && word_class(t->data[b]) == cls) b = ui_text_next_cp(t, b);
    *lo = a;
    *hi = b;
}

isize ui_text_word_left(const UiText *t, isize pos) {
    isize i = align_cp(t, pos);
    while (i > 0 && word_class(t->data[ui_text_prev_cp(t, i)]) == 0) i = ui_text_prev_cp(t, i);
    if (i == 0) return 0;
    int cls = word_class(t->data[ui_text_prev_cp(t, i)]);
    while (i > 0 && word_class(t->data[ui_text_prev_cp(t, i)]) == cls) i = ui_text_prev_cp(t, i);
    return i;
}

isize ui_text_word_right(const UiText *t, isize pos) {
    isize i = align_cp(t, pos);
    if (i >= t->len) return t->len;
    int cls = word_class(t->data[i]);
    while (i < t->len && word_class(t->data[i]) == cls) i = ui_text_next_cp(t, i);
    while (i < t->len && word_class(t->data[i]) == 0) i = ui_text_next_cp(t, i);
    return i;
}

void ui_text_backspace(UiText *t, b32 word) {
    if (ui_text_delete_sel(t)) return;
    isize from = word ? ui_text_word_left(t, t->caret) : ui_text_prev_cp(t, t->caret);
    ui_text_select_range(t, from, t->caret);
    ui_text_delete_sel(t);
}

void ui_text_delete_fwd(UiText *t, b32 word) {
    if (ui_text_delete_sel(t)) return;
    isize to = word ? ui_text_word_right(t, t->caret) : ui_text_next_cp(t, t->caret);
    ui_text_select_range(t, t->caret, to);
    ui_text_delete_sel(t);
}

// --- hit-test ---
//
// Les largeurs de préfixe croissent avec l'offset : une recherche dichotomique
// suffit, ce qui limite à ~9 mesures GDI par clic au lieu d'une par caractère.

static isize cp_offset(const UiText *t, isize index) {
    isize i = 0;
    while (index > 0 && i < t->len) {
        i = ui_text_next_cp(t, i);
        index--;
    }
    return i;
}

static isize cp_count(const UiText *t) {
    isize n = 0;
    for (isize i = 0; i < t->len; i++) {
        if ((t->data[i] & 0xc0) != 0x80) n++;
    }
    return n;
}

isize ui_text_hit(const UiText *t, const UiTextMetrics *m, i32 x) {
    if (t->len == 0 || x <= 0) return 0;
    isize n = cp_count(t);
    isize lo = 0, hi = n;  // plus grand k tel que width(k) <= x
    while (lo < hi) {
        isize mid = (lo + hi + 1) / 2;
        if (m->prefix_width(m->ctx, t, cp_offset(t, mid)) <= x) lo = mid;
        else hi = mid - 1;
    }
    if (lo >= n) return t->len;
    isize off0 = cp_offset(t, lo), off1 = cp_offset(t, lo + 1);
    i32 w0 = m->prefix_width(m->ctx, t, off0), w1 = m->prefix_width(m->ctx, t, off1);
    // On bascule sur le caractère suivant passé la moitié de sa largeur.
    return (x - w0 > w1 - x) ? off1 : off0;
}

// --- presse-papiers ---

static void clip_copy(UiApp *a, Str8 s) {
    if (s.len == 0 || !a->hwnd) return;
    isize n = 0;
    const u16 *w = (const u16 *)utf8_to_utf16(a->scratch, s, &n);
    if (!OpenClipboard(a->hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * sizeof(u16));
    if (h) {
        u16 *dst = (u16 *)GlobalLock(h);
        if (dst) {
            memcpy(dst, w, (size_t)n * sizeof(u16));
            dst[n] = 0;
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

static void clip_paste(UiApp *a, UiText *t) {
    if (!a->hwnd || !OpenClipboard(a->hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const u16 *w = (const u16 *)GlobalLock(h);
        if (w) {
            TempArena tmp = temp_begin(a->scratch);
            ui_text_insert_str(t, utf16_to_utf8(a->scratch, w));
            temp_end(tmp);
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

void ui_on_mouse_double(UiApp *app, i32 x, i32 y) {
    app->mouse_x = x;
    app->mouse_y = y;
    app->mouse_down = 1;
    app->mouse_pressed = 1;
    app->mouse_double = 1;
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

void ui_on_key(UiApp *app, u32 vk, u32 mods) {
    if (app->key_count < VS_ARRAY_COUNT(app->keys)) {
        app->keys[app->key_count].vk = vk;
        app->keys[app->key_count].mods = mods;
        app->key_count++;
    }
}

static b32 key_pressed(UiApp *app, u32 vk) {
    for (isize i = 0; i < app->key_count; i++) {
        if (app->keys[i].vk == vk) return 1;
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
    b32 hover = enabled && !a->input_locked && rect_hit(r, a->mouse_x, a->mouse_y);
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
    if (show_play) {
        // Les deux flancs du triangle sont obliques : sans anticrénelage, ils
        // montent en marches d'escalier bien visibles sur un bouton violet.
        AaMask m;
        if (aa_begin(&m, dc, r)) {
            POINT p[3] = {
                {m.w / 6, 0},
                {m.w - m.w / 8, m.h / 2},
                {m.w / 6, m.h},
            };
            aa_shape(&m, p, 3, NULL, 1);
            aa_end(&m, dc, color);
            return;
        }
    }
    // Pause : deux barres à angles droits, que GDI rend déjà parfaitement.
    HBRUSH br = CreateSolidBrush(cr(color));
    HPEN pen = CreatePen(PS_SOLID, 1, cr(color));
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, pen);
    if (show_play) {
        POINT p[3] = {
            {r.x + r.w / 6, r.y},
            {r.x + r.w - r.w / 8, r.y + r.h / 2},
            {r.x + r.w / 6, r.y + r.h},
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

// --- champ de saisie ---
//
// FieldMx branche le modèle d'édition sur les vraies métriques de la police.
// Pour un champ masqué, tous les points « • » ont la même largeur : on la
// mesure une fois et on multiplie, ce qui évite de reconstruire une chaîne de
// points à chaque appel du hit-test.
typedef struct {
    UiApp *a;
    HDC dc;
    b32 password;
    i32 bullet_w;
} FieldMx;

static i32 field_prefix_width(void *ctx, const UiText *t, isize byte_off) {
    FieldMx *m = (FieldMx *)ctx;
    if (byte_off <= 0) return 0;
    if (m->password) {
        isize dots = 0;
        for (isize i = 0; i < byte_off && i < t->len; i++) {
            if ((t->data[i] & 0xc0) != 0x80) dots++;
        }
        return (i32)dots * m->bullet_w;
    }
    TempArena tmp = temp_begin(m->a->scratch);
    Str8 s = {(u8 *)t->data, VS_MIN(byte_off, t->len)};
    i32 w = text_width(m->a, m->dc, s, m->a->f_body);
    temp_end(tmp);
    return w;
}

// bullets construit la chaîne de « • » affichée à la place d'un mot de passe.
static Str8 bullets(UiApp *a, isize count) {
    u8 *buf = arena_push_array(a->scratch, u8, count * 3 + 1);
    isize n = 0;
    for (isize i = 0; i < count; i++) n += utf8_encode(0x2022, buf + n);
    return str8(buf, n);
}

// glyph_gear dessine un engrenage à 8 dents. L'ancienne version alternait deux
// rayons tous les 22,5° : ça ne fait pas des dents mais des pointes, d'où
// l'étoile à huit branches qu'on voyait à la place d'une roue. Une vraie dent
// demande quatre sommets — pied, sommet, sommet, pied — donc un flanc incliné et
// un plat en tête. Sur 128 pas de tour, la denture fait 16 pas : le plat occupe
// ±3, le pied ±5, ce qui laisse 6 pas de creux entre deux dents.
//
// Le moyeu est un évidement (couverture retirée) et non un disque de la couleur
// du fond : le bouton peut changer de teinte au survol sans que la roue s'en
// aperçoive.
static void glyph_gear(HDC dc, Rect r, u32 color) {
    AaMask m;
    if (!aa_begin(&m, dc, r)) return;
    i32 ro = VS_MIN(m.w, m.h) / 2;
    i32 rr = ro * 74 / 100;  // rayon de pied
    i32 cx = m.w / 2, cy = m.h / 2;
    static const i32 OFF[4] = {-5, -3, 3, 5};
    POINT p[32];
    for (i32 t = 0; t < 8; t++) {
        for (i32 i = 0; i < 4; i++) {
            i32 k = t * 16 + OFF[i];
            i32 rad = (i == 1 || i == 2) ? ro : rr;
            p[t * 4 + i].x = cx + cos_128(k) * rad / 1000;
            p[t * 4 + i].y = cy + sin_128(k) * rad / 1000;
        }
    }
    aa_shape(&m, p, 32, NULL, 1);
    i32 hub = ro * 36 / 100;
    Rect hole = rect(cx - hub, cy - hub, 2 * hub, 2 * hub);
    aa_shape(&m, NULL, 0, &hole, 0);
    aa_end(&m, dc, color);
}

// field dessine un champ de saisie. Renvoie 1 si Entrée a été pressée dedans.
static b32 field(UiApp *a, HDC dc, Rect r, UiText *t, const char *placeholder, u64 id, b32 password,
                 i64 now_ms) {
    if (a->take_next_focus && !a->input_locked) {
        a->focus = id;
        a->take_next_focus = 0;
        a->caret_blink_ms = now_ms;
        ui_text_select_all(t);
    }
    i32 pad = S(a, 10);
    Rect inner = rect(r.x + pad, r.y, r.w - 2 * pad, r.h);
    if (++a->field_seq == a->probe_index) {
        a->probe_x = r.x;
        a->probe_y = r.y;
        a->probe_w = r.w;
        a->probe_h = r.h;
    }

    FieldMx mx = {a, dc, password, 0};
    if (password) mx.bullet_w = text_width(a, dc, bullets(a, 1), a->f_body);
    UiTextMetrics metrics = {field_prefix_width, &mx};

    b32 hover = !a->input_locked && rect_hit(r, a->mouse_x, a->mouse_y);
    if (hover) {
        a->hot = id;
        a->hot_is_text = 1;
    }

    // --- souris : clic pour placer le caret, drag pour sélectionner ---
    if (!a->input_locked && a->mouse_pressed) {
        if (hover) {
            a->focus = id;
            a->caret_blink_ms = now_ms;
            isize pos = ui_text_hit(t, &metrics, a->mouse_x - inner.x + t->scroll);
            if (a->mouse_double) {
                isize lo, hi;
                ui_text_word_bounds(t, pos, &lo, &hi);
                ui_text_select_range(t, lo, hi);
            } else {
                ui_text_move(t, pos, 0);
                a->active = id;
                a->dragging_text = 1;
            }
        } else if (a->focus == id) {
            a->focus = 0;
        }
    }
    if (a->active == id && a->dragging_text) {
        if (a->mouse_down) {
            ui_text_move(t, ui_text_hit(t, &metrics, a->mouse_x - inner.x + t->scroll), 1);
            a->caret_blink_ms = now_ms;
        }
        if (a->mouse_released) {
            a->active = 0;
            a->dragging_text = 0;
        }
    }

    b32 focused = a->focus == id;
    b32 submitted = 0;
    if (focused && !a->input_locked) {
        for (isize i = 0; i < a->char_count; i++) ui_text_insert_cp(t, a->chars[i]);
        if (a->char_count > 0) a->caret_blink_ms = now_ms;
        a->char_count = 0;
        for (isize i = 0; i < a->key_count; i++) {
            u32 vk = a->keys[i].vk;
            b32 ctrl = (a->keys[i].mods & UI_MOD_CTRL) != 0;
            b32 shift = (a->keys[i].mods & UI_MOD_SHIFT) != 0;
            switch (vk) {
                case VK_BACK: ui_text_backspace(t, ctrl); break;
                case VK_DELETE: ui_text_delete_fwd(t, ctrl); break;
                case VK_LEFT:
                    if (!shift && ui_text_has_sel(t) && !ctrl) ui_text_move(t, ui_text_sel_lo(t), 0);
                    else ui_text_move(t, ctrl ? ui_text_word_left(t, t->caret) : ui_text_prev_cp(t, t->caret), shift);
                    break;
                case VK_RIGHT:
                    if (!shift && ui_text_has_sel(t) && !ctrl) ui_text_move(t, ui_text_sel_hi(t), 0);
                    else ui_text_move(t, ctrl ? ui_text_word_right(t, t->caret) : ui_text_next_cp(t, t->caret), shift);
                    break;
                case VK_HOME: ui_text_move(t, 0, shift); break;
                case VK_END: ui_text_move(t, t->len, shift); break;
                case VK_RETURN: submitted = 1; break;
                case VK_TAB:
                    a->focus = 0;
                    a->take_next_focus = 1;
                    break;
                case 'A':
                    if (ctrl) ui_text_select_all(t);
                    break;
                case 'C':
                    // Un mot de passe se sélectionne (pour l'effacer), jamais ne se copie.
                    if (ctrl && !password) clip_copy(a, ui_text_selection(t));
                    break;
                case 'X':
                    if (ctrl && !password) {
                        clip_copy(a, ui_text_selection(t));
                        ui_text_delete_sel(t);
                    }
                    break;
                case 'V':
                    if (ctrl) clip_paste(a, t);
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

    // Contenu affiché : les mots de passe sont masqués par des points.
    Str8 shown = password && t->len > 0 ? bullets(a, cp_count(t)) : ui_text_str(t);

    if (shown.len == 0 && !focused) {
        draw_text(a, dc, inner, placeholder, UI_FAINT, a->f_body, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        t->scroll = 0;
        return submitted;
    }

    // Défilement horizontal : le caret reste visible, y compris pendant un drag
    // qui sort du champ.
    i32 caret_x = field_prefix_width(&mx, t, t->caret);
    i32 total = field_prefix_width(&mx, t, t->len);
    i32 slack = S(a, 2);
    if (total <= inner.w) {
        t->scroll = 0;
    } else {
        if (caret_x - t->scroll < 0) t->scroll = caret_x;
        if (caret_x - t->scroll > inner.w - slack) t->scroll = caret_x - (inner.w - slack);
        i32 max_scroll = total - inner.w + slack;
        if (t->scroll > max_scroll) t->scroll = max_scroll;
        if (t->scroll < 0) t->scroll = 0;
    }

    HRGN clip = CreateRectRgn(inner.x, inner.y, inner.x + inner.w, inner.y + inner.h);
    SelectClipRgn(dc, clip);
    i32 ch = text_height(a, dc, a->f_body);
    i32 cy = r.y + (r.h - ch) / 2;
    if (ui_text_has_sel(t)) {
        i32 x0 = field_prefix_width(&mx, t, ui_text_sel_lo(t)) - t->scroll;
        i32 x1 = field_prefix_width(&mx, t, ui_text_sel_hi(t)) - t->scroll;
        fill_rect(dc, rect(inner.x + x0, cy, VS_MAX(1, x1 - x0), ch), focused ? UI_ACCENT_DIM : UI_LINE);
    }
    Rect tr = rect(inner.x - t->scroll, inner.y, inner.w + t->scroll + S(a, 400), inner.h);
    draw_text_rect(a, dc, tr, shown, UI_TEXT, a->f_body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    // Caret : masqué tant qu'une sélection est en cours (le bloc parle déjà).
    if (focused && !ui_text_has_sel(t) && ((now_ms - a->caret_blink_ms) / 500) % 2 == 0) {
        fill_rect(dc, rect(inner.x + caret_x - t->scroll, cy, VS_MAX(1, S(a, 1)), ch), UI_ACCENT_HI);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
    return submitted;
}

// checkbox : case à cocher dessinée à la main (coche en deux traits). Renvoie 1
// si l'état vient de changer.
static b32 checkbox(UiApp *a, HDC dc, Rect r, const char *text, b32 *value, u64 id) {
    b32 hover = !a->input_locked && rect_hit(r, a->mouse_x, a->mouse_y);
    if (hover) a->hot = id;
    b32 changed = 0;
    if (hover && a->mouse_pressed) {
        a->active = id;
        a->focus = id;
    }
    if (a->active == id && a->mouse_released) {
        a->active = 0;
        if (hover) {
            *value = !*value;
            changed = 1;
        }
    }
    // Espace ou Entrée quand la case a le focus : accessible au clavier.
    if (a->focus == id) {
        for (isize i = 0; i < a->key_count; i++) {
            if (a->keys[i].vk == VK_SPACE || a->keys[i].vk == VK_RETURN) {
                *value = !*value;
                changed = 1;
            }
        }
        if (changed) a->key_count = 0;
    }

    i32 box = S(a, 16);
    Rect b = rect(r.x, r.y + (r.h - box) / 2, box, box);
    u32 bg = *value ? UI_ACCENT : (hover ? UI_PANEL_HI : mix(UI_PANEL, UI_BG, 90));
    fill_round(dc, b, S(a, 4), bg);
    stroke_round(dc, b, S(a, 4), *value ? UI_ACCENT_HI : (hover ? UI_MUTED : UI_LINE), 1);
    if (a->focus == id) stroke_round(dc, rect_inset(b, -2), S(a, 6), UI_ACCENT_HI, 1);
    if (*value) {
        // Coche : deux segments obliques, donc anticrénelés — au trait GDI nu,
        // la branche montante partait en escalier d'un pixel sur deux.
        AaMask m;
        if (aa_begin(&m, dc, b)) {
            POINT pts[3] = {
                {m.w * 25 / 100, m.h * 51 / 100},
                {m.w * 43 / 100, m.h * 70 / 100},
                {m.w * 77 / 100, m.h * 31 / 100},
            };
            aa_stroke(&m, pts, 3, VS_MAX(2, S(a, 2)) * UI_AA_SS);
            aa_end(&m, dc, 0xffffffu);
        }
    }
    draw_text(a, dc, rect(b.x + box + S(a, 9), r.y, r.w - box - S(a, 9), r.h), text,
              hover ? UI_TEXT : UI_MUTED, a->f_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return changed;
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
    b32 hover = !a->input_locked && rect_hit(rect(r.x, r.y, r.w, r.h), a->mouse_x, a->mouse_y);
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
    snprintf(app->version_client, sizeof(app->version_client), "%s", VS_VERSION);
    app->remember_password = 1;  // cochée par défaut ; l'ini peut la décocher
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
    if (app->logo_bmp) {
        DeleteObject(app->logo_bmp);
        app->logo_bmp = NULL;
    }
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
    ID_GEAR,
    ID_SET_SERVER,
    ID_SET_NAME,
    ID_SET_ROOM,
    ID_SET_VLC,
    ID_SET_BROWSE,
    ID_SET_DETECT,
    ID_SET_SAVE,
    ID_SET_CANCEL,
    ID_TEST,
    ID_CANCEL,
    ID_USE_WSS,
    // Les bandeaux consomment deux identifiants chacun (corps + fermeture) :
    // notice_bar utilise id et id+1.
    ID_UPDATE,
    ID_UPDATE_CLOSE,
    ID_WATCH,
    ID_WATCH_CLOSE,
    ID_NOTICE,
    ID_NOTICE_CLOSE,
    ID_REMEMBER,
    ID_MEDIA_ADD,
    ID_MEDIA_ROW,  // + index de la ligne
};

// gear_button dessine le bouton engrenage (discret, même rendu partout).
static b32 gear_button(UiApp *a, HDC dc, Rect r) {
    b32 hover = !a->input_locked && rect_hit(r, a->mouse_x, a->mouse_y);
    b32 clicked = button(a, dc, r, "", ID_GEAR, BTN_GHOST, 1);
    i32 g = VS_MIN(r.w, r.h) - S(a, 12);
    glyph_gear(dc, rect(r.x + (r.w - g) / 2, r.y + (r.h - g) / 2, g, g), hover ? UI_TEXT : UI_MUTED);
    return clicked;
}

// health_dot dessine la pastille de joignabilité et son libellé.
static void health_line(UiApp *a, HDC dc, Rect r) {
    u32 col = UI_FAINT;
    const char *label = "Joignabilité non testée";
    char buf[224];
    switch (a->health) {
        case UI_HEALTH_TESTING:
            col = UI_WARN;
            label = "Test du serveur…";
            break;
        case UI_HEALTH_OK:
            col = UI_OK;
            snprintf(buf, sizeof(buf), "Serveur en ligne (%lld ms)", (long long)a->health_latency_ms);
            label = buf;
            break;
        case UI_HEALTH_FAIL:
            col = UI_DANGER;
            snprintf(buf, sizeof(buf), "Injoignable — %s", a->health_msg);
            label = buf;
            break;
        case UI_HEALTH_UNKNOWN:
        default: break;
    }
    i32 d = S(a, 8);
    fill_circle(dc, rect(r.x, r.y + (r.h - d) / 2, d, d), col);
    draw_text(a, dc, rect(r.x + d + S(a, 7), r.y, r.w - d - S(a, 7), r.h), label, col == UI_FAINT ? UI_FAINT : UI_MUTED,
              a->f_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---- logo de l'application ----
//
// Le logo de l'en-tête EST l'icône de l'exe (ressource 1, le vibesync.ico que
// montrent l'explorateur et la barre des tâches) : fini la pastille dessinée à
// la main qui ne ressemblait à rien de la marque. Le .ico embarque un calque
// 256 × 256 ; on le tire à cette définition, on le compose sur la couleur du
// panneau, puis on le réduit nous-mêmes par moyenne de blocs. C'est ce dernier
// point qui compte : LoadImage et DrawIconEx ne savent redimensionner qu'au plus
// proche voisin, ce qui donne une bouillie d'escaliers à 32 px. La moyenne, elle,
// donne un logo net à 100 % comme à 150 %, et toujours identique à l'icône.
//
// Le résultat est mis en cache pour la taille courante : le coût (un DrawIconEx
// 256² et 65 536 pixels moyennés) n'est payé qu'une fois par DPI.
#define UI_LOGO_SRC 256

static void logo_build(UiApp *a, HDC dc, i32 px) {
    // logo_px mémorise la taille *tentée*, pas seulement celle qui a réussi :
    // sans ressource icône (binaire de test), on ne rappelle pas LoadImage à
    // chaque frame pour se voir répondre NULL.
    if (a->logo_px == px) return;
    if (a->logo_bmp) {
        DeleteObject(a->logo_bmp);
        a->logo_bmp = NULL;
    }
    a->logo_px = px;
    if (px <= 0 || px > UI_LOGO_SRC) return;
    HICON ico = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, UI_LOGO_SRC,
                                  UI_LOGO_SRC, LR_DEFAULTCOLOR);
    if (!ico) return;  // pas de ressource (binaire de test) : l'appelant se rabat
    void *sbits = NULL;
    HBITMAP src = dib_create(dc, UI_LOGO_SRC, UI_LOGO_SRC, &sbits);
    HDC sdc = src ? CreateCompatibleDC(dc) : NULL;
    if (sdc) {
        HGDIOBJ so = SelectObject(sdc, src);
        fill_rect(sdc, rect(0, 0, UI_LOGO_SRC, UI_LOGO_SRC), UI_PANEL);
        DrawIconEx(sdc, 0, 0, ico, UI_LOGO_SRC, UI_LOGO_SRC, 0, NULL, DI_NORMAL);
        GdiFlush();
        void *dbits = NULL;
        HBITMAP dst = dib_create(dc, px, px, &dbits);
        if (dst) {
            const u8 *s = (const u8 *)sbits;
            u8 *d = (u8 *)dbits;
            for (i32 y = 0; y < px; y++) {
                i32 y0 = y * UI_LOGO_SRC / px, y1 = (y + 1) * UI_LOGO_SRC / px;
                for (i32 x = 0; x < px; x++) {
                    i32 x0 = x * UI_LOGO_SRC / px, x1 = (x + 1) * UI_LOGO_SRC / px;
                    i32 sb = 0, sg = 0, sr = 0, n = (y1 - y0) * (x1 - x0);
                    for (i32 j = y0; j < y1; j++) {
                        const u8 *row = s + ((isize)j * UI_LOGO_SRC + x0) * 4;
                        for (i32 i = x0; i < x1; i++, row += 4) {
                            sb += row[0];
                            sg += row[1];
                            sr += row[2];
                        }
                    }
                    u8 *o = d + ((isize)y * px + x) * 4;
                    o[0] = (u8)(sb / n);
                    o[1] = (u8)(sg / n);
                    o[2] = (u8)(sr / n);
                    o[3] = 255;
                }
            }
            a->logo_bmp = dst;
        }
        SelectObject(sdc, so);
        DeleteDC(sdc);
    }
    if (src) DeleteObject(src);
    DestroyIcon(ico);
}

// logo_draw pose le logo dans un carré. Les pixels sont déjà composés sur
// UI_PANEL : le logo ne va que sur la carte de connexion, dont c'est le fond.
static void logo_draw(UiApp *a, HDC dc, Rect r) {
    logo_build(a, dc, r.w);
    if (!a->logo_bmp) {
        i32 d = r.w / 2;
        fill_circle(dc, rect(r.x + (r.w - d) / 2, r.y + (r.h - d) / 2, d, d), UI_ACCENT);
        return;
    }
    HDC mdc = CreateCompatibleDC(dc);
    if (!mdc) return;
    HGDIOBJ ob = SelectObject(mdc, a->logo_bmp);
    BitBlt(dc, r.x, r.y, r.w, r.h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, ob);
    DeleteDC(mdc);
}

static void screen_connect(UiApp *a, HDC dc, i64 now_ms) {
    i32 card_w = VS_MIN(S(a, 440), a->width - S(a, 48));
    i32 pad = S(a, 24);
    i32 fh = S(a, 38);   // hauteur d'un champ
    i32 gap = S(a, 14);
    i32 hint_h = S(a, 20);  // ligne de joignabilité / d'aide sous le serveur
    // Marges + en-tête, 4 champs, la ligne de joignabilité, la case à cocher,
    // le bouton et le message d'état. La ligne d'aide n'existe qu'au besoin.
    i32 card_h = S(a, 118) + 4 * (S(a, 18) + fh + gap) + hint_h + S(a, 28) + S(a, 46) + S(a, 46);
    if (a->server_hint[0]) card_h += hint_h;
    Rect card = rect((a->width - card_w) / 2, VS_MAX(S(a, 16), (a->height - card_h) / 2), card_w, card_h);
    panel(a, dc, card);

    i32 x = card.x + pad, w = card.w - 2 * pad;
    i32 y = card.y + pad;

    // Marque : l'icône de l'application, puis le nom.
    i32 logo = S(a, 32);
    logo_draw(a, dc, rect(x, y + S(a, 6), logo, logo));
    draw_text(a, dc, rect(x + logo + S(a, 10), y, w, S(a, 40)), "vibesync", UI_TEXT, a->f_huge,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    i32 gs = S(a, 34);
    if (gear_button(a, dc, rect(x + w - gs, y + S(a, 4), gs, gs))) a->act_settings_open = 1;
    y += S(a, 46);
    draw_text(a, dc, rect(x, y, w, S(a, 20)), "Regarder ensemble, chacun chez soi.", UI_MUTED, a->f_small,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += S(a, 34);

    b32 submit = 0;

    // --- Serveur : champ + bouton Tester, puis l'état de joignabilité ---
    label(a, dc, rect(x, y, w, S(a, 18)), "Serveur");
    y += S(a, 18);
    i32 test_w = S(a, 74);
    if (field(a, dc, rect(x, y, w - test_w - S(a, 8), fh), &a->f_server, "vibesync.exemple.fr", ID_SERVER, 0,
              now_ms)) {
        submit = 1;
    }
    if (button(a, dc, rect(x + w - test_w, y, test_w, fh), "Tester", ID_TEST, BTN_GHOST,
               a->health != UI_HEALTH_TESTING)) {
        a->act_test_server = 1;
    }
    y += fh + S(a, 4);
    health_line(a, dc, rect(x, y, w, hint_h));
    y += hint_h;
    if (a->server_hint[0]) {
        // Adresse normalisée ou bascule TLS proposée : un clic pour l'adopter.
        u32 col = a->health_tls_hint ? UI_WARN : UI_FAINT;
        i32 use_w = S(a, 78);
        draw_text(a, dc, rect(x, y, w - use_w - S(a, 6), hint_h), a->server_hint, col, a->f_small,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (button(a, dc, rect(x + w - use_w, y - S(a, 3), use_w, hint_h + S(a, 6)), "Utiliser", ID_USE_WSS,
                   BTN_SUBTLE, 1)) {
            a->act_use_wss = 1;
        }
        y += hint_h;
    }
    y += gap - S(a, 4);

    struct {
        const char *lab;
        UiText *t;
        u64 id;
        const char *ph;
        b32 pwd;
    } fields[] = {
        {"Pseudo", &a->f_name, ID_NAME, "votre pseudo", 0},
        {"Salle", &a->f_room, ID_ROOM, "salon", 0},
        {"Mot de passe (optionnel)", &a->f_password, ID_PASSWORD, "aucun", 1},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(fields); i++) {
        label(a, dc, rect(x, y, w, S(a, 18)), fields[i].lab);
        y += S(a, 18);
        if (field(a, dc, rect(x, y, w, fh), fields[i].t, fields[i].ph, fields[i].id, fields[i].pwd, now_ms)) {
            submit = 1;
        }
        y += fh + (fields[i].pwd ? S(a, 6) : gap);
    }

    if (checkbox(a, dc, rect(x, y, w, S(a, 22)), "Se souvenir du mot de passe (chiffré par Windows)",
                 &a->remember_password, ID_REMEMBER)) {
        a->act_remember_changed = 1;
    }
    y += S(a, 22) + gap;

    // --- Connexion : pendant une tentative, Annuler rend la main ---
    b32 busy = a->connecting || a->retrying_wait;
    i32 bh = S(a, 44);
    if (busy) {
        i32 cancel_w = S(a, 108);
        char lab[64];
        if (a->retrying_wait && a->retry_seconds > 0) {
            snprintf(lab, sizeof(lab), "Nouvel essai dans %lld s…", (long long)a->retry_seconds);
        } else {
            snprintf(lab, sizeof(lab), "Connexion…");
        }
        button(a, dc, rect(x, y, w - cancel_w - S(a, 8), bh), lab, ID_CONNECT, BTN_PRIMARY, 0);
        if (button(a, dc, rect(x + w - cancel_w, y, cancel_w, bh), "Annuler", ID_CANCEL, BTN_GHOST, 1)) {
            a->act_cancel_connect = 1;
        }
    } else if (button(a, dc, rect(x, y, w, bh), "Se connecter", ID_CONNECT, BTN_PRIMARY, 1)) {
        submit = 1;
    }
    y += bh + S(a, 12);

    if (a->status[0]) {
        draw_text(a, dc, rect(x, y, w, S(a, 40)), a->status, a->status_error ? UI_DANGER : UI_MUTED, a->f_small,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    // Entrée valide le formulaire depuis n'importe quel champ.
    if (submit && !busy) a->act_connect = 1;

    // Demande de focus venue de main.c : le champ fautif est prêt à corriger.
    if (a->focus_request != UI_FIELD_NONE) {
        UiText *t = NULL;
        switch (a->focus_request) {
            case UI_FIELD_SERVER: a->focus = ID_SERVER; t = &a->f_server; break;
            case UI_FIELD_NAME: a->focus = ID_NAME; t = &a->f_name; break;
            case UI_FIELD_ROOM: a->focus = ID_ROOM; t = &a->f_room; break;
            case UI_FIELD_PASSWORD: a->focus = ID_PASSWORD; t = &a->f_password; break;
            case UI_FIELD_NONE: break;
        }
        if (t) ui_text_select_all(t);  // prêt à être remplacé d'une frappe
        a->focus_request = UI_FIELD_NONE;
        a->caret_blink_ms = now_ms;
    }

    Rect foot = rect(card.x, card.y + card.h + S(a, 10), card.w, S(a, 18));
    char version[64];
    snprintf(version, sizeof(version), "client v%s · protocole v%s", a->version_client,
             VS_PROTOCOL_VERSION_TEXT);
    draw_text(a, dc, foot, version, UI_FAINT, a->f_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
    draw_text(a, dc, rect(foot.x, foot.y + S(a, 16), foot.w, S(a, 18)),
              "Réglages mémorisés dans %APPDATA%\\vibesync.ini", UI_FAINT, a->f_small,
              DT_CENTER | DT_TOP | DT_SINGLELINE);
}

// ---------------------------------------------------------------- écran salle ---

b32 ui_user_openable(const UiUser *u) {
    return !u->is_self && u->has_file && u->file[0] != 0 && !u->same_file;
}

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
        // Double-clic sur la ligne d'un participant qui a déclaré un fichier
        // AUTRE que le nôtre : on va le chercher dans les dossiers médias et on
        // l'ouvre. Le survol ne s'allume que là où le double-clic mène quelque
        // part — une surbrillance sur une ligne inerte serait un mensonge.
        b32 row_hover = !a->input_locked && ui_user_openable(u) && rect_hit(row, a->mouse_x, a->mouse_y);
        if (row_hover) {
            a->hot = 5000 + (u64)i;
            if (a->mouse_pressed && a->mouse_double) {
                a->act_open_user_file = 1;
                a->act_open_user_index = i;
            }
        }
        if (u->is_self) fill_round(dc, row, S(a, 8), mix(UI_ACCENT, UI_PANEL, 210));
        else if (row_hover) fill_round(dc, row, S(a, 8), UI_PANEL_HI);
        i32 tx = row.x + S(a, 8);
        draw_text(a, dc, rect(tx, row.y + S(a, 4), w - S(a, 90), S(a, 18)), u->name, UI_TEXT,
                  u->is_self ? a->f_bold : a->f_body, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        const char *sub = u->has_file ? u->file : "aucun fichier";
        draw_text(a, dc, rect(tx, row.y + S(a, 21), w - S(a, 90), S(a, 16)), sub,
                  row_hover ? UI_ACCENT_HI : UI_FAINT, a->f_small,
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

    // Versions en pied de colonne : discret, mais toujours sous les yeux quand
    // il faut diagnostiquer un décalage client/serveur.
    char ver[96];
    if (a->version_server[0]) {
        snprintf(ver, sizeof(ver), "client v%s · serveur v%s · protocole v%s", a->version_client,
                 a->version_server, VS_PROTOCOL_VERSION_TEXT);
    } else {
        snprintf(ver, sizeof(ver), "client v%s · protocole v%s", a->version_client, VS_PROTOCOL_VERSION_TEXT);
    }
    draw_text(a, dc, rect(x, r.y + r.h - pad - S(a, 14), w, S(a, 16)), ver, UI_FAINT, a->f_small,
              DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// notice_bar : bandeau cliquable et fermable, non bloquant. Sert aux trois
// invitations de la salle (mise à jour, « X regarde… », fichier introuvable).
static void notice_bar(UiApp *a, HDC dc, Rect r, const char *text, u32 tint, u64 id, b32 *clicked,
                       b32 *closed) {
    fill_round(dc, r, S(a, 8), mix(tint, UI_PANEL, 190));
    stroke_round(dc, r, S(a, 8), mix(tint, UI_PANEL, 120), 1);
    i32 pad = S(a, 12);
    i32 close_w = S(a, 30);
    Rect click = rect(r.x, r.y, r.w - close_w, r.h);
    b32 hover = !a->input_locked && rect_hit(click, a->mouse_x, a->mouse_y);
    if (hover) a->hot = id;
    if (hover && a->mouse_pressed) a->active = id;
    if (a->active == id && a->mouse_released) {
        a->active = 0;
        if (hover) *clicked = 1;
    }
    draw_text(a, dc, rect(r.x + pad, r.y, r.w - pad - close_w, r.h), text, hover ? UI_TEXT : mix(tint, UI_TEXT, 90),
              a->f_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (button(a, dc, rect(r.x + r.w - close_w - S(a, 4), r.y + (r.h - close_w) / 2, close_w, close_w), "✕",
               id + 1, BTN_SUBTLE, 1)) {
        *closed = 1;
    }
}

// room_notices empile les bandeaux en haut du contenu et renvoie la hauteur
// consommée. Aucun n'est modal : la salle reste utilisable.
static i32 room_notices(UiApp *a, HDC dc, i32 x, i32 top, i32 w, i32 gap) {
    i32 bh = S(a, 34);
    i32 used = 0;
    char msg[280];
    b32 clicked, closed;

    if (a->update_available && !a->update_dismissed) {
        snprintf(msg, sizeof(msg), "Nouvelle version disponible (v%s) — cliquer pour télécharger",
                 a->update_version);
        clicked = closed = 0;
        notice_bar(a, dc, rect(x, top + used, w, bh), msg, UI_ACCENT, ID_UPDATE, &clicked, &closed);
        if (clicked) a->act_update_download = 1;
        if (closed) a->act_update_dismiss = 1;
        used += bh + gap;
    }
    if (a->watch_show) {
        snprintf(msg, sizeof(msg), "%s regarde %s — cliquer pour l'ouvrir chez vous", a->watch_who,
                 a->watch_file);
        clicked = closed = 0;
        notice_bar(a, dc, rect(x, top + used, w, bh), msg, UI_CYAN, ID_WATCH, &clicked, &closed);
        if (clicked) a->act_open_watch_file = 1;
        if (closed) a->act_dismiss_watch = 1;
        used += bh + gap;
    }
    if (a->media_notice_show) {
        clicked = closed = 0;
        notice_bar(a, dc, rect(x, top + used, w, bh), a->media_notice, UI_WARN, ID_NOTICE, &clicked, &closed);
        if (clicked) a->act_notice_settings = 1;
        if (closed) a->act_dismiss_notice = 1;
        used += bh + gap;
    }
    return used;
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
    if (!a->input_locked && rect_hit(hist, a->mouse_x, a->mouse_y) && a->wheel != 0) {
        a->chat_scroll += a->wheel / 120;
        if (a->chat_scroll < 0) a->chat_scroll = 0;
        if (a->chat_scroll > a->chat_count) a->chat_scroll = a->chat_count;
    }

    // Les lignes sont mesurées puis empilées du bas vers le haut.
    HGDIOBJ of = SelectObject(dc, a->f_body);
    i32 y = hist.y + hist.h;

    // Messages composés hors ligne : les plus récents, donc tout en bas, en
    // gris et marqués « en attente ». Ils redeviendront des lignes normales
    // quand le serveur les rediffusera après la reconnexion.
    for (isize i = a->pending_count - 1; i >= 0 && y > hist.y; i--) {
        Builder b;
        builder_init(&b, a->scratch, 256);
        builder_cstr(&b, a->pending[i]);
        builder_cstr(&b, "   · en attente");
        Str8 line = builder_result(&b);
        isize n = 0;
        const wchar_t *wtext = wide(a, line, &n);
        RECT calc = {0, 0, hist.w, 0};
        DrawTextW(dc, wtext, (int)n, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        i32 lh = calc.bottom - calc.top + S(a, 6);
        y -= lh;
        if (y + lh < hist.y) break;
        draw_text_rect(a, dc, rect(hist.x, y, hist.w, lh), line, UI_FAINT, a->f_body,
                       DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }

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
    if (a->chat_count == 0 && a->pending_count == 0) {
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

    Rect gear = rect(leave.x - S(a, 8) - bh, y + (r.h - bh) / 2, bh, bh);
    if (gear_button(a, dc, gear)) a->act_settings_open = 1;

    i32 rx = gear.x - S(a, 12);
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

    top += room_notices(a, dc, pad, top, a->width - 2 * pad, pad);

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

// -------------------------------------------------------------- réglages ---
//
// Panneau modal superposé à l'écran courant : l'écran du dessous est dessiné
// normalement mais son entrée est verrouillée (a->input_locked), ce qui évite
// de dupliquer une machine à états d'écran juste pour des réglages.

// dim_screen assombrit tout l'écran sous le panneau modal.
//
// AlphaBlend vit dans msimg32.dll : on la charge à la demande plutôt que de
// l'ajouter à l'édition de liens (même geste que DwmSetWindowAttribute). La
// source est un unique pixel noir prémultiplié, étiré sur toute la fenêtre.
// À défaut (DLL absente), repli sur un damier : le voile reste visible.
typedef BOOL(WINAPI *AlphaBlendFn)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);

static void dim_checker(HDC dc, Rect r) {
    static const u16 bits[8] = {0x5555, 0xaaaa, 0x5555, 0xaaaa, 0x5555, 0xaaaa, 0x5555, 0xaaaa};
    HBITMAP bm = CreateBitmap(8, 8, 1, 1, bits);
    if (!bm) return;
    HBRUSH br = CreatePatternBrush(bm);
    HGDIOBJ ob = SelectObject(dc, br);
    COLORREF otc = SetTextColor(dc, RGB(0, 0, 0));
    COLORREF obc = SetBkColor(dc, RGB(255, 255, 255));
    PatBlt(dc, r.x, r.y, r.w, r.h, 0x00A000C9);  // DPa : dest ET trame
    SetTextColor(dc, otc);
    SetBkColor(dc, obc);
    SelectObject(dc, ob);
    DeleteObject(br);
    DeleteObject(bm);
}

static void dim_screen(HDC dc, Rect r) {
    static AlphaBlendFn alpha_blend = NULL;
    static b32 tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryW(L"msimg32.dll");
        if (m) alpha_blend = (AlphaBlendFn)(void *)GetProcAddress(m, "AlphaBlend");
    }
    if (!alpha_blend) {
        dim_checker(dc, r);
        return;
    }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC src = CreateCompatibleDC(dc);
    if (!src) return;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bm && bits) {
        ((u32 *)bits)[0] = 0xa8000000u;  // noir prémultiplié, alpha 168/255
        HGDIOBJ ob = SelectObject(src, bm);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        alpha_blend(dc, r.x, r.y, r.w, r.h, src, 0, 0, 1, 1, bf);
        SelectObject(src, ob);
    }
    if (bm) DeleteObject(bm);
    DeleteDC(src);
}

static void screen_settings(UiApp *a, HDC dc, i64 now_ms) {
    dim_screen(dc, rect(0, 0, a->width, a->height));

    i32 card_w = VS_MIN(S(a, 560), a->width - S(a, 48));
    i32 pad = S(a, 24);
    i32 fh = S(a, 36);
    i32 gap = S(a, 12);
    // Hauteur exacte : marges + en-tête + 3 champs + le bloc VLC + le message
    // de validation + les dossiers médias + la rangée de boutons.
    i32 head_h = S(a, 62), msg_h = S(a, 40), btn_h = S(a, 40);
    i32 row_h = S(a, 26);
    i32 media_h = S(a, 18) + (a->media_dir_count > 0 ? (i32)a->media_dir_count * row_h : row_h) + fh + S(a, 8);
    i32 card_h = 2 * pad + head_h + 3 * (S(a, 18) + fh + gap) + (S(a, 18) + fh + S(a, 6)) + msg_h + media_h +
                 S(a, 8) + btn_h;
    Rect card = rect((a->width - card_w) / 2, VS_MAX(S(a, 16), (a->height - card_h) / 2), card_w, card_h);
    // Ombre portée : le panneau flotte franchement au-dessus de l'écran.
    fill_round(dc, rect(card.x + S(a, 3), card.y + S(a, 5), card.w, card.h), S(a, 14),
               mix(UI_BG, 0x000000u, 140));
    fill_round(dc, card, S(a, 14), UI_PANEL);
    stroke_round(dc, card, S(a, 14), UI_ACCENT_DIM, 1);

    i32 x = card.x + pad, w = card.w - 2 * pad;
    i32 y = card.y + pad;
    draw_text(a, dc, rect(x, y, w, S(a, 28)), "Réglages", UI_TEXT, a->f_title,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    draw_text(a, dc, rect(x, y + S(a, 26), w, S(a, 18)), "Valeurs par défaut, mémorisées dans %APPDATA%\\vibesync.ini",
              UI_FAINT, a->f_small, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += head_h;

    b32 submit = 0;
    struct {
        const char *lab;
        UiText *t;
        u64 id;
        const char *ph;
    } fields[] = {
        {"Serveur par défaut", &a->f_set_server, ID_SET_SERVER, "wss://vibesync.exemple.fr/ws"},
        {"Pseudo par défaut", &a->f_set_name, ID_SET_NAME, "votre pseudo"},
        {"Salle par défaut", &a->f_set_room, ID_SET_ROOM, "salon"},
    };
    for (isize i = 0; i < VS_ARRAY_COUNT(fields); i++) {
        label(a, dc, rect(x, y, w, S(a, 18)), fields[i].lab);
        y += S(a, 18);
        if (field(a, dc, rect(x, y, w, fh), fields[i].t, fields[i].ph, fields[i].id, 0, now_ms)) submit = 1;
        y += fh + gap;
    }

    // Chemin de VLC : champ + « Parcourir… » + « Détecter » + état.
    //
    // « Détecter » se grise quand la détection automatique n'a rien trouvé —
    // c'est-à-dire exactement le cas où l'utilisateur a besoin d'aide. D'où
    // « Parcourir… », toujours actif, qui ouvre le sélecteur de Windows.
    label(a, dc, rect(x, y, w, S(a, 18)), "Chemin de VLC (vide = détection automatique)");
    y += S(a, 18);
    i32 det_w = S(a, 92), brw_w = S(a, 100), bgap = S(a, 8);
    if (field(a, dc, rect(x, y, w - det_w - brw_w - 2 * bgap, fh), &a->f_set_vlc, a->settings_auto_vlc[0] ? a->settings_auto_vlc : "C:\\Program Files\\VideoLAN\\VLC\\vlc.exe",
              ID_SET_VLC, 0, now_ms)) {
        submit = 1;
    }
    if (button(a, dc, rect(x + w - det_w - brw_w - bgap, y, brw_w, fh), "Parcourir…", ID_SET_BROWSE,
               BTN_GHOST, 1)) {
        a->act_settings_browse = 1;
    }
    if (button(a, dc, rect(x + w - det_w, y, det_w, fh), "Détecter", ID_SET_DETECT, BTN_GHOST,
               a->settings_auto_vlc[0] != 0)) {
        a->act_settings_detect = 1;
    }
    y += fh + S(a, 6);

    {
        const char *msg;
        u32 col;
        if (a->settings_vlc_state == 2) {
            msg = "Ce fichier n'existe pas : corrigez le chemin ou videz le champ.";
            col = UI_DANGER;
        } else if (a->settings_vlc_state == 1) {
            msg = "vlc.exe trouvé à ce chemin.";
            col = UI_OK;
        } else if (a->settings_auto_vlc[0]) {
            msg = a->settings_auto_vlc;
            col = UI_MUTED;
        } else {
            msg = "Aucun VLC détecté sur cette machine : indiquez le chemin de vlc.exe.";
            col = UI_WARN;
        }
        Builder b;
        builder_init(&b, a->scratch, 320);
        if (a->settings_vlc_state == 0 && a->settings_auto_vlc[0]) builder_cstr(&b, "Détection automatique : ");
        builder_cstr(&b, msg);
        draw_text_rect(a, dc, rect(x, y, w, S(a, 34)), builder_result(&b), col, a->f_small,
                       DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    y += msg_h;

    // --- Dossiers médias : c'est là qu'on ira chercher le fichier d'un ami ---
    label(a, dc, rect(x, y, w, S(a, 18)), "Dossiers médias (recherche du fichier d'un participant)");
    y += S(a, 18);
    if (a->media_dir_count == 0) {
        draw_text(a, dc, rect(x, y, w, row_h), "Aucun dossier : ajoutez celui de vos films.", UI_WARN,
                  a->f_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += row_h;
    }
    for (isize i = 0; i < a->media_dir_count; i++) {
        i32 del_w = S(a, 78);
        draw_text(a, dc, rect(x, y, w - del_w - S(a, 8), row_h), a->media_dirs[i], UI_MUTED, a->f_small,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        if (button(a, dc, rect(x + w - del_w, y + S(a, 2), del_w, row_h - S(a, 4)), "Retirer",
                   ID_MEDIA_ROW + (u64)i, BTN_GHOST, 1)) {
            a->act_media_remove = 1;
            a->act_media_remove_index = i;
        }
        y += row_h;
    }
    if (button(a, dc, rect(x, y + S(a, 4), S(a, 190), fh - S(a, 4)), "Ajouter un dossier…", ID_MEDIA_ADD,
               BTN_SUBTLE, a->media_dir_count < UI_MAX_MEDIA_DIRS)) {
        a->act_media_add = 1;
    }
    y += fh + S(a, 8);

    i32 bw = S(a, 130);
    if (a->settings_msg[0]) {
        // Aligné sur la rangée de boutons, borné à la place qui reste à gauche.
        draw_text(a, dc, rect(x, y + S(a, 8), w - 2 * bw - S(a, 20), btn_h), a->settings_msg,
                  a->settings_msg_error ? UI_DANGER : UI_OK, a->f_small,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (button(a, dc, rect(x + w - bw, y + S(a, 8), bw, btn_h), "Enregistrer", ID_SET_SAVE, BTN_PRIMARY, 1)) {
        submit = 1;
    }
    if (button(a, dc, rect(x + w - 2 * bw - S(a, 10), y + S(a, 8), bw, btn_h), "Annuler", ID_SET_CANCEL,
               BTN_GHOST, 1)) {
        a->act_settings_cancel = 1;
    }
    if (submit) a->act_settings_save = 1;
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
    app->hot_is_text = 0;
    app->field_seq = 0;
    if (!app->f_body) ui_set_dpi(app, app->dpi);

    fill_rect(dc, rect(0, 0, w, h), UI_BG);

    // Échap : ferme les réglages, sinon lâche le champ courant et le bandeau.
    if (key_pressed(app, VK_ESCAPE)) {
        if (app->settings_open) app->act_settings_cancel = 1;
        else {
            app->focus = 0;
            app->toast_until_ms = 0;
        }
        app->key_count = 0;
    }

    // L'écran du dessous est dessiné mais sourd tant que les réglages sont
    // ouverts : un seul chemin de rendu, aucune entrée qui fuit sous le modal.
    app->input_locked = app->settings_open;
    if (app->screen == UI_SCREEN_CONNECT) screen_connect(app, dc, now_ms);
    else screen_room(app, dc, now_ms);
    app->input_locked = 0;
    if (app->settings_open) screen_settings(app, dc, now_ms);

    // Sortie du champ Serveur : on teste la joignabilité sans rien demander à
    // l'utilisateur. C'est le moment où il vient de finir de taper l'adresse.
    if (app->focus != app->focus_prev) {
        if (app->focus_prev == ID_SERVER && app->screen == UI_SCREEN_CONNECT) app->act_test_server = 1;
        app->focus_prev = app->focus;
    }

    draw_toast(app, dc, now_ms);

    // Curseur : barre en I sur les champs, main sur le reste des zones
    // cliquables. Petit détail, grand effet — et un vrai repère d'édition.
    LPCWSTR cursor = (LPCWSTR)IDC_ARROW;
    if (app->hot_is_text || app->dragging_text) cursor = (LPCWSTR)IDC_IBEAM;
    else if (app->hot) cursor = (LPCWSTR)IDC_HAND;
    SetCursor(LoadCursorW(NULL, cursor));

    // Rafraîchissement périodique seulement si quelque chose bouge.
    app->need_timer = (app->screen == UI_SCREEN_ROOM && !app->paused && app->vlc_running) || app->focus != 0 ||
                      (app->toast[0] != 0 && now_ms < app->toast_until_ms) || app->connecting ||
                      app->retrying_wait || app->health == UI_HEALTH_TESTING;

    // Fin de frame : les entrées non consommées sont oubliées.
    app->mouse_pressed = 0;
    app->mouse_released = 0;
    app->mouse_double = 0;
    app->wheel = 0;
    app->char_count = 0;
    app->key_count = 0;
}
