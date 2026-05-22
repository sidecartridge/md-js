#ifndef TEXTEDIT_H
#define TEXTEDIT_H

#include <stdint.h>

#define TE_MAX_LINES    200
#define TE_MAX_LINE_LEN 255
#define TE_TAB_WIDTH    4
#define TE_BLINK_MS     500

typedef struct
{
    /* text buffer */
    char     lines[TE_MAX_LINES][TE_MAX_LINE_LEN + 1];
    int16_t  line_len[TE_MAX_LINES];
    int16_t  num_lines;

    /* cursor */
    int16_t  cursor_row;
    int16_t  cursor_col;
    int16_t  cursor_visible;

    /* scroll offsets (in characters) */
    int16_t  hscroll;
    int16_t  vscroll;

    /* window and VDI context set by the app before first use */
    int16_t  win_handle;
    int16_t  vdi_handle;
    int16_t  cell_w;
    int16_t  cell_h;
} TexEdit;

/* --- Lifecycle --- */
void    textedit_init(TexEdit *te, int16_t win_handle, int16_t vdi_handle,
                      int16_t cell_w, int16_t cell_h);

/* --- Content --- */
void    textedit_clear(TexEdit *te);
void    textedit_set_text(TexEdit *te, const char * const *lines, int16_t count);
void    textedit_append_line(TexEdit *te, const char *str);
int16_t textedit_get_line_count(const TexEdit *te);
int16_t textedit_get_line(const TexEdit *te, int16_t row, char *buf, int16_t buflen);

/* --- Drawing (call inside wind_update / graf_mouse guard) --- */
void    textedit_draw(TexEdit *te, int16_t cx, int16_t cy, int16_t cw, int16_t ch);
void    textedit_redraw(TexEdit *te, int16_t *area);
void    textedit_redraw_all(TexEdit *te);

/* --- Scrollbars --- */
void    textedit_update_sliders(TexEdit *te);

/* Dirty range in absolute buffer rows.
   last == TE_DIRTY_TO_END means "from first to bottom of screen". */
#define TE_DIRTY_TO_END  32767

typedef struct { int16_t first; int16_t last; } TEDirty;

/* Union two dirty ranges */
#define TE_DIRTY_UNION(a, b) \
    do { \
        if ((b).first < (a).first) (a).first = (b).first; \
        if ((b).last  > (a).last)  (a).last  = (b).last;  \
    } while (0)

/* --- Input --- */
/* Apply keycode to buffer; returns dirty buffer-row range (first>last = unhandled). */
TEDirty textedit_apply_key(TexEdit *te, int16_t keycode);
/* Redraw dirty range, update sliders, ensure cursor visible. */
void    textedit_finish_edit(TexEdit *te, TEDirty dirty);
void    textedit_key(TexEdit *te, int16_t keycode); /* single keypress convenience */
void    textedit_blink(TexEdit *te);

#endif /* TEXTEDIT_H */
