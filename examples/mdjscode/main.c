/**
 * File: demo_gem.c
 * Description: MD/JS Code — minimal GEM IDE for the SidecarTridge MD/JS
 * JavaScript Worker. Built as MDJSCODE.PRG.
 *
 * Layout:
 *   - Toolbar strip at the top of the work area with "About" and "Run"
 *   - Top fixed GEM window titled "Code" (editable via TexEdit)
 *   - Bottom fixed GEM window titled "Result"
 */

#include <gem.h>
#include <osbind.h>
#include <stdio.h>
#include <string.h>

#include "mdjs.h"
#include "textedit.h"

static short app_id;
static short aes_handle;
static VdiHdl virt_vdi = -1;
static short sys_char_w, sys_char_h, sys_cell_w, sys_cell_h;
static short desk_x, desk_y, desk_w, desk_h;
static short toolbar_h;

static short toolbar_win = -1;
static short code_win = -1;
static short result_win = -1;
static short quit_requested = 0;
static short result_top_line = 0;
static short result_left_col = 0;

/* TexEdit for the Code window — declared static because it's ~52 KB */
static TexEdit code_te;

static const char *const initial_code_lines[] = {
    "function main(name) {", "  return 'Hello, ' + (name || 'World') + '!';",
    "}", "", "/* Edit me! */"};
#define INITIAL_CODE_LINE_COUNT 5

static char current_result[256] = "";

static char about_button_label[] = "About";
static char load_button_label[] = "Load";
static char save_button_label[] = "Save";
static char run_button_label[] = "Run";
static char quit_button_label[] = "Quit";

/* Shared file selector state — remembers last directory across Load/Save */
static char fsel_path[128] = "";
static char fsel_file[13]  = "";

#define FUNC_LEN 32
#define ARGS_LEN 32
static char dialog_func[FUNC_LEN + 1];
static char dialog_func_template[FUNC_LEN + 1];
static char dialog_func_valid[FUNC_LEN + 1];
static char dialog_args[ARGS_LEN + 1];
static char dialog_args_template[ARGS_LEN + 1];
static char dialog_args_valid[ARGS_LEN + 1];

enum {
  DL_ROOT = 0,
  DL_FUNC_LABEL,
  DL_FUNC_INPUT,
  DL_ARGS_LABEL,
  DL_ARGS_INPUT,
  DL_OK,
  DL_CANCEL
};

#define DL_COUNT 7

static char d_func_label[] = "Function name";
static char d_args_label[] = "Parameters (JSON)";
static char d_ok[] = "OK";
static char d_cancel[] = "Cancel";

static OBJECT dialog_tree[DL_COUNT];
static TEDINFO dialog_func_ted;
static TEDINFO dialog_args_ted;

static short line_height(void) {
  short h = (short)(sys_char_h + 2);
  if (h < 10) {
    h = 10;
  }
  return h;
}

static short count_text_lines(const char *text) {
  short lines = 1;

  if (!text || !text[0]) {
    return 1;
  }

  while (*text) {
    if (*text == '\n') {
      lines++;
    }
    text++;
  }

  return lines;
}

static short max_text_columns(const char *text) {
  short max_cols = 0;
  short cols = 0;

  if (!text || !text[0]) {
    return 7;
  }

  while (*text) {
    if (*text == '\n') {
      if (cols > max_cols) {
        max_cols = cols;
      }
      cols = 0;
    } else {
      cols++;
    }
    text++;
  }

  if (cols > max_cols) {
    max_cols = cols;
  }

  return max_cols;
}

static short visible_lines_for_window(short win) {
  short wx, wy, ww, wh;
  short lh;
  short visible;

  wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
  lh = line_height();
  visible = (short)((wh - 4) / lh);
  if (visible < 1) {
    visible = 1;
  }
  return visible;
}

