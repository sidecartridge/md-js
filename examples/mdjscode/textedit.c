#include "textedit.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * VDI / AES bindings — minimal, private to this file
 * ---------------------------------------------------------------------- */

typedef struct
{
    int16_t *control;
    int16_t *intin;
    int16_t *ptsin;
    int16_t *intout;
    int16_t *ptsout;
} VDIPB;

typedef struct
{
    int16_t *control;
    int16_t *global;
    int16_t *int_in;
    int16_t *int_out;
    long    *addr_in;
    long    *addr_out;
} AESPB;

static int16_t vdi_control[15];
static int16_t vdi_intin[128];
static int16_t vdi_ptsin[128];
static int16_t vdi_intout[128];
static int16_t vdi_ptsout[128];

static VDIPB vdi_pb = { vdi_control, vdi_intin, vdi_ptsin, vdi_intout, vdi_ptsout };

static int16_t aes_control[5];
static int16_t aes_global[15];
static int16_t aes_int_in[16];
static int16_t aes_int_out[16];
static long    aes_addr_in[8];
static long    aes_addr_out[2];

static AESPB aes_pb = {
    aes_control, aes_global, aes_int_in, aes_int_out, aes_addr_in, aes_addr_out
};

static void vdi_trap(VDIPB *pb)
{
    __asm__ volatile(
        "move.w #115,%%d0\n\t"
        "move.l %0,%%d1\n\t"
        "trap #2"
        :
        : "g"(pb)
        : "d0", "d1", "d2", "a0", "a1", "memory", "cc");
}

static void aes_trap(AESPB *pb)
{
    __asm__ volatile(
        "move.w #200,%%d0\n\t"
        "move.l %0,%%d1\n\t"
        "trap #2"
        :
        : "g"(pb)
        : "d0", "d1", "d2", "a0", "a1", "memory", "cc");
}

static int16_t aes_call(int16_t opcode, int16_t nin, int16_t nout,
                        int16_t nain, int16_t naout)
{
    aes_control[0] = opcode;
    aes_control[1] = nin;
    aes_control[2] = nout;
    aes_control[3] = nain;
    aes_control[4] = naout;
    aes_trap(&aes_pb);
    return aes_int_out[0];
}

static void vdi_call(int16_t func, int16_t sub, int16_t n_intin,
                     int16_t n_ptsin, int16_t handle)
{
    vdi_control[0] = func;
    vdi_control[1] = n_ptsin;
    vdi_control[3] = n_intin;
    vdi_control[5] = sub;
    vdi_control[6] = handle;
    vdi_trap(&vdi_pb);
}

/* AES wrappers */

static int16_t graf_mouse(int16_t type)
{
    aes_int_in[0] = type;
    aes_addr_in[0] = 0L;
    return aes_call(78, 1, 1, 1, 0);
}

static int16_t wind_get(int16_t handle, int16_t field,
                        int16_t *o1, int16_t *o2, int16_t *o3, int16_t *o4)
{
    aes_int_in[0] = handle;
    aes_int_in[1] = field;
    aes_call(104, 2, 5, 0, 0);
    *o1 = aes_int_out[1];
    *o2 = aes_int_out[2];
    *o3 = aes_int_out[3];
    *o4 = aes_int_out[4];
    return aes_int_out[0];
}

static int16_t wind_set(int16_t handle, int16_t field,
                        int16_t p1, int16_t p2, int16_t p3, int16_t p4)
{
    aes_int_in[0] = handle;
    aes_int_in[1] = field;
    aes_int_in[2] = p1;
    aes_int_in[3] = p2;
    aes_int_in[4] = p3;
    aes_int_in[5] = p4;
    return aes_call(105, 6, 1, 0, 0);
}

static int16_t wind_update(int16_t mode)
{
    aes_int_in[0] = mode;
    return aes_call(107, 1, 1, 0, 0);
}

/* VDI wrappers */

static void vs_clip(int16_t h, int16_t flag,
                    int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
    vdi_intin[0] = flag;
    vdi_ptsin[0] = x1; vdi_ptsin[1] = y1;
    vdi_ptsin[2] = x2; vdi_ptsin[3] = y2;
    vdi_call(129, 0, 1, 2, h);
}

