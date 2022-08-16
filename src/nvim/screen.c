// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// screen.c: Lower level code for displaying on the screen.
//           grid.c contains some other lower-level code.

// Output to the screen (console, terminal emulator or GUI window) is minimized
// by remembering what is already on the screen, and only updating the parts
// that changed.

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "nvim/buffer.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/eval.h"
#include "nvim/ex_getln.h"
#include "nvim/extmark.h"
#include "nvim/fileio.h"
#include "nvim/fold.h"
#include "nvim/garray.h"
#include "nvim/getchar.h"
#include "nvim/grid.h"
#include "nvim/highlight.h"
#include "nvim/highlight_group.h"
#include "nvim/menu.h"
#include "nvim/move.h"
#include "nvim/option.h"
#include "nvim/profile.h"
#include "nvim/regexp.h"
#include "nvim/screen.h"
#include "nvim/search.h"
#include "nvim/state.h"
#include "nvim/ui_compositor.h"
#include "nvim/undo.h"
#include "nvim/window.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "screen.c.generated.h"
#endif

/// Return true if the cursor line in window "wp" may be concealed, according
/// to the 'concealcursor' option.
bool conceal_cursor_line(const win_T *wp)
  FUNC_ATTR_NONNULL_ALL
{
  int c;

  if (*wp->w_p_cocu == NUL) {
    return false;
  }
  if (get_real_state() & MODE_VISUAL) {
    c = 'v';
  } else if (State & MODE_INSERT) {
    c = 'i';
  } else if (State & MODE_NORMAL) {
    c = 'n';
  } else if (State & MODE_CMDLINE) {
    c = 'c';
  } else {
    return false;
  }
  return vim_strchr((char *)wp->w_p_cocu, c) != NULL;
}

/// Whether cursorline is drawn in a special way
///
/// If true, both old and new cursorline will need to be redrawn when moving cursor within windows.
bool win_cursorline_standout(const win_T *wp)
  FUNC_ATTR_NONNULL_ALL
{
  return wp->w_p_cul || (wp->w_p_cole > 0 && !conceal_cursor_line(wp));
}

/// Returns width of the signcolumn that should be used for the whole window
///
/// @param wp window we want signcolumn width from
/// @return max width of signcolumn (cell unit)
///
/// @note Returns a constant for now but hopefully we can improve neovim so that
///       the returned value width adapts to the maximum number of marks to draw
///       for the window
/// TODO(teto)
int win_signcol_width(win_T *wp)
{
  // 2 is vim default value
  return 2;
}

/// Call grid_fill() with columns adjusted for 'rightleft' if needed.
/// Return the new offset.
static int win_fill_end(win_T *wp, int c1, int c2, int off, int width, int row, int endrow,
                        int attr)
{
  int nn = off + width;

  if (nn > wp->w_grid.cols) {
    nn = wp->w_grid.cols;
  }

  if (wp->w_p_rl) {
    grid_fill(&wp->w_grid, row, endrow, W_ENDCOL(wp) - nn, W_ENDCOL(wp) - off,
              c1, c2, attr);
  } else {
    grid_fill(&wp->w_grid, row, endrow, off, nn, c1, c2, attr);
  }

  return nn;
}

/// Clear lines near the end of the window and mark the unused lines with "c1".
/// Use "c2" as filler character.
/// When "draw_margin" is true, then draw the sign/fold/number columns.
void win_draw_end(win_T *wp, int c1, int c2, bool draw_margin, int row, int endrow, hlf_T hl)
{
  assert(hl >= 0 && hl < HLF_COUNT);
  int n = 0;

  if (draw_margin) {
    // draw the fold column
    int fdc = compute_foldcolumn(wp, 0);
    if (fdc > 0) {
      n = win_fill_end(wp, ' ', ' ', n, fdc, row, endrow,
                       win_hl_attr(wp, HLF_FC));
    }
    // draw the sign column
    int count = wp->w_scwidth;
    if (count > 0) {
      n = win_fill_end(wp, ' ', ' ', n, win_signcol_width(wp) * count, row,
                       endrow, win_hl_attr(wp, HLF_SC));
    }
    // draw the number column
    if ((wp->w_p_nu || wp->w_p_rnu) && vim_strchr(p_cpo, CPO_NUMCOL) == NULL) {
      n = win_fill_end(wp, ' ', ' ', n, number_width(wp) + 1, row, endrow,
                       win_hl_attr(wp, HLF_N));
    }
  }

  int attr = hl_combine_attr(win_bg_attr(wp), win_hl_attr(wp, (int)hl));

  if (wp->w_p_rl) {
    grid_fill(&wp->w_grid, row, endrow, wp->w_wincol, W_ENDCOL(wp) - 1 - n,
              c2, c2, attr);
    grid_fill(&wp->w_grid, row, endrow, W_ENDCOL(wp) - 1 - n, W_ENDCOL(wp) - n,
              c1, c2, attr);
  } else {
    grid_fill(&wp->w_grid, row, endrow, n, wp->w_grid.cols, c1, c2, attr);
  }

  set_empty_rows(wp, row);
}

/// Compute the width of the foldcolumn.  Based on 'foldcolumn' and how much
/// space is available for window "wp", minus "col".
int compute_foldcolumn(win_T *wp, int col)
{
  int fdc = win_fdccol_count(wp);
  int wmw = wp == curwin && p_wmw == 0 ? 1 : (int)p_wmw;
  int wwidth = wp->w_grid.cols;

  if (fdc > wwidth - (col + wmw)) {
    fdc = wwidth - (col + wmw);
  }
  return fdc;
}

/// Fills the foldcolumn at "p" for window "wp".
/// Only to be called when 'foldcolumn' > 0.
///
/// @param[out] p  Char array to write into
/// @param lnum    Absolute current line number
/// @param closed  Whether it is in 'foldcolumn' mode
///
/// Assume monocell characters
/// @return number of chars added to \param p
size_t fill_foldcolumn(char_u *p, win_T *wp, foldinfo_T foldinfo, linenr_T lnum)
{
  int i = 0;
  int level;
  int first_level;
  int fdc = compute_foldcolumn(wp, 0);    // available cell width
  size_t char_counter = 0;
  int symbol = 0;
  int len = 0;
  bool closed = foldinfo.fi_lines > 0;
  // Init to all spaces.
  memset(p, ' ', MAX_MCO * (size_t)fdc + 1);

  level = foldinfo.fi_level;

  // If the column is too narrow, we start at the lowest level that
  // fits and use numbers to indicate the depth.
  first_level = level - fdc - closed + 1;
  if (first_level < 1) {
    first_level = 1;
  }

  for (i = 0; i < MIN(fdc, level); i++) {
    if (foldinfo.fi_lnum == lnum
        && first_level + i >= foldinfo.fi_low_level) {
      symbol = wp->w_p_fcs_chars.foldopen;
    } else if (first_level == 1) {
      symbol = wp->w_p_fcs_chars.foldsep;
    } else if (first_level + i <= 9) {
      symbol = '0' + first_level + i;
    } else {
      symbol = '>';
    }

    len = utf_char2bytes(symbol, (char *)&p[char_counter]);
    char_counter += (size_t)len;
    if (first_level + i >= level) {
      i++;
      break;
    }
  }

  if (closed) {
    if (symbol != 0) {
      // rollback previous write
      char_counter -= (size_t)len;
      memset(&p[char_counter], ' ', (size_t)len);
    }
    len = utf_char2bytes(wp->w_p_fcs_chars.foldclosed, (char *)&p[char_counter]);
    char_counter += (size_t)len;
  }

  return MAX(char_counter + (size_t)(fdc - i), (size_t)fdc);
}