static short visible_columns_for_window(short win) {
  short wx, wy, ww, wh;
  short visible;

  wind_get(win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
  visible = (short)((ww - 8) / sys_char_w);
  if (visible < 1) {
    visible = 1;
  }
  return visible;
}

static short max_top_line_for_window(short win, const char *text) {
  short total = count_text_lines(text);
  short visible = visible_lines_for_window(win);

  if (total <= visible) {
    return 0;
  }
  return (short)(total - visible);
}

static short max_left_col_for_window(short win, const char *text) {
  short total = max_text_columns(text);
  short visible = visible_columns_for_window(win);

  if (total <= visible) {
    return 0;
  }
  return (short)(total - visible);
}

static void update_window_sliders(short win, const char *text, short top_line,
                                  short left_col) {
  short total = count_text_lines(text);
  short visible = visible_lines_for_window(win);
  short max_top = max_top_line_for_window(win, text);
  short total_cols = max_text_columns(text);
  short visible_cols = visible_columns_for_window(win);
  short max_left = max_left_col_for_window(win, text);
  short size;
  short pos;

  if (total <= visible) {
    size = 1000;
    pos = 0;
  } else {
    size = (short)((1000L * visible) / total);
    if (size < 1) {
      size = 1;
    }
    pos = (short)((1000L * top_line) / max_top);
  }

  wind_set(win, WF_VSLSIZE, size, 0, 0, 0);
  wind_set(win, WF_VSLIDE, pos, 0, 0, 0);

  if (total_cols <= visible_cols) {
    size = 1000;
    pos = 0;
  } else {
    size = (short)((1000L * visible_cols) / total_cols);
    if (size < 1) {
      size = 1;
    }
    pos = (short)((1000L * left_col) / max_left);
  }

  wind_set(win, WF_HSLSIZE, size, 0, 0, 0);
  wind_set(win, WF_HSLIDE, pos, 0, 0, 0);
}

static void redraw_all(void);
static void focus_result_window(void);
static void present_result_window(void);
static void fsel_init_path(void);

static void set_result_text(const char *text) {
  if (!text || !text[0]) {
    current_result[0] = '\0';
  } else {
    strncpy(current_result, text, sizeof(current_result) - 1);
    current_result[sizeof(current_result) - 1] = '\0';
  }
  result_top_line = 0;
  result_left_col = 0;
}

static void set_result_error_with_fallback(const char *fallback) {
  char buf[256];

  buf[0] = '\0';
  if (mdjs_result(buf, (int)sizeof(buf)) == 0 && buf[0] != '\0') {
    set_result_text(buf);
  } else {
    set_result_text(fallback);
  }
}

static void obfix_tree(OBJECT *tree, short count) {
  short i;

  for (i = 0; i < count; i++) {
    rsrc_obfix(tree, i);
  }
}

static void build_dialog(void) {
  short args_label_w = (short)strlen(d_args_label);
  short input_w = ARGS_LEN;
  short btn_w = 8;
  short dlg_w = (short)(input_w + 4);
  short i;

  if (args_label_w + 4 > dlg_w) dlg_w = (short)(args_label_w + 4);

  for (i = 0; i < FUNC_LEN; i++) {
    dialog_func_template[i] = '_';
    dialog_func_valid[i] = 'X';
  }
  dialog_func_template[FUNC_LEN] = '\0';
  dialog_func_valid[FUNC_LEN] = '\0';
  strcpy(dialog_func, "main");
  for (i = (short)strlen(dialog_func); i < FUNC_LEN; i++) dialog_func[i] = '\0';

  for (i = 0; i < ARGS_LEN; i++) {
    dialog_args_template[i] = '_';
    dialog_args_valid[i] = 'X';
  }
  dialog_args_template[ARGS_LEN] = '\0';
  dialog_args_valid[ARGS_LEN] = '\0';
  strcpy(dialog_args, "[]");
  for (i = (short)strlen(dialog_args); i < ARGS_LEN; i++) dialog_args[i] = '\0';

  dialog_func_ted.te_ptext = dialog_func;
  dialog_func_ted.te_ptmplt = dialog_func_template;
  dialog_func_ted.te_pvalid = dialog_func_valid;
  dialog_func_ted.te_font = 3;
  dialog_func_ted.te_fontid = 0;
  dialog_func_ted.te_just = 0;
  dialog_func_ted.te_color = 0x1180;
  dialog_func_ted.te_fontsize = 0;
  dialog_func_ted.te_thickness = -1;
  dialog_func_ted.te_txtlen = FUNC_LEN + 1;
  dialog_func_ted.te_tmplen = FUNC_LEN + 1;

  dialog_args_ted.te_ptext = dialog_args;
  dialog_args_ted.te_ptmplt = dialog_args_template;
  dialog_args_ted.te_pvalid = dialog_args_valid;
  dialog_args_ted.te_font = 3;
  dialog_args_ted.te_fontid = 0;
  dialog_args_ted.te_just = 0;
  dialog_args_ted.te_color = 0x1180;
  dialog_args_ted.te_fontsize = 0;
  dialog_args_ted.te_thickness = -1;
  dialog_args_ted.te_txtlen = ARGS_LEN + 1;
  dialog_args_ted.te_tmplen = ARGS_LEN + 1;

  /* Layout (character rows):
     0  top margin
     1  "Function:" label
     2  function input
     3  (blank line)
     4  "Parameters (JSON):" label
     5  args input
     6  top margin before buttons
     7  buttons
     8  bottom margin              => total height 9 */

  dialog_tree[DL_ROOT].ob_next = -1;
  dialog_tree[DL_ROOT].ob_head = DL_FUNC_LABEL;
  dialog_tree[DL_ROOT].ob_tail = DL_CANCEL;
  dialog_tree[DL_ROOT].ob_type = G_BOX;
  dialog_tree[DL_ROOT].ob_flags = OF_NONE;
  dialog_tree[DL_ROOT].ob_state = OS_OUTLINED;
  dialog_tree[DL_ROOT].ob_spec.index = 0x21100L;
  dialog_tree[DL_ROOT].ob_x = 0;
  dialog_tree[DL_ROOT].ob_y = 0;
  dialog_tree[DL_ROOT].ob_width = dlg_w;
  dialog_tree[DL_ROOT].ob_height = 9;

  dialog_tree[DL_FUNC_LABEL].ob_next = DL_FUNC_INPUT;
  dialog_tree[DL_FUNC_LABEL].ob_head = -1;
  dialog_tree[DL_FUNC_LABEL].ob_tail = -1;
  dialog_tree[DL_FUNC_LABEL].ob_type = G_STRING;
  dialog_tree[DL_FUNC_LABEL].ob_flags = OF_NONE;
  dialog_tree[DL_FUNC_LABEL].ob_state = OS_NORMAL;
  dialog_tree[DL_FUNC_LABEL].ob_spec.free_string = d_func_label;
  dialog_tree[DL_FUNC_LABEL].ob_x = 2;
  dialog_tree[DL_FUNC_LABEL].ob_y = 1;
  dialog_tree[DL_FUNC_LABEL].ob_width = (short)strlen(d_func_label);
  dialog_tree[DL_FUNC_LABEL].ob_height = 1;

  dialog_tree[DL_FUNC_INPUT].ob_next = DL_ARGS_LABEL;
  dialog_tree[DL_FUNC_INPUT].ob_head = -1;
  dialog_tree[DL_FUNC_INPUT].ob_tail = -1;
  dialog_tree[DL_FUNC_INPUT].ob_type = G_FTEXT;
  dialog_tree[DL_FUNC_INPUT].ob_flags = OF_EDITABLE;
  dialog_tree[DL_FUNC_INPUT].ob_state = OS_NORMAL;
  dialog_tree[DL_FUNC_INPUT].ob_spec.tedinfo = &dialog_func_ted;
  dialog_tree[DL_FUNC_INPUT].ob_x = 2;
  dialog_tree[DL_FUNC_INPUT].ob_y = 2;
  dialog_tree[DL_FUNC_INPUT].ob_width = FUNC_LEN;
  dialog_tree[DL_FUNC_INPUT].ob_height = 1;

  dialog_tree[DL_ARGS_LABEL].ob_next = DL_ARGS_INPUT;
  dialog_tree[DL_ARGS_LABEL].ob_head = -1;
  dialog_tree[DL_ARGS_LABEL].ob_tail = -1;
  dialog_tree[DL_ARGS_LABEL].ob_type = G_STRING;
  dialog_tree[DL_ARGS_LABEL].ob_flags = OF_NONE;
  dialog_tree[DL_ARGS_LABEL].ob_state = OS_NORMAL;
  dialog_tree[DL_ARGS_LABEL].ob_spec.free_string = d_args_label;
  dialog_tree[DL_ARGS_LABEL].ob_x = 2;
  dialog_tree[DL_ARGS_LABEL].ob_y = 4;
  dialog_tree[DL_ARGS_LABEL].ob_width = args_label_w;
  dialog_tree[DL_ARGS_LABEL].ob_height = 1;

  dialog_tree[DL_ARGS_INPUT].ob_next = DL_OK;
  dialog_tree[DL_ARGS_INPUT].ob_head = -1;
  dialog_tree[DL_ARGS_INPUT].ob_tail = -1;
  dialog_tree[DL_ARGS_INPUT].ob_type = G_FTEXT;
  dialog_tree[DL_ARGS_INPUT].ob_flags = OF_EDITABLE;
  dialog_tree[DL_ARGS_INPUT].ob_state = OS_NORMAL;
  dialog_tree[DL_ARGS_INPUT].ob_spec.tedinfo = &dialog_args_ted;
  dialog_tree[DL_ARGS_INPUT].ob_x = 2;
  dialog_tree[DL_ARGS_INPUT].ob_y = 5;
  dialog_tree[DL_ARGS_INPUT].ob_width = input_w;
  dialog_tree[DL_ARGS_INPUT].ob_height = 1;

  dialog_tree[DL_OK].ob_next = DL_CANCEL;
  dialog_tree[DL_OK].ob_head = -1;
  dialog_tree[DL_OK].ob_tail = -1;
  dialog_tree[DL_OK].ob_type = G_BUTTON;
  dialog_tree[DL_OK].ob_flags = OF_SELECTABLE | OF_EXIT | OF_DEFAULT;
  dialog_tree[DL_OK].ob_state = OS_NORMAL;
  dialog_tree[DL_OK].ob_spec.free_string = d_ok;
  dialog_tree[DL_OK].ob_x = 2;
  dialog_tree[DL_OK].ob_y = 7;
  dialog_tree[DL_OK].ob_width = btn_w;
  dialog_tree[DL_OK].ob_height = 1;

  dialog_tree[DL_CANCEL].ob_next = DL_ROOT;
  dialog_tree[DL_CANCEL].ob_head = -1;
  dialog_tree[DL_CANCEL].ob_tail = -1;
  dialog_tree[DL_CANCEL].ob_type = G_BUTTON;
  dialog_tree[DL_CANCEL].ob_flags = OF_SELECTABLE | OF_EXIT | OF_LASTOB;
  dialog_tree[DL_CANCEL].ob_state = OS_NORMAL;
  dialog_tree[DL_CANCEL].ob_spec.free_string = d_cancel;
  dialog_tree[DL_CANCEL].ob_x = (short)(dlg_w - btn_w - 2);
  dialog_tree[DL_CANCEL].ob_y = 7;
  dialog_tree[DL_CANCEL].ob_width = btn_w;
  dialog_tree[DL_CANCEL].ob_height = 1;

  obfix_tree(dialog_tree, DL_COUNT);
}

static void open_windows(void) {
  short content_y;
  short content_h;
  short half_h;
  short work_in[11];
  short work_out[57];
  short i;
  short cell_w, cell_h;

  wind_get(0, WF_WORKXYWH, &desk_x, &desk_y, &desk_w, &desk_h);

  toolbar_h = (short)(sys_cell_h + 14);
  if (toolbar_h < 22) {
    toolbar_h = 22;
  }

  toolbar_win = wind_create(0, desk_x, desk_y, desk_w, toolbar_h);
  if (toolbar_win >= 0) {
    wind_open(toolbar_win, desk_x, desk_y, desk_w, toolbar_h);
  }

  content_y = (short)(desk_y + toolbar_h);
  content_h = (short)(desk_h - toolbar_h);
  half_h = (short)(content_h / 2);

  code_win = wind_create(
      NAME | CLOSER | UPARROW | DNARROW | VSLIDE | LFARROW | RTARROW | HSLIDE,
      desk_x, content_y, desk_w, half_h);
  if (code_win < 0) {
    return;
  }
  wind_set_str(code_win, WF_NAME, " Code ");
  wind_open(code_win, desk_x, content_y, desk_w, half_h);

  result_win = wind_create(
      NAME | CLOSER | UPARROW | DNARROW | VSLIDE | LFARROW | RTARROW | HSLIDE,
      desk_x, (short)(content_y + half_h), desk_w, (short)(content_h - half_h));
  if (result_win < 0) {
    return;
  }
  wind_set_str(result_win, WF_NAME, " Result ");
  wind_open(result_win, desk_x, (short)(content_y + half_h), desk_w,
            (short)(content_h - half_h));

  /* Open virtual VDI workstation and get accurate cell metrics */
  for (i = 0; i < 10; i++) work_in[i] = 1;
  work_in[10] = 2;
  v_opnvwk(work_in, &virt_vdi, work_out);

  cell_w = sys_cell_w;
  cell_h = sys_cell_h;

  /* Override cell_w for ST low-res pixel doubling.
     atrib[7..12] = ptsout; atrib[9] = ptsout[2] = rendered char width. */
  if (virt_vdi >= 0) {
    short atrib[13];
    vqt_attributes(virt_vdi, atrib);
    if (atrib[9] > 0) cell_w = atrib[9];
  }

  /* Init textedit for Code window */
  textedit_init(&code_te, code_win, virt_vdi, cell_w, cell_h);
  textedit_set_text(&code_te, initial_code_lines, INITIAL_CODE_LINE_COUNT);
  textedit_update_sliders(&code_te);

  update_window_sliders(result_win, current_result, result_top_line,
                        result_left_col);

  /* Keep Code focused at startup even though Result is opened afterwards. */
  wind_set(code_win, WF_TOP, 0, 0, 0, 0);
}

static void get_toolbar_layout(short *about_x, short *about_y, short *about_w,
                               short *about_h, short *load_x, short *load_y,
                               short *load_w, short *load_h, short *save_x,
                               short *save_y, short *save_w, short *save_h,
                               short *run_x, short *run_y, short *run_w,
                               short *run_h, short *quit_x, short *quit_y,
                               short *quit_w, short *quit_h) {
  short wx, wy, ww, wh;
  short btn_h;
  short btn_y;

  wind_get(toolbar_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);

  btn_h = (short)(sys_char_h + 8);
  if (btn_h < 14) {
    btn_h = 14;
  }
  btn_y = (short)(wy + ((wh - btn_h) / 2));

  *about_x = (short)(wx + 8);
  *about_y = btn_y;
  *about_w = (short)((short)strlen(about_button_label) * sys_char_w + 14);
  *about_h = btn_h;

  *load_x = (short)(*about_x + *about_w + 8);
  *load_y = btn_y;
  *load_w = (short)((short)strlen(load_button_label) * sys_char_w + 14);
  *load_h = btn_h;

  *save_x = (short)(*load_x + *load_w + 8);
  *save_y = btn_y;
  *save_w = (short)((short)strlen(save_button_label) * sys_char_w + 14);
  *save_h = btn_h;

  *run_x = (short)(*save_x + *save_w + 8);
  *run_y = btn_y;
  *run_w = (short)((short)strlen(run_button_label) * sys_char_w + 14);
  *run_h = btn_h;

  *quit_w = (short)((short)strlen(quit_button_label) * sys_char_w + 14);
  *quit_h = btn_h;
  *quit_x = (short)(wx + ww - *quit_w - 8);
  *quit_y = btn_y;
}

static void fill_clip_rect(short x, short y, short w, short h) {
  short pxy[4];

  pxy[0] = x;
  pxy[1] = y;
  pxy[2] = (short)(x + w - 1);
  pxy[3] = (short)(y + h - 1);

  vswr_mode(aes_handle, MD_REPLACE);
  vsf_color(aes_handle, 0);
  vsf_interior(aes_handle, FIS_SOLID);
  vsf_perimeter(aes_handle, 0);
  vr_recfl(aes_handle, pxy);
}

static void draw_button(short x, short y, short w, short h, const char *label) {
  short fill[4];
  short outline[10];
  short text_x;
  short text_y;

  fill[0] = x;
  fill[1] = y;
  fill[2] = (short)(x + w - 1);
  fill[3] = (short)(y + h - 1);

  vswr_mode(aes_handle, MD_REPLACE);
  vsf_color(aes_handle, 0);
  vsf_interior(aes_handle, FIS_SOLID);
  vsf_perimeter(aes_handle, 0);
  vr_recfl(aes_handle, fill);

  outline[0] = x;
  outline[1] = y;
  outline[2] = (short)(x + w - 1);
  outline[3] = y;
  outline[4] = (short)(x + w - 1);
  outline[5] = (short)(y + h - 1);
  outline[6] = x;
  outline[7] = (short)(y + h - 1);
  outline[8] = x;
  outline[9] = y;

  vswr_mode(aes_handle, MD_REPLACE);
  vsl_color(aes_handle, 1);
  v_pline(aes_handle, 5, outline);

  text_x = (short)(x + ((w - ((short)strlen(label) * sys_char_w)) / 2));
  text_y = (short)(y + (h + sys_char_h * 3 / 4) / 2);
  vst_effects(aes_handle, 0);
  vst_color(aes_handle, 1);
  v_gtext(aes_handle, text_x, text_y, (char *)label);
}

static void draw_multiline_text(const char *text, short left, short top,
                                short bottom, short first_line,
                                short first_col) {
  short line_y;
  short line_h;
  char line_buf[160];
  short current_line = 0;

  line_h = line_height();
  line_y = (short)(top + sys_char_h);

  vswr_mode(aes_handle, MD_REPLACE);
  vst_effects(aes_handle, 0);
  vst_color(aes_handle, 1);

  if (text && text[0]) {
    const char *p = text;
    while (*p && line_y + sys_char_h <= bottom) {
      const char *eol = p;
      short len;

      while (*eol && *eol != '\n') {
        eol++;
      }
      len = (short)(eol - p);
      if (len > (short)(sizeof(line_buf) - 1)) {
        len = (short)(sizeof(line_buf) - 1);
      }

      if (current_line >= first_line) {
        short copy_start = first_col;
        short copy_len = len;

        if (copy_start > copy_len) {
          copy_start = copy_len;
        }
        copy_len = (short)(copy_len - copy_start);
        if (copy_len > (short)(sizeof(line_buf) - 1)) {
          copy_len = (short)(sizeof(line_buf) - 1);
        }
        memcpy(line_buf, p + copy_start, (size_t)copy_len);
        line_buf[copy_len] = '\0';
        v_gtext(aes_handle, left, line_y, line_buf);
        line_y = (short)(line_y + line_h);
      }
      current_line++;
      if (*eol == '\0') {
        break;
      }
      p = eol + 1;
    }
  } else {
    v_gtext(aes_handle, left, line_y, "Click Run to execute the code");
  }
}

static void redraw_toolbar_window(void) {
  short clip_x, clip_y, clip_w, clip_h;
  short about_x, about_y, about_w, about_h;
  short load_x, load_y, load_w, load_h;
  short save_x, save_y, save_w, save_h;
  short run_x, run_y, run_w, run_h;
  short quit_x, quit_y, quit_w, quit_h;
  short clip[4];

  if (toolbar_win < 0) {
    return;
  }

  wind_update(BEG_UPDATE);
  graf_mouse(M_OFF, NULL);

  get_toolbar_layout(&about_x, &about_y, &about_w, &about_h,
                     &load_x, &load_y, &load_w, &load_h,
                     &save_x, &save_y, &save_w, &save_h,
                     &run_x, &run_y, &run_w, &run_h,
                     &quit_x, &quit_y, &quit_w, &quit_h);

  wind_get(toolbar_win, WF_FIRSTXYWH, &clip_x, &clip_y, &clip_w, &clip_h);
  while (clip_w > 0 && clip_h > 0) {
    clip[0] = clip_x;
    clip[1] = clip_y;
    clip[2] = (short)(clip_x + clip_w - 1);
    clip[3] = (short)(clip_y + clip_h - 1);

    vs_clip(aes_handle, 1, clip);
    fill_clip_rect(clip_x, clip_y, clip_w, clip_h);
    draw_button(about_x, about_y, about_w, about_h, about_button_label);
    draw_button(load_x, load_y, load_w, load_h, load_button_label);
    draw_button(save_x, save_y, save_w, save_h, save_button_label);
    draw_button(run_x, run_y, run_w, run_h, run_button_label);
    draw_button(quit_x, quit_y, quit_w, quit_h, quit_button_label);
    vs_clip(aes_handle, 0, clip);

    wind_get(toolbar_win, WF_NEXTXYWH, &clip_x, &clip_y, &clip_w, &clip_h);
  }

  graf_mouse(M_ON, NULL);
  graf_mouse(ARROW, NULL);
  wind_update(END_UPDATE);
}

static void redraw_code_window(void) {
  if (code_win < 0) {
    return;
  }
  textedit_redraw_all(&code_te);
}

static void redraw_result_window(void) {
  short clip_x, clip_y, clip_w, clip_h;
  short work_x, work_y, work_w, work_h;
  short clip[4];

  if (result_win < 0) {
    return;
  }

  wind_update(BEG_UPDATE);
  graf_mouse(M_OFF, NULL);

  wind_get(result_win, WF_WORKXYWH, &work_x, &work_y, &work_w, &work_h);
  wind_get(result_win, WF_FIRSTXYWH, &clip_x, &clip_y, &clip_w, &clip_h);
  while (clip_w > 0 && clip_h > 0) {
    clip[0] = clip_x;
    clip[1] = clip_y;
    clip[2] = (short)(clip_x + clip_w - 1);
    clip[3] = (short)(clip_y + clip_h - 1);

    vs_clip(aes_handle, 1, clip);
    fill_clip_rect(clip_x, clip_y, clip_w, clip_h);
    draw_multiline_text(current_result, (short)(work_x + 4), work_y,
                        (short)(work_y + work_h - 4), result_top_line,
                        result_left_col);
    vs_clip(aes_handle, 0, clip);

    wind_get(result_win, WF_NEXTXYWH, &clip_x, &clip_y, &clip_w, &clip_h);
  }

  graf_mouse(M_ON, NULL);
  graf_mouse(ARROW, NULL);
  wind_update(END_UPDATE);
  update_window_sliders(result_win, current_result, result_top_line,
                        result_left_col);
}

static void do_redraw(short win) {
  if (win == toolbar_win) {
    redraw_toolbar_window();
  } else if (win == code_win) {
    redraw_code_window();
  } else if (win == result_win) {
    redraw_result_window();
  }
}

static short run_dialog(void) {
  short x, y, w, h;
  short exit_obj;

  strcpy(dialog_args, "[]");

  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  form_center(dialog_tree, &x, &y, &w, &h);
  form_dial(FMD_START, 0, 0, 0, 0, x, y, w, h);
  form_dial(FMD_GROW, 0, 0, 0, 0, x, y, w, h);

  objc_draw(dialog_tree, ROOT, MAX_DEPTH, x, y, w, h);
  exit_obj = (short)(form_do(dialog_tree, DL_FUNC_INPUT) & 0x7FFF);
  dialog_tree[exit_obj].ob_state &= ~OS_SELECTED;

  form_dial(FMD_SHRINK, x, y, w, h, 0, 0, 0, 0);
  form_dial(FMD_FINISH, x, y, w, h, 0, 0, 0, 0);
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);
  redraw_all();
  graf_mouse(ARROW, NULL);

  return (short)(exit_obj == DL_OK);
}