static void v_gtext(int16_t h, int16_t x, int16_t y, const char *str)
{
    int16_t len = 0;
    while (str[len] != '\0' && len < 120)
    {
        vdi_intin[len] = (uint8_t)str[len];
        len++;
    }
    vdi_ptsin[0] = x;
    vdi_ptsin[1] = y;
    vdi_call(8, 0, len, 1, h);
}

static void vst_alignment(int16_t h, int16_t hin, int16_t vin)
{
    int16_t dummy;
    vdi_intin[0] = hin;
    vdi_intin[1] = vin;
    vdi_call(39, 0, 2, 0, h);
    dummy = vdi_intout[0];
    (void)dummy;
}

static void vsf_color(int16_t h, int16_t color)
{
    vdi_intin[0] = color;
    vdi_call(25, 0, 1, 0, h);
}

static void vsf_interior(int16_t h, int16_t style)
{
    vdi_intin[0] = style;
    vdi_call(23, 0, 1, 0, h);
}

static void vr_recfl(int16_t h, int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
    vdi_ptsin[0] = x1; vdi_ptsin[1] = y1;
    vdi_ptsin[2] = x2; vdi_ptsin[3] = y2;
    vdi_call(114, 0, 0, 2, h);
}

static void vswr_mode(int16_t h, int16_t mode)
{
    vdi_intin[0] = mode;
    vdi_call(32, 0, 1, 0, h);
}

/* -------------------------------------------------------------------------
 * GEM window field / message constants
 * ---------------------------------------------------------------------- */

enum { WF_WORKXYWH = 4, WF_HSLIDE = 8,  WF_VSLIDE = 9,
       WF_FIRSTXYWH = 11, WF_NEXTXYWH = 12,
       WF_HSLSIZE = 15, WF_VSLSIZE = 16 };

enum { M_OFF = 256, M_ON = 257 };

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static void te_get_work(TexEdit *te,
                        int16_t *wx, int16_t *wy, int16_t *ww, int16_t *wh)
{
    wind_get(te->win_handle, WF_WORKXYWH, wx, wy, ww, wh);
}

static int16_t te_vis_cols(TexEdit *te, int16_t ww)
{
    return (te->cell_w > 0) ? ww / te->cell_w : 1;
}

static int16_t te_vis_rows(TexEdit *te, int16_t wh)
{
    return (te->cell_h > 0) ? wh / te->cell_h : 1;
}

static int16_t te_max_line_width(TexEdit *te)
{
    int16_t i, max = 0;
    for (i = 0; i < te->num_lines; i++)
        if (te->line_len[i] > max) max = te->line_len[i];
    return max;
}

static int16_t te_hscroll_max(TexEdit *te, int16_t ww)
{
    int16_t m = te_max_line_width(te) + 1 - te_vis_cols(te, ww);
    return (m < 0) ? 0 : m;
}

static int16_t te_vscroll_max(TexEdit *te, int16_t wh)
{
    int16_t m = te->num_lines - te_vis_rows(te, wh);
    return (m < 0) ? 0 : m;
}

static void te_clamp_scroll(TexEdit *te, int16_t ww, int16_t wh)
{
    int16_t hmax = te_hscroll_max(te, ww);
    int16_t vmax = te_vscroll_max(te, wh);

    if (te->hscroll > hmax) te->hscroll = hmax;
    if (te->hscroll < 0)    te->hscroll = 0;
    if (te->vscroll > vmax) te->vscroll = vmax;
    if (te->vscroll < 0)    te->vscroll = 0;
}

static int16_t te_rect_intersect(int16_t *r,
                                  int16_t cx, int16_t cy, int16_t cw, int16_t ch)
{
    int16_t x1 = r[0], y1 = r[1];
    int16_t x2 = r[0] + r[2] - 1, y2 = r[1] + r[3] - 1;
    int16_t cx2 = cx + cw - 1, cy2 = cy + ch - 1;

    if (x1 < cx)  x1 = cx;
    if (y1 < cy)  y1 = cy;
    if (x2 > cx2) x2 = cx2;
    if (y2 > cy2) y2 = cy2;

    if (x1 > x2 || y1 > y2) return 0;

    r[0] = x1; r[1] = y1;
    r[2] = x2 - x1 + 1; r[3] = y2 - y1 + 1;
    return 1;
}