/// Mirror text "str" for right-left displaying.
/// Only works for single-byte characters (e.g., numbers).
void rl_mirror(char_u *str)
{
  char_u *p1, *p2;
  char_u t;

  for (p1 = str, p2 = str + STRLEN(str) - 1; p1 < p2; p1++, p2--) {
    t = *p1;
    *p1 = *p2;
    *p2 = t;
  }
}

/// Get the length of an item as it will be shown in the status line.
static int status_match_len(expand_T *xp, char_u *s)
{
  int len = 0;

  int emenu = (xp->xp_context == EXPAND_MENUS
               || xp->xp_context == EXPAND_MENUNAMES);

  // Check for menu separators - replace with '|'.
  if (emenu && menu_is_separator((char *)s)) {
    return 1;
  }

  while (*s != NUL) {
    s += skip_status_match_char(xp, s);
    len += ptr2cells((char *)s);
    MB_PTR_ADV(s);
  }

  return len;
}

/// Redraw all status lines at the bottom of frame "frp".
void win_redraw_last_status(const frame_T *frp)
  FUNC_ATTR_NONNULL_ARG(1)
{
  if (frp->fr_layout == FR_LEAF) {
    frp->fr_win->w_redr_status = true;
  } else if (frp->fr_layout == FR_ROW) {
    FOR_ALL_FRAMES(frp, frp->fr_child) {
      win_redraw_last_status(frp);
    }
  } else {
    assert(frp->fr_layout == FR_COL);
    frp = frp->fr_child;
    while (frp->fr_next != NULL) {
      frp = frp->fr_next;
    }
    win_redraw_last_status(frp);
  }
}

/// Return the number of characters that should be skipped in a status match.
/// These are backslashes used for escaping.  Do show backslashes in help tags.
static int skip_status_match_char(expand_T *xp, char_u *s)
{
  if ((rem_backslash(s) && xp->xp_context != EXPAND_HELP)
      || ((xp->xp_context == EXPAND_MENUS
           || xp->xp_context == EXPAND_MENUNAMES)
          && (s[0] == '\t'
              || (s[0] == '\\' && s[1] != NUL)))) {
#ifndef BACKSLASH_IN_FILENAME
    if (xp->xp_shell && csh_like_shell() && s[1] == '\\' && s[2] == '!') {
      return 2;
    }
#endif
    return 1;
  }
  return 0;
}

/// Show wildchar matches in the status line.
/// Show at least the "match" item.
/// We start at item 'first_match' in the list and show all matches that fit.
///
/// If inversion is possible we use it. Else '=' characters are used.
///
/// @param matches  list of matches
void win_redr_status_matches(expand_T *xp, int num_matches, char **matches, int match, int showtail)
{
#define L_MATCH(m) (showtail ? sm_gettail(matches[m], false) : matches[m])
  int row;
  char_u *buf;
  int len;
  int clen;                     // length in screen cells
  int fillchar;
  int attr;
  int i;
  bool highlight = true;
  char_u *selstart = NULL;
  int selstart_col = 0;
  char_u *selend = NULL;
  static int first_match = 0;
  bool add_left = false;
  char_u *s;
  int emenu;
  int l;

  if (matches == NULL) {        // interrupted completion?
    return;
  }

  buf = xmalloc((size_t)Columns * MB_MAXBYTES + 1);

  if (match == -1) {    // don't show match but original text
    match = 0;
    highlight = false;
  }
  // count 1 for the ending ">"
  clen = status_match_len(xp, (char_u *)L_MATCH(match)) + 3;
  if (match == 0) {
    first_match = 0;
  } else if (match < first_match) {
    // jumping left, as far as we can go
    first_match = match;
    add_left = true;
  } else {
    // check if match fits on the screen
    for (i = first_match; i < match; i++) {
      clen += status_match_len(xp, (char_u *)L_MATCH(i)) + 2;
    }
    if (first_match > 0) {
      clen += 2;
    }
    // jumping right, put match at the left
    if ((long)clen > Columns) {
      first_match = match;
      // if showing the last match, we can add some on the left
      clen = 2;
      for (i = match; i < num_matches; i++) {
        clen += status_match_len(xp, (char_u *)L_MATCH(i)) + 2;
        if ((long)clen >= Columns) {
          break;
        }
      }
      if (i == num_matches) {
        add_left = true;
      }
    }
  }
  if (add_left) {
    while (first_match > 0) {
      clen += status_match_len(xp, (char_u *)L_MATCH(first_match - 1)) + 2;
      if ((long)clen >= Columns) {
        break;
      }
      first_match--;
    }
  }

  fillchar = fillchar_status(&attr, curwin);

  if (first_match == 0) {
    *buf = NUL;
    len = 0;
  } else {
    STRCPY(buf, "< ");
    len = 2;
  }
  clen = len;

  i = first_match;
  while (clen + status_match_len(xp, (char_u *)L_MATCH(i)) + 2 < Columns) {
    if (i == match) {
      selstart = buf + len;
      selstart_col = clen;
    }

    s = (char_u *)L_MATCH(i);
    // Check for menu separators - replace with '|'
    emenu = (xp->xp_context == EXPAND_MENUS
             || xp->xp_context == EXPAND_MENUNAMES);
    if (emenu && menu_is_separator((char *)s)) {
      STRCPY(buf + len, transchar('|'));
      l = (int)STRLEN(buf + len);
      len += l;
      clen += l;
    } else {
      for (; *s != NUL; s++) {
        s += skip_status_match_char(xp, s);
        clen += ptr2cells((char *)s);
        if ((l = utfc_ptr2len((char *)s)) > 1) {
          STRNCPY(buf + len, s, l);  // NOLINT(runtime/printf)
          s += l - 1;
          len += l;
        } else {
          STRCPY(buf + len, transchar_byte(*s));
          len += (int)STRLEN(buf + len);
        }
      }
    }
    if (i == match) {
      selend = buf + len;
    }

    *(buf + len++) = ' ';
    *(buf + len++) = ' ';
    clen += 2;
    if (++i == num_matches) {
      break;
    }
  }

  if (i != num_matches) {
    *(buf + len++) = '>';
    clen++;
  }

  buf[len] = NUL;

  row = cmdline_row - 1;
  if (row >= 0) {
    if (wild_menu_showing == 0 || wild_menu_showing == WM_LIST) {
      if (msg_scrolled > 0) {
        // Put the wildmenu just above the command line.  If there is
        // no room, scroll the screen one line up.
        if (cmdline_row == Rows - 1) {
          msg_scroll_up(false);
          msg_scrolled++;
        } else {
          cmdline_row++;
          row++;
        }
        wild_menu_showing = WM_SCROLLED;
      } else {
        // Create status line if needed by setting 'laststatus' to 2.
        // Set 'winminheight' to zero to avoid that the window is
        // resized.
        if (lastwin->w_status_height == 0 && global_stl_height() == 0) {
          save_p_ls = (int)p_ls;
          save_p_wmh = (int)p_wmh;
          p_ls = 2;
          p_wmh = 0;
          last_status(false);
        }
        wild_menu_showing = WM_SHOWN;
      }
    }

    // Tricky: wildmenu can be drawn either over a status line, or at empty
    // scrolled space in the message output
    ScreenGrid *grid = (wild_menu_showing == WM_SCROLLED)
                        ? &msg_grid_adj : &default_grid;

    grid_puts(grid, buf, row, 0, attr);
    if (selstart != NULL && highlight) {
      *selend = NUL;
      grid_puts(grid, selstart, row, selstart_col, HL_ATTR(HLF_WM));
    }

    grid_fill(grid, row, row + 1, clen, Columns,
              fillchar, fillchar, attr);
  }

  win_redraw_last_status(topframe);
  xfree(buf);
}