/* Flatten the TexEdit buffer back into a single string for upload */
static void build_upload_buf(char *buf, int buflen) {
  short r, n;
  char line[TE_MAX_LINE_LEN + 1];
  int pos = 0;

  n = textedit_get_line_count(&code_te);
  for (r = 0; r < n; r++) {
    short len = textedit_get_line(&code_te, r, line, (short)sizeof(line));
    if (len < 0) len = 0;
    if (pos + len + 1 >= buflen) break;
    memcpy(buf + pos, line, len);
    pos += len;
    if (r < n - 1) {
      buf[pos++] = '\n';
    }
  }
  buf[pos] = '\0';
}

static void do_run(void) {
  short err;
  char args_input[ARGS_LEN + 4];
  char result[256];
  static char upload_buf[TE_MAX_LINES * (TE_MAX_LINE_LEN + 1)];

  if (!run_dialog()) {
    return;
  }

  {
    short i, end;

    strcpy(args_input, dialog_args);
    end = (short)strlen(args_input);
    for (i = (short)(end - 1); i >= 0; i--) {
      if (args_input[i] == ' ' || args_input[i] == '_') {
        args_input[i] = '\0';
      } else {
        break;
      }
    }
    if (args_input[0] == '\0') {
      strcpy(args_input, "[]");
    }
  }

  err = (short)mdjs_ping();
  if (err != 0) {
    set_result_text("Error: no SidecarT or MD/JS not loaded.");
    present_result_window();
    return;
  }

  build_upload_buf(upload_buf, (int)sizeof(upload_buf));

  err = (short)mdjs_upload(upload_buf);
  if (err != 0) {
    set_result_error_with_fallback("Error: upload failed.");
    present_result_window();
    return;
  }

  memset(result, 0, sizeof(result));
  {
    short i, end;
    end = (short)strlen(dialog_func);
    for (i = (short)(end - 1); i >= 0; i--) {
      if (dialog_func[i] == ' ' || dialog_func[i] == '_') {
        dialog_func[i] = '\0';
      } else {
        break;
      }
    }
    if (dialog_func[0] == '\0') strcpy(dialog_func, "main");
  }

  err = (short)mdjs_call(dialog_func, args_input, result, (int)sizeof(result));
  if (err != 0) {
    set_result_error_with_fallback("Error: call failed.");
    present_result_window();
    return;
  }

  set_result_text(result);
  present_result_window();
}