static void te_clamp_cursor(TexEdit *te)
{
    if (te->cursor_row < 0)                        te->cursor_row = 0;
    if (te->cursor_row >= te->num_lines)            te->cursor_row = te->num_lines - 1;
    if (te->cursor_col < 0)                        te->cursor_col = 0;
    if (te->cursor_col > te->line_len[te->cursor_row])
        te->cursor_col = te->line_len[te->cursor_row];
}

static void te_ensure_cursor_visible(TexEdit *te)
{
    int16_t wx, wy, ww, wh;
    int16_t vc, vr;

    te_get_work(te, &wx, &wy, &ww, &wh);
    vc = te_vis_cols(te, ww);
    vr = te_vis_rows(te, wh);

    if (te->cursor_col < te->hscroll)
        te->hscroll = te->cursor_col;
    else if (te->cursor_col >= te->hscroll + vc)
        te->hscroll = te->cursor_col - vc + 1;

    if (te->cursor_row < te->vscroll)
        te->vscroll = te->cursor_row;
    else if (te->cursor_row >= te->vscroll + vr)
        te->vscroll = te->cursor_row - vr + 1;

    if (te->hscroll < 0) te->hscroll = 0;
    if (te->vscroll < 0) te->vscroll = 0;
}

/* -------------------------------------------------------------------------
 * Text mutation
 * ---------------------------------------------------------------------- */

static void te_insert_char(TexEdit *te, char c)
{
    int16_t i, len;

    if (te->cursor_row < 0 || te->cursor_row >= te->num_lines) return;
    len = te->line_len[te->cursor_row];
    if (len >= TE_MAX_LINE_LEN) return;

    for (i = len; i > te->cursor_col; i--)
        te->lines[te->cursor_row][i] = te->lines[te->cursor_row][i - 1];

    te->lines[te->cursor_row][te->cursor_col] = c;
    te->line_len[te->cursor_row] = len + 1;
    te->lines[te->cursor_row][len + 1] = '\0';
    te->cursor_col++;
}

static void te_delete_before(TexEdit *te)
{
    int16_t i, len, prev_len, copy_len;

    if (te->cursor_col > 0)
    {
        len = te->line_len[te->cursor_row];
        for (i = te->cursor_col - 1; i < len - 1; i++)
            te->lines[te->cursor_row][i] = te->lines[te->cursor_row][i + 1];
        te->line_len[te->cursor_row] = len - 1;
        te->lines[te->cursor_row][len - 1] = '\0';
        te->cursor_col--;
    }
    else if (te->cursor_row > 0)
    {
        prev_len = te->line_len[te->cursor_row - 1];
        copy_len = te->line_len[te->cursor_row];
        if (prev_len + copy_len > TE_MAX_LINE_LEN)
            copy_len = TE_MAX_LINE_LEN - prev_len;

        for (i = 0; i < copy_len; i++)
            te->lines[te->cursor_row - 1][prev_len + i] = te->lines[te->cursor_row][i];
        te->line_len[te->cursor_row - 1] = prev_len + copy_len;
        te->lines[te->cursor_row - 1][prev_len + copy_len] = '\0';

        for (i = te->cursor_row; i < te->num_lines - 1; i++)
        {
            memcpy(te->lines[i], te->lines[i + 1], TE_MAX_LINE_LEN + 1);
            te->line_len[i] = te->line_len[i + 1];
        }
        te->num_lines--;
        te->cursor_row--;
        te->cursor_col = prev_len;
    }
}