/// Only call if (wp->w_vsep_width != 0).
///
/// @return  true if the status line of window "wp" is connected to the status
/// line of the window right of it.  If not, then it's a vertical separator.
bool stl_connected(win_T *wp)
{
  frame_T *fr;

  fr = wp->w_frame;
  while (fr->fr_parent != NULL) {
    if (fr->fr_parent->fr_layout == FR_COL) {
      if (fr->fr_next != NULL) {
        break;
      }
    } else {
      if (fr->fr_next != NULL) {
        return true;
      }
    }
    fr = fr->fr_parent;
  }
  return false;
}

/// Get the value to show for the language mappings, active 'keymap'.
///
/// @param fmt  format string containing one %s item
/// @param buf  buffer for the result
/// @param len  length of buffer
bool get_keymap_str(win_T *wp, char *fmt, char *buf, int len)
{
  char *p;

  if (wp->w_buffer->b_p_iminsert != B_IMODE_LMAP) {
    return false;
  }

  {
    buf_T *old_curbuf = curbuf;
    win_T *old_curwin = curwin;
    char *s;

    curbuf = wp->w_buffer;
    curwin = wp;
    STRCPY(buf, "b:keymap_name");       // must be writable
    emsg_skip++;
    s = p = eval_to_string(buf, NULL, false);
    emsg_skip--;
    curbuf = old_curbuf;
    curwin = old_curwin;
    if (p == NULL || *p == NUL) {
      if (wp->w_buffer->b_kmap_state & KEYMAP_LOADED) {
        p = (char *)wp->w_buffer->b_p_keymap;
      } else {
        p = "lang";
      }
    }
    if (vim_snprintf(buf, (size_t)len, fmt, p) > len - 1) {
      buf[0] = NUL;
    }
    xfree(s);
  }
  return buf[0] != NUL;
}