static void do_about(void) {
  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  form_alert(1,
             "[1][MD/JS Code"
             "| "
             "|Want to embed MD/JS in your"
             "|own ST apps? Visit"
             "|github.com/neilrackett/md-js][OK]");
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);

  redraw_all();
  graf_mouse(ARROW, NULL);
}

static void do_save(void) {
  short exit_btn;
  char save_file[13] = "";
  char save_path[128];
  char full[128 + 13];
  char *slash;
  long fh;
  static char line_buf[TE_MAX_LINE_LEN + 2];
  short r, n, len;

  fsel_init_path();

  /* Build a save-path using the current fsel directory but no wildcard */
  strcpy(save_path, fsel_path);
  slash = save_path;
  {
    char *p = save_path;
    while (*p) { if (*p == '\\') slash = p; p++; }
  }
  slash[1] = '\0';
  strcat(save_path, "*.JS");

  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  fsel_exinput(save_path, save_file, &exit_btn, "Save JavaScript file");
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);
  redraw_all();
  graf_mouse(ARROW, NULL);

  if (exit_btn != 1 || save_file[0] == '\0') {
    return;
  }

  /* Build full path */
  strcpy(full, save_path);
  slash = full;
  {
    char *p = full;
    while (*p) { if (*p == '\\') slash = p; p++; }
  }
  slash[1] = '\0';
  strcat(full, save_file);

  fh = Fcreate(full, 0);
  if (fh < 0) {
    form_alert(1, "[1][Could not create file.][OK]");
    return;
  }

  n = textedit_get_line_count(&code_te);
  for (r = 0; r < n; r++) {
    len = textedit_get_line(&code_te, r, line_buf, TE_MAX_LINE_LEN + 1);
    if (len < 0) len = 0;
    if (r < n - 1) {
      line_buf[len]     = '\r';
      line_buf[len + 1] = '\n';
      len += 2;
    }
    Fwrite((short)fh, (long)len, line_buf);
  }

  Fclose((short)fh);

  /* Update shared fsel path to the saved directory */
  {
    short dirlen = (short)(slash - full + 1);
    memcpy(fsel_path, full, dirlen);
    strcpy(fsel_path + dirlen, "*.JS");
  }
}