static void te_delete_at(TexEdit *te)
{
    int16_t i, len, next_len, copy_len;

    len = te->line_len[te->cursor_row];
    if (te->cursor_col < len)
    {
        for (i = te->cursor_col; i < len - 1; i++)
            te->lines[te->cursor_row][i] = te->lines[te->cursor_row][i + 1];
        te->line_len[te->cursor_row] = len - 1;
        te->lines[te->cursor_row][len - 1] = '\0';
    }
    else if (te->cursor_row < te->num_lines - 1)
    {
        next_len = te->line_len[te->cursor_row + 1];
        copy_len = next_len;
        if (len + copy_len > TE_MAX_LINE_LEN) copy_len = TE_MAX_LINE_LEN - len;

        for (i = 0; i < copy_len; i++)
            te->lines[te->cursor_row][len + i] = te->lines[te->cursor_row + 1][i];
        te->line_len[te->cursor_row] = len + copy_len;
        te->lines[te->cursor_row][len + copy_len] = '\0';

        for (i = te->cursor_row + 1; i < te->num_lines - 1; i++)
        {
            memcpy(te->lines[i], te->lines[i + 1], TE_MAX_LINE_LEN + 1);
            te->line_len[i] = te->line_len[i + 1];
        }
        te->num_lines--;
    }
}

static void te_insert_newline(TexEdit *te)
{
    int16_t i, len, tail;

    if (te->num_lines >= TE_MAX_LINES) return;

    for (i = te->num_lines; i > te->cursor_row + 1; i--)
    {
        memcpy(te->lines[i], te->lines[i - 1], TE_MAX_LINE_LEN + 1);
        te->line_len[i] = te->line_len[i - 1];
    }

    len  = te->line_len[te->cursor_row];
    tail = len - te->cursor_col;

    for (i = 0; i < tail; i++)
        te->lines[te->cursor_row + 1][i] = te->lines[te->cursor_row][te->cursor_col + i];
    te->lines[te->cursor_row + 1][tail] = '\0';
    te->line_len[te->cursor_row + 1] = tail;

    te->lines[te->cursor_row][te->cursor_col] = '\0';
    te->line_len[te->cursor_row] = te->cursor_col;

    te->num_lines++;
    te->cursor_row++;
    te->cursor_col = 0;
}

/* -------------------------------------------------------------------------
 * Drawing
 * ---------------------------------------------------------------------- */

void textedit_draw(TexEdit *te, int16_t cx, int16_t cy, int16_t cw, int16_t ch)
{
    int16_t wx, wy, ww, wh;
    int16_t row, first_row, last_row;
    int16_t y_top, i;
    char    display[TE_MAX_LINE_LEN + 1];
    int16_t len;
    int16_t cur_sc, cur_sr, cursor_x, cursor_y;
    int16_t vh = te->vdi_handle;

    te_get_work(te, &wx, &wy, &ww, &wh);

    if (cw <= 0 || ch <= 0) return;

    vs_clip(vh, 1, cx, cy, cx + cw - 1, cy + ch - 1);
    vswr_mode(vh, 1);
    vsf_color(vh, 0);
    vsf_interior(vh, 1);
    vr_recfl(vh, cx, cy, cx + cw - 1, cy + ch - 1);

    first_row = (cy - wy) / te->cell_h;
    last_row  = (cy + ch - 1 - wy) / te->cell_h;
    if (first_row < 0) first_row = 0;
    if (last_row  < 0) { vs_clip(vh, 0, 0, 0, 0, 0); return; }

    for (row = first_row; row <= last_row; row++)
    {
        int16_t line_idx = te->vscroll + row;
        if (line_idx >= te->num_lines) break;

        y_top = wy + row * te->cell_h;
        len   = te->line_len[line_idx] - te->hscroll;
        if (len <= 0) continue;

        i = 0;
        while (i < len && i < ww / te->cell_w + 1 && i < TE_MAX_LINE_LEN)
        {
            char c = te->lines[line_idx][te->hscroll + i];
            display[i++] = (c < 32) ? ' ' : c;
        }
        display[i] = '\0';

        v_gtext(vh, wx, y_top, display);
    }

    if (te->cursor_visible)
    {
        cur_sc = te->cursor_col - te->hscroll;
        cur_sr = te->cursor_row - te->vscroll;
        if (cur_sc >= 0 && cur_sr >= 0 &&
            cur_sc <= ww / te->cell_w &&
            cur_sr  < wh / te->cell_h)
        {
            cursor_x = wx + cur_sc * te->cell_w;
            cursor_y = wy + cur_sr * te->cell_h;
            vswr_mode(vh, 1);
            vsf_color(vh, 1);
            vsf_interior(vh, 1);
            vr_recfl(vh, cursor_x, cursor_y, cursor_x, cursor_y + te->cell_h - 1);
        }
    }

    vs_clip(vh, 0, 0, 0, 0, 0);
}