/// Redraw the status line, window bar or ruler of window "wp".
/// When "wp" is NULL redraw the tab pages line from 'tabline'.
void win_redr_custom(win_T *wp, bool draw_winbar, bool draw_ruler)
{
  static bool entered = false;
  int attr;
  int curattr;
  int row;
  int col = 0;
  int maxwidth;
  int width;
  int n;
  int len;
  int fillchar;
  char buf[MAXPATHL];
  char_u *stl;
  char *p;
  stl_hlrec_t *hltab;
  StlClickRecord *tabtab;
  int use_sandbox = false;
  win_T *ewp;
  int p_crb_save;
  bool is_stl_global = global_stl_height() > 0;

  ScreenGrid *grid = &default_grid;

  // There is a tiny chance that this gets called recursively: When
  // redrawing a status line triggers redrawing the ruler or tabline.
  // Avoid trouble by not allowing recursion.
  if (entered) {
    return;
  }
  entered = true;

  // setup environment for the task at hand
  if (wp == NULL) {
    // Use 'tabline'.  Always at the first line of the screen.
    stl = p_tal;
    row = 0;
    fillchar = ' ';
    attr = HL_ATTR(HLF_TPF);
    maxwidth = Columns;
    use_sandbox = was_set_insecurely(wp, "tabline", 0);
  } else if (draw_winbar) {
    stl = (char_u *)((*wp->w_p_wbr != NUL) ? wp->w_p_wbr : p_wbr);
    row = -1;  // row zero is first row of text
    col = 0;
    grid = &wp->w_grid;
    grid_adjust(&grid, &row, &col);

    if (row < 0) {
      return;
    }

    fillchar = wp->w_p_fcs_chars.wbr;
    attr = (wp == curwin) ? win_hl_attr(wp, HLF_WBR) : win_hl_attr(wp, HLF_WBRNC);
    maxwidth = wp->w_width_inner;
    use_sandbox = was_set_insecurely(wp, "winbar", 0);

    stl_clear_click_defs(wp->w_winbar_click_defs, (long)wp->w_winbar_click_defs_size);
    // Allocate / resize the click definitions array for winbar if needed.
    if (wp->w_winbar_height && wp->w_winbar_click_defs_size < (size_t)maxwidth) {
      xfree(wp->w_winbar_click_defs);
      wp->w_winbar_click_defs_size = (size_t)maxwidth;
      wp->w_winbar_click_defs = xcalloc(wp->w_winbar_click_defs_size, sizeof(StlClickRecord));
    }
  } else {
    row = is_stl_global ? (Rows - (int)p_ch - 1) : W_ENDROW(wp);
    fillchar = fillchar_status(&attr, wp);
    maxwidth = is_stl_global ? Columns : wp->w_width;

    stl_clear_click_defs(wp->w_status_click_defs, (long)wp->w_status_click_defs_size);
    // Allocate / resize the click definitions array for statusline if needed.
    if (wp->w_status_click_defs_size < (size_t)maxwidth) {
      xfree(wp->w_status_click_defs);
      wp->w_status_click_defs_size = (size_t)maxwidth;
      wp->w_status_click_defs = xcalloc(wp->w_status_click_defs_size, sizeof(StlClickRecord));
    }

    if (draw_ruler) {
      stl = p_ruf;
      // advance past any leading group spec - implicit in ru_col
      if (*stl == '%') {
        if (*++stl == '-') {
          stl++;
        }
        if (atoi((char *)stl)) {
          while (ascii_isdigit(*stl)) {
            stl++;
          }
        }
        if (*stl++ != '(') {
          stl = p_ruf;
        }
      }
      col = ru_col - (Columns - maxwidth);
      if (col < (maxwidth + 1) / 2) {
        col = (maxwidth + 1) / 2;
      }
      maxwidth = maxwidth - col;
      if (!wp->w_status_height && !is_stl_global) {
        grid = &msg_grid_adj;
        row = Rows - 1;
        maxwidth--;  // writing in last column may cause scrolling
        fillchar = ' ';
        attr = HL_ATTR(HLF_MSG);
      }

      use_sandbox = was_set_insecurely(wp, "rulerformat", 0);
    } else {
      if (*wp->w_p_stl != NUL) {
        stl = wp->w_p_stl;
      } else {
        stl = p_stl;
      }
      use_sandbox = was_set_insecurely(wp, "statusline", *wp->w_p_stl == NUL ? 0 : OPT_LOCAL);
    }

    col += is_stl_global ? 0 : wp->w_wincol;
  }

  if (maxwidth <= 0) {
    goto theend;
  }

  // Temporarily reset 'cursorbind', we don't want a side effect from moving
  // the cursor away and back.
  ewp = wp == NULL ? curwin : wp;
  p_crb_save = ewp->w_p_crb;
  ewp->w_p_crb = false;

  // Make a copy, because the statusline may include a function call that
  // might change the option value and free the memory.
  stl = vim_strsave(stl);
  width =
    build_stl_str_hl(ewp, buf, sizeof(buf), (char *)stl, use_sandbox,
                     fillchar, maxwidth, &hltab, &tabtab);
  xfree(stl);
  ewp->w_p_crb = p_crb_save;

  // Make all characters printable.
  p = transstr(buf, true);
  len = (int)STRLCPY(buf, p, sizeof(buf));
  len = (size_t)len < sizeof(buf) ? len : (int)sizeof(buf) - 1;
  xfree(p);

  // fill up with "fillchar"
  while (width < maxwidth && len < (int)sizeof(buf) - 1) {
    len += utf_char2bytes(fillchar, buf + len);
    width++;
  }
  buf[len] = NUL;

  // Draw each snippet with the specified highlighting.
  grid_puts_line_start(grid, row);

  curattr = attr;
  p = buf;
  for (n = 0; hltab[n].start != NULL; n++) {
    int textlen = (int)(hltab[n].start - p);
    grid_puts_len(grid, (char_u *)p, textlen, row, col, curattr);
    col += vim_strnsize((char_u *)p, textlen);
    p = hltab[n].start;

    if (hltab[n].userhl == 0) {
      curattr = attr;
    } else if (hltab[n].userhl < 0) {
      curattr = syn_id2attr(-hltab[n].userhl);
    } else if (wp != NULL && wp != curwin && wp->w_status_height != 0) {
      curattr = highlight_stlnc[hltab[n].userhl - 1];
    } else {
      curattr = highlight_user[hltab[n].userhl - 1];
    }
  }
  // Make sure to use an empty string instead of p, if p is beyond buf + len.
  grid_puts(grid, p >= buf + len ? (char_u *)"" : (char_u *)p, row, col,
            curattr);

  grid_puts_line_flush(false);

  // Fill the tab_page_click_defs, w_status_click_defs or w_winbar_click_defs array for clicking
  // in the tab page line, status line or window bar
  StlClickDefinition *click_defs = (wp == NULL) ? tab_page_click_defs
                                                : draw_winbar ? wp->w_winbar_click_defs
                                                              : wp->w_status_click_defs;

  if (click_defs == NULL) {
    goto theend;
  }

  col = 0;
  len = 0;
  p = buf;
  StlClickDefinition cur_click_def = {
    .type = kStlClickDisabled,
  };
  for (n = 0; tabtab[n].start != NULL; n++) {
    len += vim_strnsize((char_u *)p, (int)(tabtab[n].start - p));
    while (col < len) {
      click_defs[col++] = cur_click_def;
    }
    p = (char *)tabtab[n].start;
    cur_click_def = tabtab[n].def;
    if ((wp != NULL) && !(cur_click_def.type == kStlClickDisabled
                          || cur_click_def.type == kStlClickFuncRun)) {
      // window bar and status line only support click functions
      cur_click_def.type = kStlClickDisabled;
    }
  }
  while (col < maxwidth) {
    click_defs[col++] = cur_click_def;
  }

theend:
  entered = false;
}

/// Prepare for 'hlsearch' highlighting.
void start_search_hl(void)
{
  if (p_hls && !no_hlsearch) {
    end_search_hl();  // just in case it wasn't called before
    last_pat_prog(&screen_search_hl.rm);
    // Set the time limit to 'redrawtime'.
    screen_search_hl.tm = profile_setlimit(p_rdt);
  }
}

/// Clean up for 'hlsearch' highlighting.
void end_search_hl(void)
{
  if (screen_search_hl.rm.regprog != NULL) {
    vim_regfree(screen_search_hl.rm.regprog);
    screen_search_hl.rm.regprog = NULL;
  }
}

/// Check if there should be a delay.  Used before clearing or redrawing the
/// screen or the command line.
void check_for_delay(bool check_msg_scroll)
{
  if ((emsg_on_display || (check_msg_scroll && msg_scroll))
      && !did_wait_return
      && emsg_silent == 0) {
    ui_flush();
    os_delay(1006L, true);
    emsg_on_display = false;
    if (check_msg_scroll) {
      msg_scroll = false;
    }
  }
}

/// Clear status line, window bar or tab page line click definition table
///
/// @param[out]  tpcd  Table to clear.
/// @param[in]  tpcd_size  Size of the table.
void stl_clear_click_defs(StlClickDefinition *const click_defs, const long click_defs_size)
{
  if (click_defs != NULL) {
    for (long i = 0; i < click_defs_size; i++) {
      if (i == 0 || click_defs[i].func != click_defs[i - 1].func) {
        xfree(click_defs[i].func);
      }
    }
    memset(click_defs, 0, (size_t)click_defs_size * sizeof(click_defs[0]));
  }
}

/// Set cursor to its position in the current window.
void setcursor(void)
{
  setcursor_mayforce(false);
}