static void fsel_init_path(void) {
  short drive, len;

  if (fsel_path[0] != '\0') return;

  drive = Dgetdrv();
  fsel_path[0] = (char)('A' + drive);
  fsel_path[1] = ':';
  fsel_path[2] = '\\';
  Dgetpath(fsel_path + 3, (short)(drive + 1));
  len = (short)strlen(fsel_path);
  if (fsel_path[len - 1] != '\\') {
    fsel_path[len]     = '\\';
    fsel_path[len + 1] = '\0';
  }
  strcat(fsel_path, "*.JS");
}

static void do_load(void) {
  short exit_btn;

  fsel_init_path();

  wind_update(BEG_UPDATE);
  wind_update(BEG_MCTRL);
  fsel_exinput(fsel_path, fsel_file, &exit_btn, "Load JavaScript file");
  wind_update(END_MCTRL);
  wind_update(END_UPDATE);
  redraw_all();
  graf_mouse(ARROW, NULL);

  if (exit_btn != 1 || fsel_file[0] == '\0') {
    return;
  }

  {
    /* Build full path: fsel_path ends at last backslash (or is bare drive),
       append the filename returned in fsel_file. */
    char full[128 + 13];
    char *slash;
    short len;
    long fh;
    long file_size;
    char *buf;
    static char line_buf[TE_MAX_LINE_LEN + 1];
    long bytes_read;

    strcpy(full, fsel_path);
    slash = full;
    {
      char *p = full;
      while (*p) { if (*p == '\\') slash = p; p++; }
    }
    slash[1] = '\0';
    strcat(full, fsel_file);

    fh = Fopen(full, 0);
    if (fh < 0) {
      form_alert(1, "[1][Could not open file.][OK]");
      return;
    }

    file_size = Fseek(0L, (short)fh, 2);
    Fseek(0L, (short)fh, 0);

    if (file_size <= 0 || file_size > (long)(TE_MAX_LINES * (TE_MAX_LINE_LEN + 1))) {
      form_alert(1, "[1][File is empty or too large.][OK]");
      Fclose((short)fh);
      return;
    }

    buf = (char *)Malloc(file_size + 1);
    if (!buf) {
      form_alert(1, "[1][Not enough memory.][OK]");
      Fclose((short)fh);
      return;
    }

    bytes_read = Fread((short)fh, file_size, buf);
    Fclose((short)fh);
    buf[bytes_read > 0 ? bytes_read : 0] = '\0';

    /* Split on newlines and load into editor.
       Zero num_lines directly so append_line fills from row 0 without
       the ghost empty line that textedit_clear intentionally leaves. */
    textedit_clear(&code_te);
    code_te.num_lines = 0;
    {
      char *p = buf;
      char *eol;
      short line_len;

      while (*p) {
        eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        line_len = (short)(eol - p);
        if (line_len > TE_MAX_LINE_LEN) line_len = TE_MAX_LINE_LEN;
        memcpy(line_buf, p, line_len);
        line_buf[line_len] = '\0';
        textedit_append_line(&code_te, line_buf);
        if (*eol == '\r' && *(eol + 1) == '\n') eol++;
        p = (*eol) ? eol + 1 : eol;
      }
    }

    Mfree(buf);

    /* Update the fsel path to point to the directory we just loaded from */
    len = (short)(slash - full + 1);
    memcpy(fsel_path, full, len);
    strcpy(fsel_path + len, "*.JS");

    textedit_update_sliders(&code_te);
    textedit_redraw_all(&code_te);
  }
}