void textedit_redraw(TexEdit *te, int16_t *area)
{
    int16_t rect[4], r[4];
    int16_t wx, wy, ww, wh;

    if (te->win_handle < 0) return;

    te_get_work(te, &wx, &wy, &ww, &wh);
    graf_mouse(M_OFF);
    wind_update(1);

    wind_get(te->win_handle, WF_FIRSTXYWH, &rect[0], &rect[1], &rect[2], &rect[3]);
    while (rect[2] > 0 && rect[3] > 0)
    {
        r[0] = rect[0]; r[1] = rect[1]; r[2] = rect[2]; r[3] = rect[3];
        if (area != NULL)
        {
            if (te_rect_intersect(r, area[0], area[1], area[2], area[3]) &&
                te_rect_intersect(r, wx, wy, ww, wh))
                textedit_draw(te, r[0], r[1], r[2], r[3]);
        }
        else
        {
            if (te_rect_intersect(r, wx, wy, ww, wh))
                textedit_draw(te, r[0], r[1], r[2], r[3]);
        }
        wind_get(te->win_handle, WF_NEXTXYWH, &rect[0], &rect[1], &rect[2], &rect[3]);
    }

    wind_update(0);
    graf_mouse(M_ON);
}

void textedit_redraw_all(TexEdit *te)
{
    textedit_redraw(te, NULL);
}

/* Repaint screen rows [first_row, last_row] inclusive (0-based screen rows).
   Pass last_row = -1 to mean "to the bottom of the work area". */
static void te_redraw_rows(TexEdit *te, int16_t first_row, int16_t last_row)
{
    int16_t wx, wy, ww, wh;
    int16_t area[4];

    te_get_work(te, &wx, &wy, &ww, &wh);

    if (last_row < 0)
        last_row = wh / te->cell_h;

    area[0] = wx;
    area[1] = wy + first_row * te->cell_h;
    area[2] = ww;
    area[3] = (last_row - first_row + 1) * te->cell_h;

    textedit_redraw(te, area);
}

/* -------------------------------------------------------------------------
 * Scrollbars
 * ---------------------------------------------------------------------- */