/// Set cursor to its position in the current window.
/// @param force  when true, also when not redrawing.
void setcursor_mayforce(bool force)
{
  if (force || redrawing()) {
    validate_cursor();

    ScreenGrid *grid = &curwin->w_grid;
    int row = curwin->w_wrow;
    int col = curwin->w_wcol;
    if (curwin->w_p_rl) {
      // With 'rightleft' set and the cursor on a double-wide character,
      // position it on the leftmost column.
      col = curwin->w_width_inner - curwin->w_wcol
            - ((utf_ptr2cells((char *)get_cursor_pos_ptr()) == 2
                && vim_isprintc(gchar_cursor())) ? 2 : 1);
    }

    grid_adjust(&grid, &row, &col);
    ui_grid_cursor_goto(grid->handle, row, col);
  }
}

/// Scroll `line_count` lines at 'row' in window 'wp'.
///
/// Positive `line_count` means scrolling down, so that more space is available
/// at 'row'. Negative `line_count` implies deleting lines at `row`.
void win_scroll_lines(win_T *wp, int row, int line_count)
{
  if (!redrawing() || line_count == 0) {
    return;
  }

  // No lines are being moved, just draw over the entire area
  if (row + abs(line_count) >= wp->w_grid.rows) {
    return;
  }

  if (line_count < 0) {
    grid_del_lines(&wp->w_grid, row, -line_count,
                   wp->w_grid.rows, 0, wp->w_grid.cols);
  } else {
    grid_ins_lines(&wp->w_grid, row, line_count,
                   wp->w_grid.rows, 0, wp->w_grid.cols);
  }
}

/// @return true when postponing displaying the mode message: when not redrawing
/// or inside a mapping.
bool skip_showmode(void)
{
  // Call char_avail() only when we are going to show something, because it
  // takes a bit of time.  redrawing() may also call char_avail().
  if (global_busy || msg_silent != 0 || !redrawing() || (char_avail() && !KeyTyped)) {
    redraw_mode = true;  // show mode later
    return true;
  }
  return false;
}

/// Show the current mode and ruler.
///
/// If clear_cmdline is true, clear the rest of the cmdline.
/// If clear_cmdline is false there may be a message there that needs to be
/// cleared only if a mode is shown.
/// If redraw_mode is true show or clear the mode.
/// @return the length of the message (0 if no message).
int showmode(void)
{
  bool need_clear;
  int length = 0;
  int do_mode;
  int attr;
  int sub_attr;

  if (ui_has(kUIMessages) && clear_cmdline) {
    msg_ext_clear(true);
  }

  // don't make non-flushed message part of the showmode
  msg_ext_ui_flush();

  msg_grid_validate();

  do_mode = ((p_smd && msg_silent == 0)
             && ((State & MODE_TERMINAL)
                 || (State & MODE_INSERT)
                 || restart_edit != NUL
                 || VIsual_active));
  if (do_mode || reg_recording != 0) {
    if (skip_showmode()) {
      return 0;  // show mode later
    }

    bool nwr_save = need_wait_return;

    // wait a bit before overwriting an important message
    check_for_delay(false);

    // if the cmdline is more than one line high, erase top lines
    need_clear = clear_cmdline;
    if (clear_cmdline && cmdline_row < Rows - 1) {
      msg_clr_cmdline();  // will reset clear_cmdline
    }

    // Position on the last line in the window, column 0
    msg_pos_mode();
    attr = HL_ATTR(HLF_CM);                     // Highlight mode

    // When the screen is too narrow to show the entire mode message,
    // avoid scrolling and truncate instead.
    msg_no_more = true;
    int save_lines_left = lines_left;
    lines_left = 0;

    if (do_mode) {
      msg_puts_attr("--", attr);
      // CTRL-X in Insert mode
      if (edit_submode != NULL && !shortmess(SHM_COMPLETIONMENU)) {
        // These messages can get long, avoid a wrap in a narrow window.
        // Prefer showing edit_submode_extra. With external messages there
        // is no imposed limit.
        if (ui_has(kUIMessages)) {
          length = INT_MAX;
        } else {
          length = (Rows - msg_row) * Columns - 3;
        }
        if (edit_submode_extra != NULL) {
          length -= vim_strsize((char *)edit_submode_extra);
        }
        if (length > 0) {
          if (edit_submode_pre != NULL) {
            length -= vim_strsize((char *)edit_submode_pre);
          }
          if (length - vim_strsize((char *)edit_submode) > 0) {
            if (edit_submode_pre != NULL) {
              msg_puts_attr((const char *)edit_submode_pre, attr);
            }
            msg_puts_attr((const char *)edit_submode, attr);
          }
          if (edit_submode_extra != NULL) {
            msg_puts_attr(" ", attr);  // Add a space in between.
            if ((int)edit_submode_highl < HLF_COUNT) {
              sub_attr = win_hl_attr(curwin, (int)edit_submode_highl);
            } else {
              sub_attr = attr;
            }
            msg_puts_attr((const char *)edit_submode_extra, sub_attr);
          }
        }
      } else {
        if (State & MODE_TERMINAL) {
          msg_puts_attr(_(" TERMINAL"), attr);
        } else if (State & VREPLACE_FLAG) {
          msg_puts_attr(_(" VREPLACE"), attr);
        } else if (State & REPLACE_FLAG) {
          msg_puts_attr(_(" REPLACE"), attr);
        } else if (State & MODE_INSERT) {
          if (p_ri) {
            msg_puts_attr(_(" REVERSE"), attr);
          }
          msg_puts_attr(_(" INSERT"), attr);
        } else if (restart_edit == 'I' || restart_edit == 'i'
                   || restart_edit == 'a' || restart_edit == 'A') {
          if (curbuf->terminal) {
            msg_puts_attr(_(" (terminal)"), attr);
          } else {
            msg_puts_attr(_(" (insert)"), attr);
          }
        } else if (restart_edit == 'R') {
          msg_puts_attr(_(" (replace)"), attr);
        } else if (restart_edit == 'V') {
          msg_puts_attr(_(" (vreplace)"), attr);
        }
        if (p_hkmap) {
          msg_puts_attr(_(" Hebrew"), attr);
        }
        if (State & MODE_LANGMAP) {
          if (curwin->w_p_arab) {
            msg_puts_attr(_(" Arabic"), attr);
          } else if (get_keymap_str(curwin, " (%s)",
                                    (char *)NameBuff, MAXPATHL)) {
            msg_puts_attr((char *)NameBuff, attr);
          }
        }
        if ((State & MODE_INSERT) && p_paste) {
          msg_puts_attr(_(" (paste)"), attr);
        }

        if (VIsual_active) {
          char *p;

          // Don't concatenate separate words to avoid translation
          // problems.
          switch ((VIsual_select ? 4 : 0)
                  + (VIsual_mode == Ctrl_V) * 2
                  + (VIsual_mode == 'V')) {
          case 0:
            p = N_(" VISUAL"); break;
          case 1:
            p = N_(" VISUAL LINE"); break;
          case 2:
            p = N_(" VISUAL BLOCK"); break;
          case 4:
            p = N_(" SELECT"); break;
          case 5:
            p = N_(" SELECT LINE"); break;
          default:
            p = N_(" SELECT BLOCK"); break;
          }
          msg_puts_attr(_(p), attr);
        }
        msg_puts_attr(" --", attr);
      }

      need_clear = true;
    }
    if (reg_recording != 0
        && edit_submode == NULL             // otherwise it gets too long
        ) {
      recording_mode(attr);
      need_clear = true;
    }

    mode_displayed = true;
    if (need_clear || clear_cmdline || redraw_mode) {
      msg_clr_eos();
    }
    msg_didout = false;                 // overwrite this message
    length = msg_col;
    msg_col = 0;
    msg_no_more = false;
    lines_left = save_lines_left;
    need_wait_return = nwr_save;        // never ask for hit-return for this
  } else if (clear_cmdline && msg_silent == 0) {
    // Clear the whole command line.  Will reset "clear_cmdline".
    msg_clr_cmdline();
  } else if (redraw_mode) {
    msg_pos_mode();
    msg_clr_eos();
  }

  // NB: also handles clearing the showmode if it was empty or disabled
  msg_ext_flush_showmode();

  // In Visual mode the size of the selected area must be redrawn.
  if (VIsual_active) {
    clear_showcmd();
  }

  // If the last window has no status line and global statusline is disabled,
  // the ruler is after the mode message and must be redrawn
  win_T *last = lastwin_nofloating();
  if (redrawing() && last->w_status_height == 0 && global_stl_height() == 0) {
    win_redr_ruler(last, true);
  }
  redraw_cmdline = false;
  redraw_mode = false;
  clear_cmdline = false;

  return length;
}