static void scroll_window(short win, const char *text, short *top_line,
                          short delta) {
  short max_top = max_top_line_for_window(win, text);
  short new_top = (short)(*top_line + delta);

  if (new_top < 0) {
    new_top = 0;
  } else if (new_top > max_top) {
    new_top = max_top;
  }

  if (new_top == *top_line) {
    return;
  }

  *top_line = new_top;
  do_redraw(win);
}

static void slider_window(short win, const char *text, short *top_line,
                          short pos) {
  short max_top = max_top_line_for_window(win, text);
  short new_top;

  if (max_top <= 0) {
    new_top = 0;
  } else {
    new_top = (short)((pos * max_top + 500L) / 1000L);
  }

  if (new_top == *top_line) {
    return;
  }

  *top_line = new_top;
  do_redraw(win);
}

static void hscroll_window(short win, const char *text, short *left_col,
                           short delta) {
  short max_left = max_left_col_for_window(win, text);
  short new_left = (short)(*left_col + delta);

  if (new_left < 0) {
    new_left = 0;
  } else if (new_left > max_left) {
    new_left = max_left;
  }

  if (new_left == *left_col) {
    return;
  }

  *left_col = new_left;
  do_redraw(win);
}

static void hslider_window(short win, const char *text, short *left_col,
                           short pos) {
  short max_left = max_left_col_for_window(win, text);
  short new_left;

  if (max_left <= 0) {
    new_left = 0;
  } else {
    new_left = (short)((pos * max_left + 500L) / 1000L);
  }

  if (new_left == *left_col) {
    return;
  }

  *left_col = new_left;
  do_redraw(win);
}