void textedit_update_sliders(TexEdit *te)
{
    int16_t wx, wy, ww, wh;
    int16_t hmax, vmax, hpos, vpos, hsize, vsize;
    int16_t total_cols, total_rows, vis_cols, vis_rows;

    te_get_work(te, &wx, &wy, &ww, &wh);
    te_clamp_scroll(te, ww, wh);

    hmax = te_hscroll_max(te, ww);
    vmax = te_vscroll_max(te, wh);

    hpos = (hmax > 0) ? (int16_t)((long)te->hscroll * 1000L / hmax) : 0;
    vpos = (vmax > 0) ? (int16_t)((long)te->vscroll * 1000L / vmax) : 0;

    total_cols = te_max_line_width(te) + 1;
    total_rows = te->num_lines;
    vis_cols   = te_vis_cols(te, ww);
    vis_rows   = te_vis_rows(te, wh);
    if (total_cols < 1) total_cols = 1;
    if (total_rows < 1) total_rows = 1;

    hsize = (int16_t)((long)vis_cols * 1000L / total_cols);
    vsize = (int16_t)((long)vis_rows * 1000L / total_rows);
    if (hsize > 1000) hsize = 1000;
    if (hsize < 1)    hsize = 1;
    if (vsize > 1000) vsize = 1000;
    if (vsize < 1)    vsize = 1;

    wind_set(te->win_handle, WF_HSLIDE,  hpos,  0, 0, 0);
    wind_set(te->win_handle, WF_VSLIDE,  vpos,  0, 0, 0);
    wind_set(te->win_handle, WF_HSLSIZE, hsize, 0, 0, 0);
    wind_set(te->win_handle, WF_VSLSIZE, vsize, 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Input
 * ---------------------------------------------------------------------- */

/* Apply a keycode to the buffer and cursor.
   Returns dirty range in absolute buffer rows; first > last means unhandled. */
TEDirty textedit_apply_key(TexEdit *te, int16_t keycode)
{
    int16_t sc    = (keycode >> 8) & 0xFF;
    int16_t ascii = keycode & 0xFF;
    int16_t i;
    int16_t old_row = te->cursor_row;
    TEDirty d;

    d.first = old_row;
    d.last  = old_row;

    switch (sc)
    {
        case 0x48: /* up */
            if (te->cursor_row > 0) { te->cursor_row--; te_clamp_cursor(te); }
            d.first = te->cursor_row;
            break;
        case 0x50: /* down */
            if (te->cursor_row < te->num_lines - 1) { te->cursor_row++; te_clamp_cursor(te); }
            d.last = te->cursor_row;
            break;
        case 0x4B: /* left */
            if (te->cursor_col > 0) te->cursor_col--;
            else if (te->cursor_row > 0)
            {
                te->cursor_row--;
                te->cursor_col = te->line_len[te->cursor_row];
            }
            d.first = te->cursor_row;
            break;
        case 0x4D: /* right */
            if (te->cursor_col < te->line_len[te->cursor_row]) te->cursor_col++;
            else if (te->cursor_row < te->num_lines - 1)
            {
                te->cursor_row++;
                te->cursor_col = 0;
            }
            d.last = te->cursor_row;
            break;
        case 0x47: /* Home */
            te->cursor_col = 0;
            break;
        case 0x4F: /* End */
            te->cursor_col = te->line_len[te->cursor_row];
            break;
        case 0x53: /* Delete */
            te_delete_at(te);
            d.first = te->cursor_row;
            d.last  = TE_DIRTY_TO_END;
            break;
        default:
            if (ascii == 0x08)
            {
                te_delete_before(te);
                d.first = te->cursor_row;
                d.last  = TE_DIRTY_TO_END;
                break;
            }
            if (ascii == 0x0D || ascii == 0x0A)
            {
                te_insert_newline(te);
                d.last = TE_DIRTY_TO_END;
                break;
            }
            if (ascii == 0x09)
            {
                for (i = 0; i < TE_TAB_WIDTH; i++) te_insert_char(te, ' ');
                break;
            }
            if (ascii >= 32 && ascii < 127)
            {
                te_insert_char(te, (char)ascii);
                break;
            }
            /* unhandled: signal with first > last */
            d.first = 1;
            d.last  = 0;
            return d;
    }
    return d;
}

void textedit_finish_edit(TexEdit *te, TEDirty dirty)
{
    int16_t old_hscroll = te->hscroll;
    int16_t old_vscroll = te->vscroll;
    int16_t screen_first, screen_last;

    te_ensure_cursor_visible(te);
    textedit_update_sliders(te);

    if (te->hscroll != old_hscroll || te->vscroll != old_vscroll)
    {
        textedit_redraw_all(te);
        return;
    }

    screen_first = dirty.first - te->vscroll;
    screen_last  = (dirty.last == TE_DIRTY_TO_END)
                   ? -1
                   : dirty.last - te->vscroll;

    if (screen_first < 0) screen_first = 0;
    te_redraw_rows(te, screen_first, screen_last);
}

void textedit_key(TexEdit *te, int16_t keycode)
{
    TEDirty d = textedit_apply_key(te, keycode);
    if (d.first <= d.last || d.last == TE_DIRTY_TO_END)
        textedit_finish_edit(te, d);
}

void textedit_blink(TexEdit *te)
{
    int16_t wx, wy, ww, wh;
    int16_t cur_sc, cur_sr, cursor_x, cursor_y;
    int16_t vh = te->vdi_handle;

    te->cursor_visible = !te->cursor_visible;

    if (te->win_handle < 0) return;

    te_get_work(te, &wx, &wy, &ww, &wh);
    cur_sc = te->cursor_col - te->hscroll;
    cur_sr = te->cursor_row - te->vscroll;

    if (cur_sc < 0 || cur_sr < 0 ||
        cur_sc > ww / te->cell_w ||
        cur_sr >= wh / te->cell_h)
        return;

    cursor_x = wx + cur_sc * te->cell_w;
    cursor_y = wy + cur_sr * te->cell_h;

    graf_mouse(M_OFF);
    wind_update(1);

    vs_clip(vh, 1, cursor_x, cursor_y, cursor_x, cursor_y + te->cell_h - 1);
    vswr_mode(vh, 1);
    vsf_color(vh, te->cursor_visible ? 1 : 0);
    vsf_interior(vh, 1);
    vr_recfl(vh, cursor_x, cursor_y, cursor_x, cursor_y + te->cell_h - 1);
    vs_clip(vh, 0, 0, 0, 0, 0);

    wind_update(0);
    graf_mouse(M_ON);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

void textedit_init(TexEdit *te, int16_t win_handle, int16_t vdi_handle,
                   int16_t cell_w, int16_t cell_h)
{
    te->win_handle     = win_handle;
    te->vdi_handle     = vdi_handle;
    te->cell_w         = cell_w;
    te->cell_h         = cell_h;
    te->num_lines      = 0;
    te->cursor_row     = 0;
    te->cursor_col     = 0;
    te->cursor_visible = 1;
    te->hscroll        = 0;
    te->vscroll        = 0;

    /* ensure at least one empty line */
    te->lines[0][0]  = '\0';
    te->line_len[0]  = 0;
    te->num_lines    = 1;

    vst_alignment(vdi_handle, 0, 5); /* left, top */
}

/* -------------------------------------------------------------------------
 * Content API
 * ---------------------------------------------------------------------- */

void textedit_clear(TexEdit *te)
{
    te->num_lines      = 1;
    te->lines[0][0]    = '\0';
    te->line_len[0]    = 0;
    te->cursor_row     = 0;
    te->cursor_col     = 0;
    te->hscroll        = 0;
    te->vscroll        = 0;
    te->cursor_visible = 1;
}

void textedit_set_text(TexEdit *te, const char * const *lines, int16_t count)
{
    int16_t i, len;

    textedit_clear(te);
    if (count <= 0 || lines == NULL) return;
    if (count > TE_MAX_LINES) count = TE_MAX_LINES;

    for (i = 0; i < count; i++)
    {
        len = 0;
        if (lines[i] != NULL)
        {
            while (lines[i][len] != '\0' && len < TE_MAX_LINE_LEN) len++;
            memcpy(te->lines[i], lines[i], len);
        }
        te->lines[i][len] = '\0';
        te->line_len[i]   = len;
    }
    te->num_lines = count;
}

void textedit_append_line(TexEdit *te, const char *str)
{
    int16_t idx, len;

    if (te->num_lines >= TE_MAX_LINES) return;

    idx = te->num_lines;
    len = 0;
    if (str != NULL)
        while (str[len] != '\0' && len < TE_MAX_LINE_LEN) len++;

    if (str != NULL) memcpy(te->lines[idx], str, len);
    te->lines[idx][len] = '\0';
    te->line_len[idx]   = len;
    te->num_lines++;
}

int16_t textedit_get_line_count(const TexEdit *te)
{
    return te->num_lines;
}

int16_t textedit_get_line(const TexEdit *te, int16_t row,
                           char *buf, int16_t buflen)
{
    int16_t len;

    if (row < 0 || row >= te->num_lines || buf == NULL || buflen <= 0)
        return -1;

    len = te->line_len[row];
    if (len > buflen - 1) len = buflen - 1;
    memcpy(buf, te->lines[row], len);
    buf[len] = '\0';
    return len;
}