/// Position for a mode message.
static void msg_pos_mode(void)
{
  msg_col = 0;
  msg_row = Rows - 1;
}

/// Delete mode message.  Used when ESC is typed which is expected to end
/// Insert mode (but Insert mode didn't end yet!).
/// Caller should check "mode_displayed".
void unshowmode(bool force)
{
  // Don't delete it right now, when not redrawing or inside a mapping.
  if (!redrawing() || (!force && char_avail() && !KeyTyped)) {
    redraw_cmdline = true;  // delete mode later
  } else {
    clearmode();
  }
}

// Clear the mode message.
void clearmode(void)
{
  const int save_msg_row = msg_row;
  const int save_msg_col = msg_col;

  msg_ext_ui_flush();
  msg_pos_mode();
  if (reg_recording != 0) {
    recording_mode(HL_ATTR(HLF_CM));
  }
  msg_clr_eos();
  msg_ext_flush_showmode();

  msg_col = save_msg_col;
  msg_row = save_msg_row;
}

static void recording_mode(int attr)
{
  msg_puts_attr(_("recording"), attr);
  if (!shortmess(SHM_RECORDING)) {
    char s[4];
    snprintf(s, ARRAY_SIZE(s), " @%c", reg_recording);
    msg_puts_attr(s, attr);
  }
}

/// Draw the tab pages line at the top of the Vim window.
void draw_tabline(void)
{
  int tabcount = 0;
  int tabwidth = 0;
  int col = 0;
  int scol = 0;
  int attr;
  win_T *wp;
  win_T *cwp;
  int wincount;
  int modified;
  int c;
  int len;
  int attr_nosel = HL_ATTR(HLF_TP);
  int attr_fill = HL_ATTR(HLF_TPF);
  char_u *p;
  int room;
  int use_sep_chars = (t_colors < 8);

  if (default_grid.chars == NULL) {
    return;
  }
  redraw_tabline = false;

  if (ui_has(kUITabline)) {
    ui_ext_tabline_update();
    return;
  }

  if (tabline_height() < 1) {
    return;
  }

  // Init TabPageIdxs[] to zero: Clicking outside of tabs has no effect.
  assert(Columns == tab_page_click_defs_size);
  stl_clear_click_defs(tab_page_click_defs, tab_page_click_defs_size);

  // Use the 'tabline' option if it's set.
  if (*p_tal != NUL) {
    int saved_did_emsg = did_emsg;

    // Check for an error.  If there is one we would loop in redrawing the
    // screen.  Avoid that by making 'tabline' empty.
    did_emsg = false;
    win_redr_custom(NULL, false, false);
    if (did_emsg) {
      set_string_option_direct("tabline", -1, "", OPT_FREE, SID_ERROR);
    }
    did_emsg |= saved_did_emsg;
  } else {
    FOR_ALL_TABS(tp) {
      tabcount++;
    }

    if (tabcount > 0) {
      tabwidth = (Columns - 1 + tabcount / 2) / tabcount;
    }

    if (tabwidth < 6) {
      tabwidth = 6;
    }

    attr = attr_nosel;
    tabcount = 0;

    FOR_ALL_TABS(tp) {
      if (col >= Columns - 4) {
        break;
      }

      scol = col;

      if (tp == curtab) {
        cwp = curwin;
        wp = firstwin;
      } else {
        cwp = tp->tp_curwin;
        wp = tp->tp_firstwin;
      }

      if (tp->tp_topframe == topframe) {
        attr = win_hl_attr(cwp, HLF_TPS);
      }
      if (use_sep_chars && col > 0) {
        grid_putchar(&default_grid, '|', 0, col++, attr);
      }

      if (tp->tp_topframe != topframe) {
        attr = win_hl_attr(cwp, HLF_TP);
      }

      grid_putchar(&default_grid, ' ', 0, col++, attr);

      modified = false;

      for (wincount = 0; wp != NULL; wp = wp->w_next, ++wincount) {
        if (bufIsChanged(wp->w_buffer)) {
          modified = true;
        }
      }

      if (modified || wincount > 1) {
        if (wincount > 1) {
          vim_snprintf((char *)NameBuff, MAXPATHL, "%d", wincount);
          len = (int)STRLEN(NameBuff);
          if (col + len >= Columns - 3) {
            break;
          }
          grid_puts_len(&default_grid, NameBuff, len, 0, col,
                        hl_combine_attr(attr, win_hl_attr(cwp, HLF_T)));
          col += len;
        }
        if (modified) {
          grid_puts_len(&default_grid, (char_u *)"+", 1, 0, col++, attr);
        }
        grid_putchar(&default_grid, ' ', 0, col++, attr);
      }

      room = scol - col + tabwidth - 1;
      if (room > 0) {
        // Get buffer name in NameBuff[]
        get_trans_bufname(cwp->w_buffer);
        shorten_dir(NameBuff);
        len = vim_strsize((char *)NameBuff);
        p = NameBuff;
        while (len > room) {
          len -= ptr2cells((char *)p);
          MB_PTR_ADV(p);
        }
        if (len > Columns - col - 1) {
          len = Columns - col - 1;
        }

        grid_puts_len(&default_grid, p, (int)STRLEN(p), 0, col, attr);
        col += len;
      }
      grid_putchar(&default_grid, ' ', 0, col++, attr);

      // Store the tab page number in tab_page_click_defs[], so that
      // jump_to_mouse() knows where each one is.
      tabcount++;
      while (scol < col) {
        tab_page_click_defs[scol++] = (StlClickDefinition) {
          .type = kStlClickTabSwitch,
          .tabnr = tabcount,
          .func = NULL,
        };
      }
    }

    if (use_sep_chars) {
      c = '_';
    } else {
      c = ' ';
    }
    grid_fill(&default_grid, 0, 1, col, Columns, c, c, attr_fill);

    // Put an "X" for closing the current tab if there are several.
    if (first_tabpage->tp_next != NULL) {
      grid_putchar(&default_grid, 'X', 0, Columns - 1, attr_nosel);
      tab_page_click_defs[Columns - 1] = (StlClickDefinition) {
        .type = kStlClickTabClose,
        .tabnr = 999,
        .func = NULL,
      };
    }
  }

  // Reset the flag here again, in case evaluating 'tabline' causes it to be
  // set.
  redraw_tabline = false;
}