static void handle_toolbar_click(short mouse_x, short mouse_y) {
  short about_x, about_y, about_w, about_h;
  short load_x, load_y, load_w, load_h;
  short save_x, save_y, save_w, save_h;
  short run_x, run_y, run_w, run_h;
  short quit_x, quit_y, quit_w, quit_h;

  get_toolbar_layout(&about_x, &about_y, &about_w, &about_h,
                     &load_x, &load_y, &load_w, &load_h,
                     &save_x, &save_y, &save_w, &save_h,
                     &run_x, &run_y, &run_w, &run_h,
                     &quit_x, &quit_y, &quit_w, &quit_h);

  if (mouse_x >= about_x && mouse_x < about_x + about_w && mouse_y >= about_y &&
      mouse_y < about_y + about_h) {
    do_about();
  } else if (mouse_x >= load_x && mouse_x < load_x + load_w &&
             mouse_y >= load_y && mouse_y < load_y + load_h) {
    do_load();
  } else if (mouse_x >= save_x && mouse_x < save_x + save_w &&
             mouse_y >= save_y && mouse_y < save_y + save_h) {
    do_save();
  } else if (mouse_x >= run_x && mouse_x < run_x + run_w && mouse_y >= run_y &&
             mouse_y < run_y + run_h) {
    do_run();
  } else if (mouse_x >= quit_x && mouse_x < quit_x + quit_w &&
             mouse_y >= quit_y && mouse_y < quit_y + quit_h) {
    quit_requested = 1;
  }
}

static void handle_code_arrowed(short direction) {
  short wx, wy, ww, wh;
  short vis_rows, vis_cols;

  wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
  vis_rows = (code_te.cell_h > 0) ? wh / code_te.cell_h : 1;
  vis_cols = (code_te.cell_w > 0) ? ww / code_te.cell_w : 1;

  switch (direction) {
    case WA_UPLINE:
      code_te.vscroll--;
      break;
    case WA_DNLINE:
      code_te.vscroll++;
      break;
    case WA_UPPAGE:
      code_te.vscroll -= vis_rows;
      break;
    case WA_DNPAGE:
      code_te.vscroll += vis_rows;
      break;
    case WA_LFLINE:
      code_te.hscroll--;
      break;
    case WA_RTLINE:
      code_te.hscroll++;
      break;
    case WA_LFPAGE:
      code_te.hscroll -= vis_cols;
      break;
    case WA_RTPAGE:
      code_te.hscroll += vis_cols;
      break;
  }
  textedit_update_sliders(&code_te);
  textedit_redraw_all(&code_te);
}