static void ui_ext_tabline_update(void)
{
  Arena arena = ARENA_EMPTY;
  arena_start(&arena, &ui_ext_fixblk);

  size_t n_tabs = 0;
  FOR_ALL_TABS(tp) {
    n_tabs++;
  }

  Array tabs = arena_array(&arena, n_tabs);
  FOR_ALL_TABS(tp) {
    Dictionary tab_info = arena_dict(&arena, 2);
    PUT_C(tab_info, "tab", TABPAGE_OBJ(tp->handle));

    win_T *cwp = (tp == curtab) ? curwin : tp->tp_curwin;
    get_trans_bufname(cwp->w_buffer);
    PUT_C(tab_info, "name", STRING_OBJ(arena_string(&arena, cstr_as_string((char *)NameBuff))));

    ADD_C(tabs, DICTIONARY_OBJ(tab_info));
  }

  size_t n_buffers = 0;
  FOR_ALL_BUFFERS(buf) {
    n_buffers += buf->b_p_bl ? 1 : 0;
  }

  Array buffers = arena_array(&arena, n_buffers);
  FOR_ALL_BUFFERS(buf) {
    // Do not include unlisted buffers
    if (!buf->b_p_bl) {
      continue;
    }

    Dictionary buffer_info = arena_dict(&arena, 2);
    PUT_C(buffer_info, "buffer", BUFFER_OBJ(buf->handle));

    get_trans_bufname(buf);
    PUT_C(buffer_info, "name", STRING_OBJ(arena_string(&arena, cstr_as_string((char *)NameBuff))));

    ADD_C(buffers, DICTIONARY_OBJ(buffer_info));
  }

  ui_call_tabline_update(curtab->handle, tabs, curbuf->handle, buffers);
  arena_mem_free(arena_finish(&arena), &ui_ext_fixblk);
}

void get_trans_bufname(buf_T *buf)
{
  if (buf_spname(buf) != NULL) {
    STRLCPY(NameBuff, buf_spname(buf), MAXPATHL);
  } else {
    home_replace(buf, buf->b_fname, (char *)NameBuff, MAXPATHL, true);
  }
  trans_characters((char *)NameBuff, MAXPATHL);
}

/// Get the character to use in a status line.  Get its attributes in "*attr".
int fillchar_status(int *attr, win_T *wp)
{
  int fill;
  bool is_curwin = (wp == curwin);
  if (is_curwin) {
    *attr = win_hl_attr(wp, HLF_S);
    fill = wp->w_p_fcs_chars.stl;
  } else {
    *attr = win_hl_attr(wp, HLF_SNC);
    fill = wp->w_p_fcs_chars.stlnc;
  }
  // Use fill when there is highlighting, and highlighting of current
  // window differs, or the fillchars differ, or this is not the
  // current window
  if (*attr != 0 && ((win_hl_attr(wp, HLF_S) != win_hl_attr(wp, HLF_SNC)
                      || !is_curwin || ONE_WINDOW)
                     || (wp->w_p_fcs_chars.stl != wp->w_p_fcs_chars.stlnc))) {
    return fill;
  }
  if (is_curwin) {
    return '^';
  }
  return '=';
}

/// Get the character to use in a separator between vertically split windows.
/// Get its attributes in "*attr".
int fillchar_vsep(win_T *wp, int *attr)
{
  *attr = win_hl_attr(wp, HLF_C);
  return wp->w_p_fcs_chars.vert;
}

/// Get the character to use in a separator between horizontally split windows.
/// Get its attributes in "*attr".
int fillchar_hsep(win_T *wp, int *attr)
{
  *attr = win_hl_attr(wp, HLF_C);
  return wp->w_p_fcs_chars.horiz;
}

/// Return true if redrawing should currently be done.
bool redrawing(void)
{
  return !RedrawingDisabled
         && !(p_lz && char_avail() && !KeyTyped && !do_redraw);
}

/// Return true if printing messages should currently be done.
bool messaging(void)
{
  return !(p_lz && char_avail() && !KeyTyped) && ui_has_messages();
}

void win_redr_ruler(win_T *wp, bool always)
{
  bool is_stl_global = global_stl_height() > 0;
  static bool did_show_ext_ruler = false;

  // If 'ruler' off, don't do anything
  if (!p_ru) {
    return;
  }

  // Check if cursor.lnum is valid, since win_redr_ruler() may be called
  // after deleting lines, before cursor.lnum is corrected.
  if (wp->w_cursor.lnum > wp->w_buffer->b_ml.ml_line_count) {
    return;
  }

  // Don't draw the ruler while doing insert-completion, it might overwrite
  // the (long) mode message.
  if (wp == lastwin && lastwin->w_status_height == 0 && !is_stl_global) {
    if (edit_submode != NULL) {
      return;
    }
  }

  if (*p_ruf && p_ch > 0 && !ui_has(kUIMessages)) {
    const int called_emsg_before = called_emsg;
    win_redr_custom(wp, false, true);
    if (called_emsg > called_emsg_before) {
      set_string_option_direct("rulerformat", -1, "", OPT_FREE, SID_ERROR);
    }
    return;
  }

  // Check if not in Insert mode and the line is empty (will show "0-1").
  int empty_line = false;
  if ((State & MODE_INSERT) == 0 && *ml_get_buf(wp->w_buffer, wp->w_cursor.lnum, false) == NUL) {
    empty_line = true;
  }

  // Only draw the ruler when something changed.
  validate_virtcol_win(wp);
  if (redraw_cmdline
      || always
      || wp->w_cursor.lnum != wp->w_ru_cursor.lnum
      || wp->w_cursor.col != wp->w_ru_cursor.col
      || wp->w_virtcol != wp->w_ru_virtcol
      || wp->w_cursor.coladd != wp->w_ru_cursor.coladd
      || wp->w_topline != wp->w_ru_topline
      || wp->w_buffer->b_ml.ml_line_count != wp->w_ru_line_count
      || wp->w_topfill != wp->w_ru_topfill
      || empty_line != wp->w_ru_empty) {
    int width;
    int row;
    int fillchar;
    int attr;
    int off;
    bool part_of_status = false;

    if (wp->w_status_height) {
      row = W_ENDROW(wp);
      fillchar = fillchar_status(&attr, wp);
      off = wp->w_wincol;
      width = wp->w_width;
      part_of_status = true;
    } else if (is_stl_global) {
      row = Rows - (int)p_ch - 1;
      fillchar = fillchar_status(&attr, wp);
      off = 0;
      width = Columns;
      part_of_status = true;
    } else {
      row = Rows - 1;
      fillchar = ' ';
      attr = HL_ATTR(HLF_MSG);
      width = Columns;
      off = 0;
    }

    if (!part_of_status && !ui_has_messages()) {
      return;
    }

    // In list mode virtcol needs to be recomputed
    colnr_T virtcol = wp->w_virtcol;
    if (wp->w_p_list && wp->w_p_lcs_chars.tab1 == NUL) {
      wp->w_p_list = false;
      getvvcol(wp, &wp->w_cursor, NULL, &virtcol, NULL);
      wp->w_p_list = true;
    }

#define RULER_BUF_LEN 70
    char buffer[RULER_BUF_LEN];

    // Some sprintfs return the length, some return a pointer.
    // To avoid portability problems we use strlen() here.
    vim_snprintf(buffer, RULER_BUF_LEN, "%" PRId64 ",",
                 (wp->w_buffer->b_ml.ml_flags &
                  ML_EMPTY) ? (int64_t)0L : (int64_t)wp->w_cursor.lnum);
    size_t len = STRLEN(buffer);
    col_print(buffer + len, RULER_BUF_LEN - len,
              empty_line ? 0 : (int)wp->w_cursor.col + 1,
              (int)virtcol + 1);

    // Add a "50%" if there is room for it.
    // On the last line, don't print in the last column (scrolls the
    // screen up on some terminals).
    int i = (int)STRLEN(buffer);
    get_rel_pos(wp, buffer + i + 1, RULER_BUF_LEN - i - 1);
    int o = i + vim_strsize(buffer + i + 1);
    if (wp->w_status_height == 0 && !is_stl_global) {  // can't use last char of screen
      o++;
    }
    int this_ru_col = ru_col - (Columns - width);
    if (this_ru_col < 0) {
      this_ru_col = 0;
    }
    // Never use more than half the window/screen width, leave the other half
    // for the filename.
    if (this_ru_col < (width + 1) / 2) {
      this_ru_col = (width + 1) / 2;
    }
    if (this_ru_col + o < width) {
      // Need at least 3 chars left for get_rel_pos() + NUL.
      while (this_ru_col + o < width && RULER_BUF_LEN > i + 4) {
        i += utf_char2bytes(fillchar, buffer + i);
        o++;
      }
      get_rel_pos(wp, buffer + i, RULER_BUF_LEN - i);
    }

    if (ui_has(kUIMessages) && !part_of_status) {
      MAXSIZE_TEMP_ARRAY(content, 1);
      MAXSIZE_TEMP_ARRAY(chunk, 2);
      ADD_C(chunk, INTEGER_OBJ(attr));
      ADD_C(chunk, STRING_OBJ(cstr_as_string((char *)buffer)));
      ADD_C(content, ARRAY_OBJ(chunk));
      ui_call_msg_ruler(content);
      did_show_ext_ruler = true;
    } else {
      if (did_show_ext_ruler) {
        ui_call_msg_ruler((Array)ARRAY_DICT_INIT);
        did_show_ext_ruler = false;
      }
      // Truncate at window boundary.
      o = 0;
      for (i = 0; buffer[i] != NUL; i += utfc_ptr2len(buffer + i)) {
        o += utf_ptr2cells(buffer + i);
        if (this_ru_col + o > width) {
          buffer[i] = NUL;
          break;
        }
      }

      ScreenGrid *grid = part_of_status ? &default_grid : &msg_grid_adj;
      grid_puts(grid, (char_u *)buffer, row, this_ru_col + off, attr);
      grid_fill(grid, row, row + 1,
                this_ru_col + off + (int)STRLEN(buffer), off + width, fillchar,
                fillchar, attr);
    }

    wp->w_ru_cursor = wp->w_cursor;
    wp->w_ru_virtcol = wp->w_virtcol;
    wp->w_ru_empty = (char)empty_line;
    wp->w_ru_topline = wp->w_topline;
    wp->w_ru_line_count = wp->w_buffer->b_ml.ml_line_count;
    wp->w_ru_topfill = wp->w_topfill;
  }
}

/// Return the width of the 'number' and 'relativenumber' column.
/// Caller may need to check if 'number' or 'relativenumber' is set.
/// Otherwise it depends on 'numberwidth' and the line count.
int number_width(win_T *wp)
{
  int n;
  linenr_T lnum;

  if (wp->w_p_rnu && !wp->w_p_nu) {
    // cursor line shows "0"
    lnum = wp->w_height_inner;
  } else {
    // cursor line shows absolute line number
    lnum = wp->w_buffer->b_ml.ml_line_count;
  }

  if (lnum == wp->w_nrwidth_line_count) {
    return wp->w_nrwidth_width;
  }
  wp->w_nrwidth_line_count = lnum;

  n = 0;
  do {
    lnum /= 10;
    n++;
  } while (lnum > 0);

  // 'numberwidth' gives the minimal width plus one
  if (n < wp->w_p_nuw - 1) {
    n = (int)wp->w_p_nuw - 1;
  }

  // If 'signcolumn' is set to 'number' and there is a sign to display, then
  // the minimal width for the number column is 2.
  if (n < 2 && (wp->w_buffer->b_signlist != NULL)
      && (*wp->w_p_scl == 'n' && *(wp->w_p_scl + 1) == 'u')) {
    n = 2;
  }

  wp->w_nrwidth_width = n;
  return n;
}

/// Check if the new Nvim application "screen" dimensions are valid.
/// Correct it if it's too small or way too big.
void check_screensize(void)
{
  // Limit Rows and Columns to avoid an overflow in Rows * Columns.
  if (Rows < min_rows()) {
    // need room for one window and command line
    Rows = min_rows();
  } else if (Rows > 1000) {
    Rows = 1000;
  }

  if (Columns < MIN_COLUMNS) {
    Columns = MIN_COLUMNS;
  } else if (Columns > 10000) {
    Columns = 10000;
  }
}