static void event_loop(void) {
  short ev;
  short msg[8];
  short mx, my, mb, ks, key, clicks;
  short last_mb;

  redraw_toolbar_window();
  redraw_code_window();
  redraw_result_window();
  graf_mkstate(&mx, &my, &last_mb, &ks);

  while (!quit_requested) {
    ev = evnt_multi(MU_MESAG | MU_KEYBD | MU_TIMER, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, msg, (long)TE_BLINK_MS, &mx, &my, &mb, &ks,
                    &key, &clicks);

    if (last_mb == 0 && mb != 0) {
      short wx, wy, ww, wh;

      if (toolbar_win >= 0) {
        wind_get(toolbar_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
        if (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh) {
          handle_toolbar_click(mx, my);
        }
      }
      if (code_win >= 0) {
        wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
        if (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh) {
          textedit_click(&code_te, mx, my);
        }
      }
    }
    last_mb = mb;

    if (ev & MU_TIMER) {
      textedit_blink(&code_te);
    }

    if (ev & MU_KEYBD) {
      TEDirty batch;
      short peek_ev;
      short peek_msg[8];
      short peek_mx, peek_my, peek_mb, peek_ks, peek_key, peek_clicks;

      code_te.cursor_visible = 1;
      batch = textedit_apply_key(&code_te, key);

      /* Drain queued keystrokes to reduce flicker */
      do {
        peek_ev = evnt_multi(MU_KEYBD | MU_TIMER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, peek_msg, 0L, &peek_mx, &peek_my,
                             &peek_mb, &peek_ks, &peek_key, &peek_clicks);
        if (peek_ev & MU_KEYBD) {
          TEDirty d = textedit_apply_key(&code_te, peek_key);
          TE_DIRTY_UNION(batch, d);
        }
      } while (peek_ev & MU_KEYBD);

      textedit_finish_edit(&code_te, batch);
    }

    if (!(ev & MU_MESAG)) {
      continue;
    }

    switch (msg[0]) {
      case WM_REDRAW:
        if (msg[3] == code_win) {
          short area[4];
          area[0] = msg[4];
          area[1] = msg[5];
          area[2] = msg[6];
          area[3] = msg[7];
          textedit_redraw(&code_te, area);
        } else {
          do_redraw(msg[3]);
        }
        break;

      case WM_ARROWED:
        if (msg[3] == code_win) {
          handle_code_arrowed(msg[4]);
        } else if (msg[3] == result_win) {
          switch (msg[4]) {
            case WA_UPLINE:
              scroll_window(result_win, current_result, &result_top_line, -1);
              break;
            case WA_DNLINE:
              scroll_window(result_win, current_result, &result_top_line, 1);
              break;
            case WA_UPPAGE:
              scroll_window(result_win, current_result, &result_top_line,
                            -visible_lines_for_window(result_win));
              break;
            case WA_DNPAGE:
              scroll_window(result_win, current_result, &result_top_line,
                            visible_lines_for_window(result_win));
              break;
            case WA_LFLINE:
              hscroll_window(result_win, current_result, &result_left_col, -1);
              break;
            case WA_RTLINE:
              hscroll_window(result_win, current_result, &result_left_col, 1);
              break;
            case WA_LFPAGE:
              hscroll_window(result_win, current_result, &result_left_col,
                             -visible_columns_for_window(result_win));
              break;
            case WA_RTPAGE:
              hscroll_window(result_win, current_result, &result_left_col,
                             visible_columns_for_window(result_win));
              break;
          }
        }
        break;

      case WM_VSLID:
        if (msg[3] == code_win) {
          short wx, wy, ww, wh;
          short vmax;
          wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
          vmax = code_te.num_lines -
                 ((code_te.cell_h > 0) ? wh / code_te.cell_h : 1);
          if (vmax < 0) vmax = 0;
          code_te.vscroll = (short)((long)msg[4] * vmax / 1000L);
          textedit_update_sliders(&code_te);
          textedit_redraw_all(&code_te);
        } else if (msg[3] == result_win) {
          slider_window(result_win, current_result, &result_top_line, msg[4]);
        }
        break;

      case WM_HSLID:
        if (msg[3] == code_win) {
          short wx, wy, ww, wh;
          short hmax;
          short max_w = 0;
          short i;
          wind_get(code_win, WF_WORKXYWH, &wx, &wy, &ww, &wh);
          for (i = 0; i < code_te.num_lines; i++)
            if (code_te.line_len[i] > max_w) max_w = code_te.line_len[i];
          hmax = max_w + 1 - ((code_te.cell_w > 0) ? ww / code_te.cell_w : 1);
          if (hmax < 0) hmax = 0;
          code_te.hscroll = (short)((long)msg[4] * hmax / 1000L);
          textedit_update_sliders(&code_te);
          textedit_redraw_all(&code_te);
        } else if (msg[3] == result_win) {
          hslider_window(result_win, current_result, &result_left_col, msg[4]);
        }
        break;

      case WM_SIZED:
      case WM_MOVED:
        wind_set(msg[3], WF_CURRXYWH, msg[4], msg[5], msg[6], msg[7]);
        if (msg[3] == code_win) {
          textedit_update_sliders(&code_te);
          textedit_redraw_all(&code_te);
        } else {
          do_redraw(msg[3]);
        }
        break;

      case WM_TOPPED:
        wind_set(msg[3], WF_TOP, 0, 0, 0, 0);
        break;

      case WM_CLOSED:
        quit_requested = 1;
        break;

      default:
        break;
    }
  }
}

static void redraw_all(void) {
  redraw_toolbar_window();
  redraw_code_window();
  redraw_result_window();
}

static void focus_result_window(void) {
  if (code_win >= 0) {
    wind_set(code_win, WF_TOP, 0, 0, 0, 0);
  }
  if (result_win >= 0) {
    wind_set(result_win, WF_TOP, 0, 0, 0, 0);
  }
}

static void present_result_window(void) {
  redraw_all();
  focus_result_window();
  redraw_all();
}

int main(void) {
  app_id = appl_init();
  if (app_id < 0) {
    return 1;
  }

  aes_handle = graf_handle(&sys_char_w, &sys_char_h, &sys_cell_w, &sys_cell_h);

  build_dialog();
  open_windows();
  event_loop();

  if (toolbar_win >= 0) {
    wind_close(toolbar_win);
    wind_delete(toolbar_win);
  }
  if (code_win >= 0) {
    wind_close(code_win);
    wind_delete(code_win);
  }
  if (result_win >= 0) {
    wind_close(result_win);
    wind_delete(result_win);
  }

  if (virt_vdi >= 0) {
    v_clsvwk(virt_vdi);
  }

  appl_exit();
  return 0;
}
