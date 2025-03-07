/**
 * \file markup.c
 *
 * \brief Markup handling in PennMUSH strings.
 *
 *
 */

#include "copyrite.h"
#ifdef WIN32
#include <Windows.h>
#endif
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include "ansi.h"
#include "case.h"
#include "conf.h"
#include "externs.h"
#include "game.h"
#include "log.h"
#include "mymalloc.h"
#include "parse.h"
#include "pueblo.h"
#include "strutil.h"
#include "mushsql.h"
#include "charconv.h"
#include "map_file.h"
#include "markup.h"

#define ANSI_BEGIN "\x1B["
#define ANSI_FINISH "m"

/* COL_* defines */

#define CBIT_HILITE (1)     /**< ANSI hilite attribute bit */
#define CBIT_INVERT (2)     /**< ANSI inverse attribute bit */
#define CBIT_FLASH (4)      /**< ANSI flash attribute bit */
#define CBIT_UNDERSCORE (8) /**< ANSI underscore attribute bit */

#define COL_NORMAL (0)     /**< ANSI normal */
#define COL_HILITE (1)     /**< ANSI hilite attribute value */
#define COL_UNDERSCORE (4) /**< ANSI underscore attribute value */
#define COL_FLASH (5)      /**< ANSI flag attribute value */
#define COL_INVERT (7)     /**< ANSI inverse attribute value */

#define COL_BLACK (30)   /**< ANSI color black */
#define COL_RED (31)     /**< ANSI color red */
#define COL_GREEN (32)   /**< ANSI color green */
#define COL_YELLOW (33)  /**< ANSI color yellow */
#define COL_BLUE (34)    /**< ANSI color blue */
#define COL_MAGENTA (35) /**< ANSI color magenta */
#define COL_CYAN (36)    /**< ANSI color cyan */
#define COL_WHITE (37)   /**< ANSI color white */

/* Now the code */

static int write_ansi_letters(const ansi_data *cur, char *buff, char **bp);
static int safe_markup(char const *a_tag, char *buf, char **bp, char type);
static int safe_markup_cancel(char const *a_tag, char *buf, char **bp,
                              char type);
static int escape_marked_str(char **str, char *buff, char **bp);
static bool valid_hex_digits(const char *, int);

int ansi_equal(const ansi_data *a, const ansi_data *b);
int ansi_isnull(const ansi_data a);
int safe_markup_codes(new_markup_information *mi, int end, char *buff,
                      char **bp);

static ansi_data ansi_null = NULL_ANSI;

bool colorname_lookup(const char *name, int len, int *rgb, int *ansi,
                      int *xnum);
bool rgb_lookup(int rgb, int *ansi, int *xnum);

/** Info on a color from the 16-color ANSI palette */
struct COLORMAP_16 {
  int id;         /**< Code for this color (0-7) */
  char desc;      /**< Lowercase char representing color */
  uint8_t hilite; /**< Is this char highlighted? */
  uint32_t hex;   /**< Hex code for this color */
};

/* Taken from the xterm color chart on wikipedia */
struct COLORMAP_16 colormap_16[] = {
  /* normal colors */
  {0, 'x', 0, 0x000000},
  {1, 'r', 0, 0xaa0000},
  {2, 'g', 0, 0x00aa00},
  {3, 'y', 0, 0xaaaa00},
  {4, 'b', 0, 0x0000aa},
  {5, 'm', 0, 0xaa00aa},
  {6, 'c', 0, 0x00aaaa},
  {7, 'w', 0, 0xaaaaaa},

  /* Hilite colors */
  {0, 'x', 1, 0xffffff},
  {1, 'r', 1, 0xff5555},
  {2, 'g', 1, 0x55ff55},
  {3, 'y', 1, 0xffff55},
  {4, 'b', 1, 0x5555ff},
  {5, 'm', 1, 0xff55ff},
  {6, 'c', 1, 0x55ffff},
  {7, 'w', 1, 0xffffff},

  {-1, 0, 0, 0}};

/* Populate the RGB color to name mapping */
void
build_rgb_map(void)
{
  sqlite3 *sqldb;
  sqlite3_stmt *creator;
  int status;
  MAPPED_FILE *mf;
  char *errmsg;
  const char query[] =
    "INSERT INTO colors(name, rgb, xterm, ansi) SELECT "
    "json_extract(j.value, '$.name')"
    ", from_hexstr(json_extract(j.value, '$.rgb'))"
    ", json_extract(j.value, '$.xterm')"
    ", json_extract(j.value, '$.ansi') FROM json_each(?) AS j";

  sqldb = get_shared_db();

  if (!sqldb) {
    return;
  }

  if (sqlite3_exec(sqldb,
                   "CREATE TABLE colors(name TEXT NOT NULL PRIMARY KEY COLLATE UINT,"
                   "rgb INTEGER NOT NULL, xterm INTEGER NOT NULL,"
                   "ansi INTEGER NOT NULL) WITHOUT ROWID;"
                   "CREATE INDEX rgb_idx ON colors(rgb);"
                   "CREATE VIEW named_colors AS SELECT * FROM colors WHERE "
                   "name NOT LIKE 'xterm%'",
                   NULL, NULL, &errmsg) != SQLITE_OK) {
    do_rawlog(LT_ERR, "Unable to create colors table: %s", errmsg);
    sqlite3_free(errmsg);
    return;
  }

  creator = prepare_statement_cache(sqldb, query, "colors.insert", 0);
  if (!creator) {
    return;
  }

  mf = map_file(options.colors_file, 0);
  if (!mf) {
    sqlite3_finalize(creator);
    return;
  }
  sqlite3_bind_text(creator, 1, mf->data, mf->len, SQLITE_STATIC);
  do {
    status = sqlite3_step(creator);
  } while (is_busy_status(status));
  if (status != SQLITE_DONE) {
    do_rawlog(LT_ERR, "Unable to populate colors table: %s",
              sqlite3_errmsg(sqldb));
  }
  sqlite3_finalize(creator);
  unmap_file(mf);
}

/* ARGSUSED */
FUNCTION(fun_stripansi)
{
  char *cp;
  cp = remove_markup(args[0], NULL);
  safe_str(cp, buff, bp);
}

FUNCTION(fun_ansigen)
{
  char *ptr;
  if (nargs < 1)
    return;
  for (ptr = args[0]; *ptr; ptr++) {
    switch (*ptr) {
    case '<':
      safe_chr(TAG_START, buff, bp);
      break;
    case '>':
      safe_chr(TAG_END, buff, bp);
      break;
    case '&':
      safe_chr(ESC_CHAR, buff, bp);
      break;
    default:
      safe_chr(*ptr, buff, bp);
    }
  }
}

/* ARGSUSED */
FUNCTION(fun_ansi)
{
  ansi_data colors;
  char *save = *bp;
  char *codes;

  if (!*args[1])
    return;

  codes = remove_markup(args[0], NULL);

  /* Populate the colors struct */
  if (define_ansi_data(&colors, codes)) {
    safe_format(buff, bp, T("#-1 INVALID ANSI DEFINITION: %s"), codes);
    safe_chr(' ', buff, bp);
  }

  /* If there are no colors designated at all, then just return args[1]. */
  if (!HAS_ANSI(colors)) {
    safe_strl(args[1], arglens[1], buff, bp);
    return;
  }

  /* Write the colors to buff */
  if (write_ansi_data(&colors, buff, bp)) {
    *bp = save;
    return;
  }

  /* If the contents overrun the buffer, we
   * place an ANSI_ENDALL tag at the end */
  if (safe_strl(args[1], arglens[1], buff, bp) || write_ansi_close(buff, bp)) {
    char *p = *bp - 1;
    size_t ealen = strlen(ANSI_ENDALL); /* <c/a> */

    while (p - buff > (ptrdiff_t)(BUFFER_LEN - 1 - ealen)) {
      if (*p == TAG_END) {
        /* There's an extant tag that would be overwritten by the closing tag.
         * Scan to the start of it. */
        while (*p != TAG_START)
          p -= 1;
      } else
        p -= 1;
    }

    *bp = p;
    safe_strl(ANSI_ENDALL, ealen, buff, bp);
  }
}

enum color_styles {
  CS_HEX,
  CS_16,
  CS_256,
  CS_256hex,
  CS_RGB,
  CS_NAME,
  CS_AUTO
};

/* ARGSUSED */
FUNCTION(fun_colors)
{
  if (nargs <= 1) {
    bool shown = 0;
    sqlite3 *sqldb;
    sqlite3_stmt *lister;
    int status;

    /* Return list of available color names, skipping over the 256 'xtermN'
     * colors */

    sqldb = get_shared_db();
    if (!sqldb) {
      safe_str(T("#-1 SQLITE ERROR"), buff, bp);
      return;
    }

    if (args[0] && *args[0]) {
      /* List colors matching a wildcard. */
      lister = prepare_statement(
        sqldb,
        "SELECT name FROM colors WHERE name LIKE ? ESCAPE '$' ORDER BY name",
        "colors.list.names_pattern");
      if (lister) {
        int len, ulen;
        char *as_utf8;
        char *converted = glob_to_like(args[0], '$', &len);
        as_utf8 = latin1_to_utf8(converted, len, &ulen, "string");
        mush_free(converted, "string");
        sqlite3_bind_text(lister, 1, as_utf8, ulen, free_string);
      }
    } else {
      /* List all colors but xtermXX ones */
      lister =
        prepare_statement(sqldb, "SELECT name FROM named_colors ORDER BY name",
                          "colors.list.names_all");
    }
    if (!lister) {
      safe_str(T("#-1 SQLITE ERROR"), buff, bp);
      return;
    }

    do {
      status = sqlite3_step(lister);
      if (status == SQLITE_ROW) {
        const char *name = (const char *) sqlite3_column_text(lister, 0);
        if (shown)
          safe_chr(' ', buff, bp);
        else
          shown = 1;
        safe_strl(name, sqlite3_column_bytes(lister, 0), buff, bp);
      }
    } while (status == SQLITE_ROW || is_busy_status(status));
    sqlite3_reset(lister);
  } else if (nargs == 2) {
    /* Return color info for a specific color */
    ansi_data ad;
    char *color;
    char *curr, *list;
    int i;
    enum color_styles cs = CS_HEX;
    bool ansi_styles = 0;

    if (define_ansi_data(&ad, args[0])) {
      safe_str(T("#-1 INVALID COLOR"), buff, bp);
      return;
    }

    if ((!ad.fg[0] || (!ad.fg[1] && (ad.fg[0] == 'n' || ad.fg[0] == 'd'))) &&
        (!ad.bg[0] || (!ad.bg[1] && (ad.bg[0] == 'n' || ad.bg[0] == 'D')))) {
      safe_str(T("#-1 COLORS() REQUIRES AT LEAST ONE COLOR"), buff, bp);
      return;
    }

    list = trim_space_sep(args[1], ' ');
    while ((curr = split_token(&list, ' ')) != NULL) {
      if (!*curr)
        continue;
      if (strcmp("hex", curr) == 0 || strcmp("x", curr) == 0)
        cs = CS_HEX;
      else if (strcmp("16color", curr) == 0 || strcmp("c", curr) == 0)
        cs = CS_16;
      else if (strcmp("256color", curr) == 0 || strcmp("xterm256", curr) == 0 ||
               strcmp("d", curr) == 0)
        cs = CS_256;
      else if (strcmp("xterm256x", curr) == 0 || strcmp("h", curr) == 0)
        cs = CS_256hex;
      else if (strcmp("rgb", curr) == 0 || strcmp("r", curr) == 0)
        cs = CS_RGB;
      else if (!strcmp("name", curr))
        cs = CS_NAME;
      else if (!strcmp("auto", curr))
        cs = CS_AUTO;
      else if (!strcmp("styles", curr))
        ansi_styles = 1;
      else {
        safe_str(T("#-1 INVALID ARGUMENT"), buff, bp);
        return;
      }
    }

    if (ansi_styles) {
      if (!ad.fg[0] && (ad.bits & CBIT_HILITE)) {
        /* If there's a FG color, this will be added later */
        safe_chr('h', buff, bp);
      }
      if (ad.bits & CBIT_UNDERSCORE)
        safe_chr('u', buff, bp);
      if (ad.bits & CBIT_FLASH)
        safe_chr('f', buff, bp);
      if (ad.bits & CBIT_INVERT)
        safe_chr('i', buff, bp);
    }

    for (i = 0; i < 2; i++) {
      bool hilite = 0;
      int j;
      color = (i ? ad.bg : ad.fg);
      if (!*color)
        continue;
      if (i && (cs != CS_16 && cs != CS_AUTO))
        safe_chr('/', buff, bp);

      switch (cs) {
      case CS_AUTO:
        if (i && ad.fg[0]) {
          if (ad.fg[1] && !color[1])
            safe_chr(' ', buff, bp);
          else if (color[1])
            safe_chr('/', buff, bp);
        }
        safe_str(color, buff, bp);
        if (!i && !color[1] && (ad.bits & CBIT_HILITE))
          safe_chr('h', buff, bp);
        break;
      case CS_HEX:
        safe_format(buff, bp, "#%06x",
                    color_to_hex(color, (!i && (ad.bits & CBIT_HILITE))));
        break;
      case CS_RGB: {
        uint32_t hex = color_to_hex(color, (!i && (ad.bits & CBIT_HILITE)));
        safe_format(buff, bp, "%d %d %d", (hex >> 16) & 0xFF, (hex >> 8) & 0xFF,
                    hex & 0xFF);
        break;
      }
      case CS_16:
        j = ansi_map_16(color, i, &hilite);

        if (j)
          safe_chr(colormap_16[j - (i ? 40 : 30)].desc - (i ? 32 : 0), buff,
                   bp);
        else
          safe_chr(i ? 'D' : 'd', buff, bp);
        if (!i && (hilite || (ad.bits & CBIT_HILITE)))
          safe_chr('h', buff, bp);
        break;
      case CS_256:
        safe_integer(ansi_map_256(color, (!i && (ad.bits & CBIT_HILITE)), 0),
                     buff, bp);
        break;
      case CS_256hex:
        safe_format(buff, bp, "%x",
                    ansi_map_256(color, (!i && (ad.bits & CBIT_HILITE)), 0));
        break;
      case CS_NAME: {
        uint32_t hex;
        bool shown = 0;
        sqlite3 *sqldb;
        sqlite3_stmt *finder;
        int status;

        hex = color_to_hex(color, 0);

        sqldb = get_shared_db();
        if (!sqldb) {
          safe_str(T("#-1 SQLITE ERROR"), buff, bp);
          return;
        }

        finder = prepare_statement(
          sqldb, "SELECT name FROM named_colors WHERE rgb = ? ORDER BY name",
          "colors.list.rgb");
        if (!finder) {
          safe_str(T("#-1 SQLITE ERROR"), buff, bp);
          return;
        }

        sqlite3_bind_int(finder, 1, (int) hex);
        do {
          status = sqlite3_step(finder);
          if (status == SQLITE_ROW) {
            const char *name = (const char *) sqlite3_column_text(finder, 0);
            if (shown) {
              safe_chr(' ', buff, bp);
            }
            safe_strl(name, sqlite3_column_bytes(finder, 0), buff, bp);
            shown = 1;
          }
        } while (status == SQLITE_ROW || is_busy_status(status));
        sqlite3_reset(finder);

        if (!shown)
          safe_str(T("#-1 NO MATCHING COLOR NAME"), buff, bp);
      } break;
      }
    }
  }
}

/* File generated by gperf */
#include "htmltab.c"

/* ARGSUSED */
FUNCTION(fun_html) { safe_tag(args[0], buff, bp); }

/* ARGSUSED */
FUNCTION(fun_tag)
{
  int i;
  if (!Can_Send_OOB(executor) && !is_allowed_tag(args[0], arglens[0])) {
    safe_str("#-1", buff, bp);
    return;
  }
  safe_chr(TAG_START, buff, bp);
  safe_chr(MARKUP_HTML, buff, bp);
  safe_strl(args[0], arglens[0], buff, bp);
  for (i = 1; i < nargs; i++) {
    if (ok_tag_attribute(executor, args[i])) {
      safe_chr(' ', buff, bp);
      safe_strl(args[i], arglens[i], buff, bp);
    }
  }
  safe_chr(TAG_END, buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_endtag)
{
  if (!Can_Send_OOB(executor) && !is_allowed_tag(args[0], arglens[0]))
    safe_str("#-1", buff, bp);
  else
    safe_tag_cancel(args[0], buff, bp);
}

/* ARGSUSED */
FUNCTION(fun_tagwrap)
{
  if (!Can_Send_OOB(executor) && !is_allowed_tag(args[0], arglens[0]))
    safe_str("#-1", buff, bp);
  else {
    if (nargs == 2)
      safe_tag_wrap(args[0], NULL, args[1], buff, bp, executor);
    else
      safe_tag_wrap(args[0], args[1], args[2], buff, bp, executor);
  }
}

/** A version of strlen that ignores ansi and HTML sequences.
 * \param p string to get length of.
 * \return length of string p, not including ansi/html sequences.
 */
int
ansi_strlen(const char *p)
{
  int i = 0;

  if (!p)
    return 0;

  while (*p) {
    if (*p == TAG_START) {
      while ((*p) && (*p != TAG_END))
        p++;
    } else if (*p == ESC_CHAR) {
      while ((*p) && (*p != 'm'))
        p++;
    } else {
      i++;
    }
    p++;
  }
  return i;
}

/** Returns the apparent length of a string, up to numchars visible
 * characters. The apparent length skips over nonprinting ansi and
 * tags.
 * \param p string.
 * \param numchars maximum size to report.
 * \return apparent length of string.
 */
int
ansi_strnlen(const char *p, size_t numchars)
{
  size_t i = 0;

  if (!p)
    return 0;
  while (*p && numchars > 0) {
    if (*p == ESC_CHAR) {
      while ((*p) && (*p != 'm')) {
        p++;
      }
    } else if (*p == TAG_START) {
      while ((*p) && (*p != TAG_END)) {
        p++;
      }
    } else
      numchars--;
    i++;
    p++;
  }
  return i;
}

/** Compare two strings, ignoring all ansi and html markup from a string.
 *  Is *NOT* locale safe (a la strcoll)
 * \param astr string to compare to
 * \param bstr Other string
 * \return int - 0 is identical, -1 or 1 for difference.
 */
int
ansi_strcmp(const char *astr, const char *bstr)
{
  char abuf[BUFFER_LEN];
  char *tmp;
  size_t alen = 0;

  tmp = remove_markup(astr, &alen);
  memcpy(abuf, tmp, alen);
  astr = abuf;
  bstr = remove_markup(bstr, NULL);

  return strcmp(astr, bstr);
}

/** Compare ansi_data for exact equality.
 * \param a ansi_data to compare
 * \param b other ansi_data
 * \return int - 1 is identical, 0 is different
 */
int
ansi_equal(const ansi_data *a, const ansi_data *b)
{
  return ((a->bits == b->bits) && (a->offbits == b->offbits) &&
          (!strcasecmp(a->fg, b->fg)) && (!strcasecmp(a->bg, b->bg)));
}

/** Return true if ansi_data contains no ansi values.
 * \param a ansi_data to check
 * \return int 1 on ansi_null, 0 otherwise
 */
int
ansi_isnull(const ansi_data a)
{
  return !HAS_ANSI(a);
}

/** Strip all ANSI and HTML markup from a string. As a side effect,
 * stores the length of the stripped string in a provided address.
 * NOTE! Length returned is length *including* the terminating NULL,
 * because we usually memcpy the result.
 * \param orig string to strip.
 * \param s_len address to store length of stripped string, if provided.
 * \return pointer to static buffer containing stripped string.
 */
char *
remove_markup(const char *orig, size_t *s_len)
{
  static char buff[BUFFER_LEN];
  char *bp = buff;
  const char *q;
  size_t len = 0;

  if (!orig) {
    if (s_len)
      *s_len = 0;
    return NULL;
  }

  for (q = orig; *q;) {
    switch (*q) {
    case ESC_CHAR:
      /* Skip over ansi */
      while (*q && *q++ != 'm')
        ;
      break;
    case TAG_START:
      /* Skip over HTML */
      while (*q && *q++ != TAG_END)
        ;
      break;
    default:
      safe_chr(*q++, buff, &bp);
      len++;
    }
  }
  *bp = '\0';
  if (s_len)
    *s_len = len + 1;
  return buff;
}

static char ansi_chars[50];
static int ansi_codes[255];

#define BUILD_ANSI(letter, ESCcode)                                            \
  do {                                                                         \
    ansi_chars[ESCcode] = letter;                                              \
    ansi_codes[letter] = ESCcode;                                              \
  } while (0)

/** Set up the table of ansi codes */
void
init_ansi_codes(void)
{
  memset(ansi_chars, 0, sizeof(ansi_chars));
  memset(ansi_codes, 0, sizeof(ansi_codes));
  /*
    BUILD_ANSI('n', COL_NORMAL);
    BUILD_ANSI('f', COL_FLASH);
    BUILD_ANSI('h', COL_HILITE);
    BUILD_ANSI('i', COL_INVERT);
    BUILD_ANSI('u', COL_UNDERSCORE);
  */
  BUILD_ANSI('x', COL_BLACK);
  BUILD_ANSI('X', COL_BLACK + 10);
  BUILD_ANSI('r', COL_RED);
  BUILD_ANSI('R', COL_RED + 10);
  BUILD_ANSI('g', COL_GREEN);
  BUILD_ANSI('G', COL_GREEN + 10);
  BUILD_ANSI('y', COL_YELLOW);
  BUILD_ANSI('Y', COL_YELLOW + 10);
  BUILD_ANSI('b', COL_BLUE);
  BUILD_ANSI('B', COL_BLUE + 10);
  BUILD_ANSI('m', COL_MAGENTA);
  BUILD_ANSI('M', COL_MAGENTA + 10);
  BUILD_ANSI('c', COL_CYAN);
  BUILD_ANSI('C', COL_CYAN + 10);
  BUILD_ANSI('w', COL_WHITE);
  BUILD_ANSI('W', COL_WHITE + 10);
}

#undef BUILD_ANSI

/** Write an internal markup tag for an ansi_data.
 * \param cur the ansi_data to write
 * \param buff buffer to write to
 * \param bp pointer to buff to write at
 * \retval 0 on success, >0 if the end of the buffer was hit before outputting
 * everything.
 */
int
write_ansi_data(ansi_data *cur, char *buff, char **bp)
{
  int retval = 0;
  retval += safe_chr(TAG_START, buff, bp);
  retval += safe_chr(MARKUP_COLOR, buff, bp);
  retval += write_ansi_letters(cur, buff, bp);
  retval += safe_chr(TAG_END, buff, bp);
  return retval;
}

/** Write a closing internal markup tag for color.
 * \param buff buffer to write to
 * \param bp pointer to buff to write at
 * \return 0 on success, >0 if the end of the buffer was hit.
 */
int
write_ansi_close(char *buff, char **bp)
{
  int retval = 0;
  retval += safe_chr(TAG_START, buff, bp);
  retval += safe_chr(MARKUP_COLOR, buff, bp);
  retval += safe_chr('/', buff, bp);
  retval += safe_chr(TAG_END, buff, bp);
  return retval;
}

/** Write the color codes, which would be used by ansi() to recreate
 * the given ansi_data, into a buffer.
 * \param cur the ansi data to write
 * \param buff buffer to write to
 * \param bp position in buffer to write at
 * \return 0 on success, >0 if the end of the buffer was hit.
 */
static int
write_ansi_letters(const ansi_data *cur, char *buff, char **bp)
{
  int retval = 0;
  char *save;
  save = *bp;
  if (cur->fg[0] == 'n') {
    retval += safe_chr('n', buff, bp);
  } else {
#define CBIT_ON(x, y) (x->bits & y)
    if (CBIT_ON(cur, CBIT_FLASH))
      retval += safe_chr('f', buff, bp);
    if (CBIT_ON(cur, CBIT_HILITE))
      retval += safe_chr('h', buff, bp);
    if (CBIT_ON(cur, CBIT_INVERT))
      retval += safe_chr('i', buff, bp);
    if (CBIT_ON(cur, CBIT_UNDERSCORE))
      retval += safe_chr('u', buff, bp);
#undef CBIT_ON
#define CBIT_OFF(x, y) (x->offbits & y)
    if (CBIT_OFF(cur, CBIT_FLASH))
      retval += safe_chr('F', buff, bp);
    if (CBIT_OFF(cur, CBIT_HILITE))
      retval += safe_chr('H', buff, bp);
    if (CBIT_OFF(cur, CBIT_INVERT))
      retval += safe_chr('I', buff, bp);
    if (CBIT_OFF(cur, CBIT_UNDERSCORE))
      retval += safe_chr('U', buff, bp);
#undef CBIT_OFF

    if (cur->bg[0] && cur->bg[0] != '+' && cur->bg[0] != '#') {
      retval += safe_chr(cur->bg[0], buff, bp);
    }
    if (cur->fg[0]) {
      if (cur->fg[0] == '+' || cur->fg[0] == '#') {
        retval += safe_str(cur->fg, buff, bp);
      } else {
        retval += safe_chr(cur->fg[0], buff, bp);
      }
    }
    if (cur->bg[0] == '+' || cur->bg[0] == '#') {
      retval += safe_chr('!', buff, bp);
      retval += safe_str(cur->bg, buff, bp);
    }
  }

  if (retval)
    *bp = save;
  return retval;
}

void
nest_ansi_data(ansi_data *old, ansi_data *cur)
{
  if (cur->fg[0] != 'n') {
    cur->bits |= old->bits;
    cur->bits &= ~cur->offbits;
    if (!cur->fg[0]) {
      memcpy(cur->fg, old->fg, COLOR_NAME_LEN);
    }
    if (!cur->bg[0]) {
      memcpy(cur->bg, old->bg, COLOR_NAME_LEN);
    }
  } else {
    cur->bits = 0;
    cur->offbits = 0;
    cur->bg[0] = '\0';
  }
}

#define ERROR_COLOR 0xff69b4 /* Hot Pink. */

bool
colorname_lookup(const char *name, int len, int *rgb, int *ansi, int *xnum)
{
  sqlite3 *sqldb;
  sqlite3_stmt *finder;
  int status;
  int ulen;
  char *utf8;

  sqldb = get_shared_db();
  if (!sqldb) {
    return 0;
  }

  finder = prepare_statement(
    sqldb, "SELECT rgb, ansi, xterm FROM colors WHERE name = ?",
    "colors.lookup.name");
  if (!finder) {
    return 0;
  }

  utf8 = latin1_to_utf8(name, len, &ulen, "string");
  sqlite3_bind_text(finder, 1, utf8, ulen, free_string);
  do {
    status = sqlite3_step(finder);
  } while (is_busy_status(status));
  if (status == SQLITE_ROW) {
    if (rgb) {
      *rgb = sqlite3_column_int(finder, 0);
    }
    if (ansi) {
      *ansi = sqlite3_column_int(finder, 1);
    }
    if (xnum) {
      *xnum = sqlite3_column_int(finder, 2);
    }
  }
  sqlite3_reset(finder);
  return status == SQLITE_ROW;
}

bool
rgb_lookup(int rgb, int *ansi, int *xnum)
{
  sqlite3 *sqldb;
  sqlite3_stmt *finder;
  int status;

  sqldb = get_shared_db();
  if (!sqldb) {
    return 0;
  }

  finder = prepare_statement(
    sqldb, "SELECT ansi, xterm FROM colors WHERE rgb = ?", "colors.lookup.rgb");
  if (!finder) {
    return 0;
  }

  sqlite3_bind_int(finder, 1, rgb);
  do {
    status = sqlite3_step(finder);
  } while (is_busy_status(status));
  if (status == SQLITE_ROW) {
    if (ansi) {
      *ansi = sqlite3_column_int(finder, 0);
    }
    if (xnum) {
      *xnum = sqlite3_column_int(finder, 1);
    }
  }
  sqlite3_reset(finder);
  return status == SQLITE_ROW;
}

/** Return the hex code for a given ANSI color */
uint32_t
color_to_hex(const char *name, bool hilite)
{
  int i = 0;
  const char *q;
  char n;
  char buf[BUFFER_LEN] = {'\0'}, *p;

  /* This should've been checked before it ever got here. */

  if (!name || !name[0]) {
    return 0;
  }

  if (name[0] == '#') {
    return strtol(name + 1, NULL, 16);
  }
  if (name[0] == '+') {
    int hex = 0;
    int len = 0;

    name++;
    /* Downcase and remove all spaces. */
    p = buf;
    for (q = name; *q; q++) {
      if (isspace(*q))
        continue;
      *(p++) = tolower(*q);
      len += 1;
    }
    *p = '\0';

    if (colorname_lookup(buf, len, &hex, NULL, NULL)) {
      return hex;
    }

    /* It's an invalid color. Return hot pink since we shouldn't have gotten
     * here? */
    return ERROR_COLOR;
  }
  /* We only get here if it's old-style ansi. */
  if (name[1]) {
    /* Invalid character code! */
    return ERROR_COLOR;
  }
  n = tolower(name[0]);
  if (hilite) {
    for (i = 8; i < 16; i++) {
      if (colormap_16[i].desc == n) {
        return colormap_16[i].hex;
      }
    }
  } else {
    for (i = 0; i < 8; i++) {
      if (colormap_16[i].desc == n) {
        return colormap_16[i].hex;
      }
    }
  }

  /* It's an invalid color. Return hot pink since we shouldn't have gotten here?
   */
  return ERROR_COLOR;
}

/* Color differences is the square of the individual color differences. */
#define color_diff(a, b) (((a) - (b)) * ((a) - (b)))

#define hex_difference(a, b)                                                   \
  (color_diff(a & 0xFF, b & 0xFF) +                                            \
   color_diff((a >> 8) & 0xFF, (b >> 8) & 0xFF) +                              \
   color_diff((a >> 16) & 0xFF, (b >> 16) & 0xFF))

#define ANSI_FG 0
#define ANSI_BG 1

/** Map a color (old-style ANSI code, color name or hex value) to the
    16-color ANSI palette */
int
ansi_map_16(const char *name, bool bg, bool *hilite)
{
  uint32_t hex, diff, cdiff;
  int best = 0;
  int i;
  int max;
  int ansi = 0;

  *hilite = 0;

  /* Shortcut: If it's a single character color code, it's using the 16 color
   * map. */
  if (name[0] && !name[1]) {
    return ansi_codes[name[0]];
  }

  /* Is it an xterm color number? */
  if (strncasecmp(name, "+xterm", 5) == 0) {
    unsigned int xnum;
    char xname[16];
    int len;

    xnum = strtoul(name + 6, NULL, 10);
    if (xnum > 255)
      xnum = 255;

    len = snprintf(xname, sizeof xname, "xterm%u", xnum);
    colorname_lookup(xname, len, NULL, &ansi, NULL);

    if (!bg && ansi & 0x0100) {
      *hilite = 1;
    }
    return (ansi & 0xFF) + (bg ? 40 : 30);
  }

  /* Otherwise it's a name or RGB sequence. Map it to hex. */
  hex = color_to_hex(name, 0);

  /* Predefined color names have their downgrades cached */
  if (rgb_lookup(hex, &ansi, NULL)) {
    if (!bg && ansi & 0x0100) {
      *hilite = 1;
    }
    return (ansi & 0xFF) + (bg ? 40 : 30);
  }

  diff = 0x0FFFFFFF;
  /* Now find the closest 16 color match. */
  best = 0;

  /* TODO: Figure out a way to use hilite to improve color map? Then can
   * use max=16 for foreground (but not background) */
  max = 8;
  for (i = 0; i < max; i++) {
    cdiff = hex_difference(colormap_16[i].hex, hex);
    if (cdiff < diff) {
      best = i;
      diff = cdiff;
    }
  }
  /* */
  if (bg) {
    return colormap_16[best].id + 40;
  } else {
    return colormap_16[best].id + 30;
  }
}

/** Map a RGB hex color to the 256-color XTERM palette */
int
ansi_map_256(const char *name, bool hilite, bool all)
{
  uint32_t hex, diff, cdiff;
  int best = -1;
  int num = 0;
  sqlite3 *sqldb;
  sqlite3_stmt *finder;
  int status;

  /* Is it an xterm color number? */
  if (strncasecmp(name, "+xterm", 6) == 0) {
    unsigned int xnum;
    xnum = strtoul(name + 6, NULL, 10);
    if (xnum > 255)
      xnum = 255;
    return xnum;
  }

  /* Predefined color names have their downgrades cached */
  hex = color_to_hex(name, hilite);
  if (rgb_lookup(hex, NULL, &num)) {
    return num;
  }

  diff = 0x0FFFFFFF;
  /* Now find the closest 256 color match. */

  sqldb = get_shared_db();
  if (!sqldb) {
    return -1;
  }

  finder = prepare_statement(
    sqldb, "SELECT rgb, xterm FROM colors WHERE name LIKE 'xterm%'",
    "colors.list.xterm");
  if (!finder) {
    return -1;
  }

  do {
    status = sqlite3_step(finder);
    if (status == SQLITE_ROW) {
      uint32_t rgb = sqlite3_column_int(finder, 0);
      num = sqlite3_column_int(finder, 1);

      if (all && num < 16) {
        continue;
      }

      if (hex == rgb) {
        best = num;
        break;
      }

      cdiff = hex_difference(rgb, hex);
      if (cdiff < diff) {
        best = num;
        diff = cdiff;
      }
    }
  } while (status == SQLITE_ROW || is_busy_status(status));
  sqlite3_reset(finder);

  return best;
}

typedef int (*writer_func)(ansi_data *old, ansi_data *cur, int ansi_format,
                           char *buff, char **bp);
#define ANSI_WRITER(name)                                                      \
  int name(ansi_data *old __attribute__((__unused__)),                         \
           ansi_data *cur __attribute__((__unused__)),                         \
           int ansi_format __attribute__((__unused__)),                        \
           char *buff __attribute__((__unused__)),                             \
           char **bp __attribute__((__unused__)));                             \
  int name(ansi_data *old __attribute__((__unused__)),                         \
           ansi_data *cur __attribute__((__unused__)),                         \
           int ansi_format __attribute__((__unused__)),                        \
           char *buff __attribute__((__unused__)),                             \
           char **bp __attribute__((__unused__)))

/* We need EDGE_UP to return 1 if x has bit set and y doesn't. */
#define EDGE_UP(x, y, z) (((x)->bits & (z)) != ((y)->bits & (z)))

ANSI_WRITER(ansi_reset) { return safe_str(ANSI_RAW_NORMAL, buff, bp); }

ANSI_WRITER(ansi_16color)
{
  int ret = 0;
  int f = 0;
  bool hilite = 0;

#define maybe_append_code(code)                                                \
  do {                                                                         \
    if (EDGE_UP(old, cur, CBIT_##code)) {                                      \
      if (f++)                                                                 \
        ret += safe_chr(';', buff, bp);                                        \
      else                                                                     \
        ret += safe_str(ANSI_BEGIN, buff, bp);                                 \
      ret += safe_integer(COL_##code, buff, bp);                               \
    }                                                                          \
  } while (0)

  maybe_append_code(HILITE);
  maybe_append_code(INVERT);
  maybe_append_code(FLASH);
  maybe_append_code(UNDERSCORE);

#undef maybe_append_code

  if (cur->fg[0] && strcmp(cur->fg, old->fg)) {
    if (f++)
      ret += safe_chr(';', buff, bp);
    else
      ret += safe_str(ANSI_BEGIN, buff, bp);
    ret += safe_integer(ansi_map_16(cur->fg, ANSI_FG, &hilite), buff, bp);
    if (hilite && !EDGE_UP(old, cur, CBIT_HILITE)) {
      ret += safe_chr(';', buff, bp);
      ret += safe_integer(COL_HILITE, buff, bp);
      cur->bits |= CBIT_HILITE;
      cur->offbits &= ~CBIT_HILITE;
    }
  }
  if (cur->bg[0] && strcmp(cur->bg, old->bg)) {
    if (f++)
      ret += safe_chr(';', buff, bp);
    else
      ret += safe_str(ANSI_BEGIN, buff, bp);
    ret += safe_integer(ansi_map_16(cur->bg, ANSI_BG, &hilite), buff, bp);
  }

  if (f)
    return ret + safe_str(ANSI_FINISH, buff, bp);
  else
    return ret;
}

ANSI_WRITER(ansi_hilite)
{
  int ret = 0;
  if (!EDGE_UP(old, cur, CBIT_HILITE)) {
    return 0;
  }
  ret += safe_str(ANSI_BEGIN, buff, bp);
  ret += safe_integer(COL_HILITE, buff, bp);
  return ret + safe_str(ANSI_FINISH, buff, bp);
}

#define is_new_ansi(x) (strchr(x, '+') || strchr(x, '#') || strchr(x, '/'))

ANSI_WRITER(ansi_xterm256)
{
  bool hilite = EDGE_UP(old, cur, CBIT_HILITE);
  int ret = 0;
  int f = 0;
  int bg = -1, fg = -1;

  /* If it's old-style ansi, then use ansi_16color */
  if (!(is_new_ansi(cur->fg) || is_new_ansi(cur->bg))) {
    return ansi_16color(old, cur, ansi_format, buff, bp);
  }
#define maybe_append_code(code)                                                \
  do {                                                                         \
    if (EDGE_UP(old, cur, CBIT_##code)) {                                      \
      if (f++) {                                                               \
        ret += safe_chr(';', buff, bp);                                        \
      } else {                                                                 \
        ret += safe_str(ANSI_BEGIN, buff, bp);                                 \
      }                                                                        \
      ret += safe_integer(COL_##code, buff, bp);                               \
    }                                                                          \
  } while (0)

  maybe_append_code(HILITE);
  maybe_append_code(INVERT);
  maybe_append_code(FLASH);
  maybe_append_code(UNDERSCORE);

#undef maybe_append_code

  if (cur->fg[0] && strcmp(old->fg, cur->fg)) {
    if (is_new_ansi(cur->fg)) {
      if (!strncasecmp(cur->fg, "+xterm", 6))
        fg = atoi(cur->fg + 6);
      else
        fg = ansi_map_256(cur->fg, hilite, 0);
    } else {
      if (f)
        ret += safe_chr(';', buff, bp);
      else {
        f++;
        ret += safe_str(ANSI_BEGIN, buff, bp);
      }
      ret += safe_integer(ansi_codes[cur->fg[0]], buff, bp);
    }
  }

  if (cur->bg[0] && strcmp(old->bg, cur->bg)) {
    if (is_new_ansi(cur->bg)) {
      if (!strncasecmp(cur->bg, "+xterm", 6))
        bg = atoi(cur->bg + 6);
      else
        bg = ansi_map_256(cur->bg, hilite, 0);
    } else {
      if (f)
        ret += safe_chr(';', buff, bp);
      else {
        f++;
        ret += safe_str(ANSI_BEGIN, buff, bp);
      }
      ret += safe_integer(ansi_codes[cur->bg[0]], buff, bp);
    }
  }

  /* 256 color should be separate from the bits set. */
  if (f) {
    ret += safe_str(ANSI_FINISH, buff, bp);
  }

  if (fg > -1)
    ret += safe_format(buff, bp, "%s38;5;%d%s", ANSI_BEGIN, fg, ANSI_FINISH);

  if (bg > -1) {
    ret += safe_format(buff, bp, "%s48;5;%d%s", ANSI_BEGIN, bg, ANSI_FINISH);
  }
  return ret;
}

/** Holds data on which functions to use for writing ANSI color data in various
 * formats */
struct ansi_writer {
  /* Write ansi_normal or otherwise reset. */
  int format_type;   /**< An ANSI_FORMAT_* int, specifying the type of ansi data
                        to write */
  writer_func reset; /**< Function to end the ANSI color block */
  writer_func
    change; /**< Function to write the codes when there's a change of color */
};

struct ansi_writer ansi_writers[] = {
  {ANSI_FORMAT_16COLOR, ansi_reset, ansi_16color},
  {ANSI_FORMAT_HILITE, ansi_reset, ansi_hilite},
  {ANSI_FORMAT_XTERM256, ansi_reset, ansi_xterm256},
  /* For now, HTML uses 16 color, since most Pueblo clients don't support it. */
  {ANSI_FORMAT_HTML, ansi_reset, ansi_16color},
  {-1, NULL, NULL}};

int
write_raw_ansi_data(ansi_data *old, ansi_data *cur, int ansi_format, char *buff,
                    char **bp)
{
  struct ansi_writer *aw = &ansi_writers[0];
  int ret = 0;
  int i;

  if (ansi_format == ANSI_FORMAT_NONE) {
    return 0;
  }

  for (i = 0; ansi_writers[i].format_type >= 0; i++) {
    if (ansi_writers[i].format_type == ansi_format) {
      aw = &ansi_writers[i];
      break;
    }
  }

  if (!cur) {
    return aw->reset(old, cur, ansi_format, buff, bp);
  }

  /* This shouldn't happen (Are you sure? MG) */
  if (!strcmp(cur->fg, "n")) {
    if (old->bits || (strcmp(old->fg, "n")) || old->bg[0]) {
      return aw->reset(old, cur, ansi_format, buff, bp);
    }
  }
  if (cur->fg[0] == 'd') {
    cur->fg[0] = '\0';
  }
  if (cur->bg[0] == 'D') {
    cur->bg[0] = '\0';
  }

  /* Do we *unset* anything in cur? */
  if ((old->bits & ~(cur->bits)) || (old->bg[0] && !cur->bg[0]) ||
      (old->fg[0] && !cur->fg[0])) {
    ret += aw->reset(old, cur, ansi_format, buff, bp);
    old = &ansi_null;
  }

  if (ansi_equal(old, cur)) {
    return ret;
  }

  if (!(cur->fg[0] || cur->bg[0] || cur->bits)) {
    if (old->fg[0]) {
      return ret += aw->reset(old, cur, ansi_format, buff, bp);
    }
    return ret;
  }

  return ret += aw->change(old, cur, ansi_format, buff, bp);
}

/** Validate a color name for ansi(). name does NOT include
 * the leading '+'
 */
int
valid_color_name(const char *name)
{
  int len = 0;
  char *p;
  const char *q;
  char buff[BUFFER_LEN];

  for (p = buff, q = name; *q; q++) {
    if (isspace(*q))
      continue;
    *(p++) = tolower(*q);
    len += 1;
  }
  *p = '\0';

  return colorname_lookup(buff, len, NULL, NULL, NULL);
}

extern const unsigned char *tables;

/* Returns true if the argument is nothing but hexadecimal digits. */
static bool
valid_hex_digits(const char *digits, int len)
{
  static pcre2_code *re = NULL;
  static pcre2_match_data *md = NULL;

  if (!re) {
    int errcode;
    PCRE2_SIZE erroff;

    re = pcre2_compile(
      (const PCRE2_UCHAR *) "^[[:xdigit:]]+$", PCRE2_ZERO_TERMINATED,
      re_compile_flags | PCRE2_NO_UTF_CHECK, &errcode, &erroff, re_compile_ctx);
    if (!re) {
      char errstr[120];
      pcre2_get_error_message(errcode, (PCRE2_UCHAR *) errstr, sizeof errstr);
      do_rawlog(LT_ERR, "valid_hex_code: Unable to compile re: %s", errstr);
      return 0;
    }
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    md = pcre2_match_data_create_from_pattern(re, NULL);
  }

  if (!digits) {
    return 0;
  }

  return pcre2_match(re, (const PCRE2_UCHAR *) digits, len, 0, re_match_flags,
                     md, re_match_ctx) >= 0;
}

/* Return true if s is in the format <#RRGGBB>, with optional spaces. */
static bool
valid_angle_hex(const char *s, int len)
{
  static pcre2_code *re = NULL;
  static pcre2_match_data *md = NULL;

  if (!re) {
    int errcode;
    PCRE2_SIZE erroff;

    re = pcre2_compile((const PCRE2_UCHAR *) "^<\\s*#[[:xdigit:]]{6}\\s*>\\s*$",
                       PCRE2_ZERO_TERMINATED,
                       re_compile_flags | PCRE2_NO_UTF_CHECK, &errcode, &erroff,
                       re_compile_ctx);
    if (!re) {
      char errstr[120];
      pcre2_get_error_message(errcode, (PCRE2_UCHAR *) errstr, sizeof errstr);
      do_rawlog(LT_ERR, "valid_angle_hex: Unable to compile re: %s", errstr);
      return 0;
    }
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    md = pcre2_match_data_create_from_pattern(re, NULL);
  }

  if (!s) {
    return 0;
  }

  return pcre2_match(re, (const PCRE2_UCHAR *) s, len, 0, re_match_flags, md,
                     re_match_ctx) >= 0;
}

/* Return true if s of the format <R G B>, and store the color in
   RRGGBB format in rgbs, which must be of at least size 7. If it
   returns false, rgbs contents is undefined. */
static bool
valid_angle_triple(const char *s, int len, char *rgbs)
{
  static pcre2_code *re = NULL;
  static pcre2_match_data *md = NULL;
  int matches;
  int n;
  char *rgbsp = rgbs;

  if (!re) {
    int errcode;
    PCRE2_SIZE erroff;

    re = pcre2_compile(
      (const PCRE2_UCHAR *) "^<\\s*(\\d{1,3})\\s+((?1))\\s+((?1))\\s*>\\s*$",
      PCRE2_ZERO_TERMINATED, re_compile_flags | PCRE2_NO_UTF_CHECK, &errcode,
      &erroff, re_compile_ctx);
    if (!re) {
      char errstr[120];
      pcre2_get_error_message(errcode, (PCRE2_UCHAR *) errstr, sizeof errstr);
      do_rawlog(LT_ERR, "valid_angle_triple: Unable to compile re: %s", errstr);
      return 0;
    }
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    md = pcre2_match_data_create_from_pattern(re, NULL);
  }

  if (!s) {
    return 0;
  }

  matches = pcre2_match(re, (const PCRE2_UCHAR *) s, len, 0, re_match_flags, md,
                        re_match_ctx);
  if (matches != 4) {
    return 0;
  }

  for (n = 1; n < 4; n += 1) {
    int color;
    char colorstr[8];
    PCRE2_SIZE clen = sizeof colorstr;

    pcre2_substring_copy_bynumber(md, n, (PCRE2_UCHAR *) colorstr, &clen);
    color = parse_integer(colorstr);
    if (color > 255) {
      return 0;
    }
    safe_hexchar(color, rgbs, &rgbsp);
  }
  rgbs[6] = '\0';

  return 1;
}

/* Look for a / or end of string */
static const char *
find_end_of_color(const char *s, bool angle)
{
  while (*s && *s != '/' && *s != TAG_END && *s != '!' &&
         (angle ? *s != '>' : !isspace(*s)))
    s += 1;
  if (angle && *s && *s == '>')
    s += 1;
  return s;
}

/** Populate an ansi_data struct.
 * \param store pointer to an ansi_data struct to populate.
 * \param str   String to populate it using.
 * \retval 0 success
 * \retval 1 failure.
 */
int
define_ansi_data(ansi_data *store, const char *str)
{
  const char *name;
  char *ptr = store->fg;
  char buff[BUFFER_LEN];
  int len;
  bool new_ansi = false;

  memset(store, 0, sizeof(ansi_data));

  while (str && *str && *str != TAG_END) {
    while (*str && isspace(*str)) {
      str++;
      new_ansi = false;
    }
    if (!*str)
      break;
    if (new_ansi) {
      /* *str is either +, #, <, 0-9 or / */
      switch (*str) {
      case '+':
        /* Color names. */
        name = str + 1;
        str = find_end_of_color(str, 0);
        len = str - name;
        /* len will always be less than BUFFER_LEN */
        memcpy(buff, name, len);
        buff[len] = '\0';
        len = remove_trailing_whitespace(buff, len);

        if (!valid_color_name(buff)) {
          return 1;
        }

        if (strncasecmp("xterm", buff, 5) ==
            0) { /* xterm color ids are stored directly. */
          snprintf(ptr, COLOR_NAME_LEN, "+%.8s", buff);
        } else if (len > 6) { /* Use hex code to save on buffer space */
          char cbuff[BUFFER_LEN + 1];
          snprintf(cbuff, sizeof cbuff, "+%s", buff);
          snprintf(ptr, COLOR_NAME_LEN, "#%06x", color_to_hex(cbuff, 0));
        } else /* len <= 6 */ {
          snprintf(ptr, COLOR_NAME_LEN, "+%.6s", buff);
        }

        break;
      case '#':
        /* Hex colors. */
        str += 1;
        name = str;
        str = find_end_of_color(str, 0);
        len = str - name;
        memcpy(buff, name, len);
        buff[len] = '\0';
        len = remove_trailing_whitespace(buff, len);
        if (len != 6) /* Only accept 24-bit colors */
          return 1;
        if (!valid_hex_digits(buff, len))
          return 1;
        snprintf(ptr, COLOR_NAME_LEN, "#%.6s", buff);
        break;
      case '<':
        /* <#RRGGBB> or <R G B> */
        {
          char rgbs[7];

          name = str;
          str = find_end_of_color(str, 1);
          len = str - name;
          if (valid_angle_hex(name, len)) {
            /* < #RRGGBB > */
            char *st = strchr(name, '#');
            mush_strncpy(ptr, st, 8);
          } else if (valid_angle_triple(name, len, rgbs)) {
            /* < R G B > */
            snprintf(ptr, COLOR_NAME_LEN, "#%s", rgbs);
          } else
            return 1;
        }
        break;
      case '0':
        if (*(str + 1) && (*(str + 1) == 'X' || *(str + 1) == 'x')) {
          /* 0xRRGGBB, 0xRGB and 0xN where N is a base 16 xterm color id */
          name = str + 2;
          str = find_end_of_color(str, 0);
          len = str - name;
          memcpy(buff, name, len);
          buff[len] = '\0';
          len = remove_trailing_whitespace(buff, len);
          if (!valid_hex_digits(buff, len))
            return 1;
          switch (len) {
          case 1:
          case 2: {
            unsigned int xterm;
            if (sscanf(buff, "%x", &xterm) != 1)
              return 1;
            snprintf(ptr, COLOR_NAME_LEN, "+xterm%u", xterm);
          } break;
          case 3: {
            unsigned int r, g, b;
            if (sscanf(buff, "%1x%1x%1x", &r, &g, &b) != 3)
              return 1;
            snprintf(ptr, COLOR_NAME_LEN, "#%02x%02x%02x", r, g, b);
          } break;
          case 6: {
            unsigned int r, g, b;
            if (sscanf(buff, "%2x%2x%2x", &r, &g, &b) != 3)
              return 1;
            snprintf(ptr, COLOR_NAME_LEN, "#%02x%02x%02x", r, g, b);
          } break;
          default:
            return 1;
          }
          break;
        }
      /* fall through - on other numbers starting with 0 */
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        name = str;
        str = find_end_of_color(str, 0);
        len = str - name;
        memcpy(buff, name, len);
        buff[len] = '\0';
        len = remove_trailing_whitespace(buff, len);
        if (is_strict_integer(buff)) {
          int xterm = parse_integer(buff);
          if (xterm < 0 || xterm > 255)
            return 1;
          snprintf(ptr, COLOR_NAME_LEN, "+xterm%d", xterm);
          break;
        } else
          return 1;
      case '/':
      case '!':
        ptr = store->bg;
        str++;
        break;
      default:
        return 1;
      }
    } else {
      /* old style ANSI codes */
      switch (*str) {
      case 'n': /* normal */
        /* This is explicitly normal, it'll never be colored */
        store->bits = 0;
        store->offbits = ~0;
        store->fg[0] = 'n';
        store->fg[1] = '\0';
        store->bg[0] = '\0';
        break;
      case 'f': /* flash */
        store->bits |= CBIT_FLASH;
        store->offbits &= ~CBIT_FLASH;
        break;
      case 'h': /* hilite */
        store->bits |= CBIT_HILITE;
        store->offbits &= ~CBIT_HILITE;
        break;
      case 'i': /* inverse */
        store->bits |= CBIT_INVERT;
        store->offbits &= ~CBIT_INVERT;
        break;
      case 'u': /* underscore */
        store->bits |= CBIT_UNDERSCORE;
        store->offbits &= ~CBIT_UNDERSCORE;
        break;
      case 'F': /* flash */
        store->offbits |= CBIT_FLASH;
        store->bits &= ~CBIT_FLASH;
        break;
      case 'H': /* hilite */
        store->offbits |= CBIT_HILITE;
        store->bits &= ~CBIT_HILITE;
        break;
      case 'I': /* inverse */
        store->offbits |= CBIT_INVERT;
        store->bits &= ~CBIT_INVERT;
        break;
      case 'U': /* underscore */
        store->offbits |= CBIT_UNDERSCORE;
        store->bits &= ~CBIT_UNDERSCORE;
        break;
      case 'b': /* blue fg */
      case 'c': /* cyan fg */
      case 'g': /* green fg */
      case 'm': /* magenta fg */
      case 'r': /* red fg */
      case 'w': /* white fg */
      case 'x': /* black fg */
      case 'y': /* yellow fg */
      case 'd': /* default fg */
        store->fg[0] = *str;
        store->fg[1] = '\0';
        break;
      case 'B': /* blue bg */
      case 'C': /* cyan bg */
      case 'G': /* green bg */
      case 'M': /* magenta bg */
      case 'R': /* red bg */
      case 'W': /* white bg */
      case 'X': /* black bg */
      case 'Y': /* yellow bg */
      case 'D': /* default bg */
        store->bg[0] = *str;
        store->bg[1] = '\0';
        break;
      case '#':
      case '+':
      case '/':
      case '<':
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '!':
        new_ansi = 1;
        ptr = store->fg;
      }
      if (!new_ansi)
        str++;
    }
  }
  return 0;
}

int
read_raw_ansi_data(ansi_data *store, const char *codes)
{
  int curnum;
  if (!codes || !store)
    return 0;
  store->bits = 0;
  store->offbits = 0;
  store->fg[0] = 0;
  store->bg[0] = 0;

  /* 'codes' can point at either the ESC_CHAR,
   * the '[', or the following byte. */

  /* Skip to the first ansi number */
  while (*codes && !isdigit(*codes) && *codes != 'm')
    codes++;

  while (*codes && (*codes != 'm')) {
    curnum = atoi(codes);
    if (curnum < 10) {
      switch (curnum) {
      case COL_HILITE:
        store->bits |= CBIT_HILITE;
        store->offbits &= ~CBIT_HILITE;
        break;
      case COL_UNDERSCORE:
        store->bits |= CBIT_UNDERSCORE;
        store->offbits &= ~CBIT_UNDERSCORE;
        break;
      case COL_FLASH:
        store->bits |= CBIT_FLASH;
        store->offbits &= ~CBIT_FLASH;
        break;
      case COL_INVERT:
        store->bits |= CBIT_INVERT;
        store->offbits &= ~CBIT_INVERT;
        break;
      case COL_NORMAL:
        store->bits = 0;
        store->offbits = ~0;
        store->fg[0] = 'n';
        store->fg[1] = 0;
        store->bg[0] = 0;
        break;
      }
    } else if (curnum == 38 || curnum == 48) {
      bool fg = (curnum == 38);
      /* XTERM. Bah. */
      while (*codes && isdigit(*codes))
        codes++;
      if (*codes && *codes == ';') {
        codes++;
        curnum = atoi(codes);
      } else {
        curnum = -1;
      }
      if (curnum != 5) {
        /* Something is wrong; abort */
        while (*codes && (*codes != 'm'))
          codes++;
        return 0; /* Should this return 1 or 0? Who knows */
      }
      codes++; /* skip over the '5' */
      if (*codes && *codes == ';') {
        codes++;
        curnum = atoi(codes);
      } else {
        curnum = -1;
      }
      if (curnum < 0 || curnum > 255) {
        /* Something is wrong; abort */
        while (*codes && (*codes != 'm'))
          codes++;
        return 0; /* Should this return 1 or 0? Who knows */
      }
      snprintf((fg ? store->fg : store->bg), COLOR_NAME_LEN, "+xterm%d",
               curnum);
    } else if (curnum < 40) {
      store->fg[0] = ansi_chars[curnum];
      store->fg[1] = 0;
    } else if (curnum < 50) {
      store->bg[0] = ansi_chars[curnum];
      store->bg[1] = 0;
    }
    /* Skip current and find the next ansi number */
    while (*codes && isdigit(*codes))
      codes++;
    while (*codes && !isdigit(*codes) && (*codes != 'm'))
      codes++;
  }
  return 1;
}

/** Return a string pointer past any ansi/html markup at the start.
 * \param p a string.
 * \param bound if non-NULL, don't proceed past bound. bound must point to
 * somewhere in the string.
 * \return pointer to string after any initial ansi/html markup.
 */

char *
skip_leading_ansi(const char *p, const char *bound)
{
  if (!p)
    return NULL;
  while ((*p == ESC_CHAR || *p == TAG_START) && (!bound || p < bound)) {
    if (*p == ESC_CHAR) {
      while (*p && *p != 'm' && (!bound || p <= bound))
        p++;
    } else { /* TAG_START */
      while (*p && *p != TAG_END && (!bound || p <= bound))
        p++;
    }
    if (*p)
      p++;
  }
  if (bound && p > bound)
    return NULL;
  return (char *) p;
}

/** Does a string contain markup? */
int
has_markup(const char *test)
{
  /* strtok modifies, so we don't use it. */
  return (strchr(test, ESC_CHAR) || strchr(test, TAG_START) ||
          strchr(test, TAG_END));
}

/** Extract the HTML tag name from a Pueblo markup block */
static char *
parse_tagname(const char *ptr)
{
  static char tagname[BUFFER_LEN];
  char *tag = tagname;
  if (!ptr || !*ptr)
    return NULL;
  while (*ptr && !isspace(*ptr) && *ptr != TAG_END) {
    *(tag++) = *(ptr++);
  }
  *tag = '\0';
  return tagname;
}

static const char *
as_get_tag(ansi_string *as, const char *tag)
{
  if (!tag)
    return NULL;

  if (*tag == '/' && *(tag + 1) == '\0') {
    return "/";
  }
  if (as->tags == NULL) {
    as->tags = mush_malloc(sizeof(StrTree), "ansi_string.tags");
    st_init(as->tags, "ansi_string.tags");
  }
  return st_insert(tag, as->tags);
}

/** Make sure an ansi_string has room for one more markup_information, and
 *  return its index. */
static new_markup_information *
grow_mi(ansi_string *as, char type)
{
  if (as->micount >= as->misize) {
    if (as->mi == NULL) {
      as->misize = 30;
      as->mi = mush_calloc(as->misize, sizeof(new_markup_information),
                           "ansi_string.mi");
    } else {
      as->misize *= 2;
      as->mi = mush_realloc(as->mi, as->misize * sizeof(new_markup_information),
                            "ansi_string.mi");
    }
  }
  memset(&as->mi[as->micount], 0, sizeof(new_markup_information));
  as->mi[as->micount].parentIdx = NOMARKUP;
  as->mi[as->micount].idx = as->micount;
  as->mi[as->micount].type = type;
  return &as->mi[as->micount++];
}

static inline new_markup_information *
MI_FOR(ansi_string *as, int idx)
{
  if (idx < 0 || idx > as->misize)
    return NULL;
  else
    return &as->mi[idx];
}

static char *colend = "/";

/** Convert a string into an ansi_string.
 * This takes a string that may contain ansi/html markup codes and
 * converts it to an ansi_string structure that separately stores
 * the plain string and the markup codes for each character.
 * \param source string to parse.
 * \return pointer to an ansi_string structure representing the src string.
 */
ansi_string *
parse_ansi_string(const char *source)
{
  ansi_string *as;
  int c;
  char *s;
  char *tag, type;
  int len;

  /* For stacking information. */
  new_markup_information *mi = NULL;
  new_markup_information *mip = NULL;
  int idx = NOMARKUP;
  int pidx = NOMARKUP;

  if (!source) {
    return NULL;
  }

  /* Allocate and zero it out. */
  as = mush_calloc(1, sizeof(ansi_string), "ansi_string");

  /* Quick check for no markup */
  if (!has_markup(source)) {
    as->len = strlen(source);
    if (as->len >= BUFFER_LEN - 1) {
      as->len = BUFFER_LEN - 1;
    }
    strncpy(as->text, source, as->len);
    return as;
  }
  as->source = mush_strdup(source, "ansi_string.source");

  /* The string has markup. Nuts. */
  as->flags |= AS_HAS_MARKUP;
  as->markup = mush_calloc(BUFFER_LEN, sizeof(uint32_t), "ansi_string.markup");

  c = 0;
  for (s = as->source; *s;) {
    switch (*s) {
    case TAG_START:
      s++;
      tag = s;
      while (*s && *s != TAG_END) {
        s++;
      }
      if (*s)
        *(s++) = '\0';

      /* <tag> contains the entire tag, now. */
      if (!*tag)
        break;
      type = *(tag++);
      if (!*tag)
        break;
      switch (type) {
      case MARKUP_COLOR:
        if (*tag != '/') {
          /* Start tag */
          pidx = idx;
          mi = grow_mi(as, MARKUP_COLOR);
          mi->start_code = as_get_tag(as, tag);
          mi->end_code = colend;
          mi->parentIdx = pidx;
          idx = mi->idx;
        } else {
          /* End tag */
          if (*(tag + 1) == 'a') {
            if (AS_HasTags(as)) {
              /* Color endall in a pueblo tag. Blah, we should
               * never see this in actual use. So let's just pretend
               * it's an end-all-tags until somebody complains. */
            }
            mi = NULL;
            idx = NOMARKUP;
          } else {
            if (mi) {
              /* Close all tags above the latest color tag and mark them
               * as standalone. Anybody who complains about it closing
               * overlapping pueblo tags can learn how to code nicely. The only
               * time this should close non-color tags is when they do
               * something silly like:
               *
               *   ansi(r,foo[tag(SAMP)]bar) hello [ansi(g,tag(/SAMP))].
               *
               * Use of tagwrap and responsible use of tag means this
               * won't happen. */
              for (; mi && mi->type != MARKUP_COLOR;
                   mi = MI_FOR(as, mi->parentIdx)) {
                mi->end_code = NULL;
                mi->standalone = 1;
                as->flags |= AS_HAS_STANDALONE;
              }
              if (mi) {
                idx = mi->parentIdx;
                mi = MI_FOR(as, idx);
              }
            }
          }
        }
        break;
      default:
        if (*tag != '/') {
          /* Start tag */
          as->flags |= AS_HAS_TAGS;
          pidx = idx;
          mi = grow_mi(as, type);
          mi->start_code = as_get_tag(as, tag);
          mi->parentIdx = pidx;
          mi->start = c;
          idx = mi->idx;
        } else {
          /* End tag */
          if (mi) {
            tag++;
            len = strlen(tag);
            /* Find the tag that this closes. */
            for (mip = mi; mip; mip = MI_FOR(as, mip->parentIdx)) {
              if ((mip->type == type) &&
                  (!strncasecmp(mip->start_code, tag, len)) &&
                  ((mip->start_code[len] == ' ') ||
                   (mip->start_code[len] == '\0'))) {
                break;
              }
            }
            if (mip) {
              /* Close the stack of stuff above mip. All non-'c' types
               * are standalones. all C types are force-closed. Because,
               * y'know, people really shouldn't be using overlapping
               * tags. */
              for (; mi != mip; mi = MI_FOR(as, mi->parentIdx)) {
                if (mi->type != MARKUP_COLOR) {
                  mi->end_code = NULL;
                  mi->standalone = 1;
                  as->flags |= AS_HAS_STANDALONE;
                }
              }
              tag--;
              mip->end_code = as_get_tag(as, tag);
              idx = mip->parentIdx;
              mi = MI_FOR(as, idx);
            } else {
              /* Yes, goto is useful here. Yes it is. Shut up, it is. */
              goto standalone_end;
            }
          } else {
          standalone_end:
            /* Standalone end tag?! Lame. We turn it into a start tag
             * and attach it to the next character. */
            as->flags |= AS_HAS_TAGS;
            as->flags |= AS_HAS_STANDALONE;
            pidx = idx;
            mi = grow_mi(as, type);
            mi->end_code = mi->start_code = as_get_tag(as, tag);
            mi->parentIdx = pidx;
            mi->start = c;
            mi->standalone = 1;
            idx = mi->idx;
          }
        }
        break;
      case '\0':
        /* Do nothing: Empty tag?! We'll shove it under the carpet
         * and forget about it. */
        break;
      }
      break;
    case ESC_CHAR:
      /* Here's what I'm going to do: An ESC_CHAR is an old escape code,
       * so we'll treat it as if it's a standalone tag. It's a MARKUP_OLDANSI
       * tag, which receives special handling. */
      pidx = idx;
      mi = grow_mi(as, MARKUP_OLDANSI);
      *(s++) = '\0';
      mi->start_code = s;
      mi->standalone = 1;
      /* Find the end of the ansi code, or series of ansi codes. */
      while (*s) {
        if (*s == 'm' && *(s + 1) != ESC_CHAR)
          break;
        s++;
      }
      if (*s)
        *(s++) = '\0';
      mi->start_code = as_get_tag(as, mi->start_code);
      mi->end_code = NULL;
      mi->parentIdx = pidx;
      idx = mi->idx;
      break;
    default:
      as->text[c] = *s;
      as->markup[c] = idx;
      c++;
      s++;
      while (idx >= 0 && as->mi[idx].standalone) {
        idx = as->mi[idx].parentIdx;
      }
      mi = MI_FOR(as, idx);
    }
  }
  as->len = c;
  if (mi) {
    for (; mi; mi = MI_FOR(as, mi->parentIdx)) {
      if (mi->type != MARKUP_COLOR) {
        /* Turn this tag into a standalone. */
        mi->standalone = 1;
        as->flags |= AS_HAS_STANDALONE;
      }
    }
  }
  if (as->flags & AS_HAS_STANDALONE ||
      (as->mi && as->mi[as->micount - 1].start == as->len)) {
    /* If there are any markup tags at the very end (start == as->len),
     * then we have to move them forward, change start to end code, and
     * advance them. Unless length is 0, in which case the only thing this
     * string has is a standalone tag. Ew.  */
    if (as->len > 0) {
      if (as->mi[as->micount - 1].start == as->len) {
        /* Attach to the last character's markup. */
        pidx = as->markup[as->len - 1];
        for (idx = pidx + 1; idx < as->micount; idx++) {
          if (as->mi[idx].start == as->len &&
              as->mi[idx].type != MARKUP_COLOR) {
            as->flags |= AS_HAS_STANDALONE;
            as->mi[idx].end_code = as->mi[idx].start_code;
            as->mi[idx].start_code = NULL;
            as->mi[idx].standalone = 1;
            pidx = idx;
          }
        }
        as->markup[as->len - 1] = pidx;
      }
    }
  }
  return as;
}

/** Free an ansi_string.
 * \param as pointer to ansi_string to free.
 */
void
free_ansi_string(ansi_string *as)
{
  if (!as)
    return;

  if (as->source) {
    mush_free(as->source, "ansi_string.source");
  }
  if (as->tags) {
    st_flush(as->tags);
    mush_free(as->tags, "ansi_string.tags");
  }
  if (as->markup) {
    mush_free(as->markup, "ansi_string.markup");
  }
  if (as->mi) {
    mush_free(as->mi, "ansi_string.mi");
  }

  mush_free(as, "ansi_string");
}

/* Copy the start code for a particular markup_info */
static int
safe_start_code(new_markup_information *info, char *buff, char **bp)
{
  int retval = 0;
  char *save;
  save = *bp;
  if (info && info->start_code) {
    if (info->type == MARKUP_OLDANSI) {
      retval += safe_chr(ESC_CHAR, buff, bp);
      retval += safe_str(info->start_code, buff, bp);
      retval += safe_chr('m', buff, bp);
    } else {
      retval += safe_chr(TAG_START, buff, bp);
      retval += safe_chr(info->type, buff, bp);
      retval += safe_str(info->start_code, buff, bp);
      retval += safe_chr(TAG_END, buff, bp);
    }
  }
  if (retval)
    *bp = save;
  return retval;
}

/* Copy the stop code for a particular markup_info */
static int
safe_end_code(new_markup_information *info, char *buff, char **bp)
{
  int retval = 0;
  char *save;
  save = *bp;
  if (info && info->end_code) {
    if (info->type == MARKUP_OLDANSI) {
      retval += safe_chr(ESC_CHAR, buff, bp);
      retval += safe_str(info->end_code, buff, bp);
      retval += safe_chr('m', buff, bp);
    } else {
      retval += safe_chr(TAG_START, buff, bp);
      retval += safe_chr(info->type, buff, bp);
      retval += safe_str(info->end_code, buff, bp);
      retval += safe_chr(TAG_END, buff, bp);
    }
  }
  if (retval)
    *bp = save;
  return retval;
}

/** Reverse an ansi string, preserving its ansification.
 * This function destructively modifies the ansi_string passed.
 * \param as pointer to an ansi string.
 */
void
flip_ansi_string(ansi_string *as)
{
  int s;
  int e;
  char tmp;
  uint32_t mitmp;

  for (s = 0, e = as->len - 1; s < e; s++, e--) {
    tmp = as->text[s];
    as->text[s] = as->text[e];
    as->text[e] = tmp;
    if (as->markup) {
      mitmp = as->markup[s];
      as->markup[s] = as->markup[e];
      as->markup[e] = mitmp;
    }
  }
}

/** Delete a portion of an ansi string.
 * \param as ansi_string to delete from
 * \param start start point to remove
 * \param count length of string to remove
 * \retval 0 success
 * \retval 1 failure.
 */
int
ansi_string_delete(ansi_string *as, int start, int count)
{
  int s, c, l;
  int i;
  if (count < 1)
    return 0;
  if (start > as->len)
    return 1;
  if ((start + count) > as->len) {
    count = (as->len - start);
  }
  if (count < 1)
    return 1;
  /* Move text left */
  s = start;
  c = start + count;
  l = as->len - c;
  memmove(as->text + s, as->text + c, l);
  /* Move markup left. */
  if (as->markup) {
    l *= sizeof(uint32_t);
    memmove(as->markup + s, as->markup + c, l);
  }
  if (as->flags & AS_HAS_STANDALONE) {
    /* If we have standalone markup, move the start. */
    for (i = 0; i < as->micount; i++) {
      if (as->mi[i].start > c) {
        as->mi[i].start -= count;
      }
    }
  }
  as->len -= count;
  as->text[as->len] = '\0';
  return 0;
}

/** Insert an ansi string into another ansi_string
 * with markups kept as straight as possible.
 * \param dst ansi_string to insert into.
 * \param loc Location to insert into, 0-indexed
 * \param src ansi_string to insert
 * \retval 0 success
 * \retval 1 failure.
 */
int
ansi_string_insert(ansi_string *dst, int loc, ansi_string *src)
{
  return ansi_string_replace(dst, loc, 0, src);
}

/** Replace a portion of an ansi string with
 *  another ansi string, keeping markups as
 *  straight as possible.
 * \param dst ansi_string to insert into.
 * \param loc Location to  insert into, 0-indexed
 * \param count Length of string inside dst to replace
 * \param src ansi_string to insert
 * \retval 0 success
 * \retval 1 failure.
 */
int
ansi_string_replace(ansi_string *dst, int loc, int count, ansi_string *src)
{
  int len, oldlen, srclen, srcend, dstleft;
  int idx, sidx, baseidx;
  int i, j;
  int truncated = 0;
  new_markup_information *basemi, *mis, *mi, *mie;

  oldlen = dst->len;
  srclen = src->len;

  if (loc > oldlen) {
    /* If the dst string isn't long enough, we don't replace, we just
     * insert at the end of the existing string */
    loc = oldlen;
    count = 0;
  }

  if (loc + count > oldlen)
    count = oldlen - loc;

  srcend = loc + srclen;
  len = oldlen + srclen;

  dstleft = oldlen - (loc + count);
  len -= count;

  if (len >= BUFFER_LEN) {
    if (loc >= BUFFER_LEN - 1) {
      return 1;
    }
    len = BUFFER_LEN - 1;
    truncated = 1;
    if (srcend >= BUFFER_LEN) {
      srcend = BUFFER_LEN - 1;
      srclen = len - loc;
      dstleft = 0;
    } else {
      dstleft = len - srcend;
    }
  }

  /* Nothing to copy? */
  if (src->len < 1) {
    if (count > 0) {
      ansi_string_delete(dst, loc, count);
    }
    if (src->markup && src->flags & AS_HAS_STANDALONE) {
      dst->flags |= AS_HAS_STANDALONE;
      /* Special case: src has only standalone tags. */
      if (!dst->markup) {
        dst->markup =
          mush_calloc(BUFFER_LEN, sizeof(uint32_t), "ansi_string.markup");
        for (i = 0; i < dst->len; i++) {
          dst->markup[i] = NOMARKUP;
        }
        dst->flags |= AS_HAS_MARKUP;
      }
      /* Add the incoming markup, but only the standalone. */
      baseidx = NOMARKUP;
      idx = NOMARKUP;
      for (sidx = 0; sidx < src->micount; sidx++) {
        if (!src->mi[sidx].standalone)
          continue;
        mi = grow_mi(dst, src->mi[sidx].type);
        mi->start_code = as_get_tag(dst, src->mi[sidx].start_code);
        mi->end_code = as_get_tag(dst, src->mi[sidx].end_code);
        mi->standalone = 1;
        mi->start = loc;

        mi->parentIdx = idx;
        if (baseidx < 0)
          baseidx = mi->idx;
        idx = mi->idx;
      }
      /* Now integrate them into the proper location */
      if (baseidx >= 0) {
        if (loc <= (dst->len - 1)) {
          /* Add the incoming markup to the character at dst->markup[loc] */
          dst->mi[baseidx].parentIdx = dst->markup[loc];
          dst->markup[loc] = idx;
        } else if (dst->len > 0) {
          dst->mi[baseidx].parentIdx = dst->markup[dst->len - 1];
          dst->markup[dst->len - 1] = idx;
          /* Now ensure all start tags are end tags */
          while (baseidx <= idx) {
            if (dst->mi[baseidx].start_code) {
              dst->mi[baseidx].end_code = dst->mi[baseidx].start_code;
              dst->mi[baseidx].start_code = NULL;
            }
            baseidx++;
          }
        }
      }
      return 0;
      /* If dst->len == 0, then it's just an empty string with standalone
       * markup. */
    } else {
      return 0;
    }
    return 1;
  }

  /* Move the text over. */
  if (dstleft > 0) {
    memmove(dst->text + srcend, dst->text + loc + count, dstleft);
  }

  /* Copy src over */
  memcpy(dst->text + loc, src->text, srclen);
  dst->len = len;
  dst->text[len] = '\0';

  /* If there's no markup, we're done. */
  if (!(src->markup || dst->markup)) {
    return truncated;
  }

  /* In case of copying from marked up string to non-marked-up. */
  if (!dst->markup) {
    dst->markup =
      mush_calloc(BUFFER_LEN, sizeof(uint32_t), "ansi_string.markup");
    for (i = 0; i < len; i++) {
      dst->markup[i] = NOMARKUP;
    }
    dst->flags |= AS_HAS_MARKUP;
  }

  /* Save the markup info pointers for loc and loc-1 */
  mis = NULL;
  mie = NULL;
  if (count == 0) {
    if (loc > 0 && dst->markup[loc - 1] >= 0) {
      if (dst->markup[loc] >= 0)
        mis = &dst->mi[dst->markup[loc - 1]];
    }
    if (dst->markup[loc] >= 0) {
      if (dst->markup[loc] >= 0)
        mie = &dst->mi[dst->markup[loc]];
    }
  } else {
    i = loc;
    if (i <= oldlen) {
      if (dst->markup[i] >= 0)
        mis = &dst->mi[dst->markup[i]];
    }
    i = loc + count - 1;
    if (i <= oldlen) {
      if (dst->markup[i] >= 0)
        mie = &dst->mi[dst->markup[i]];
    }
  }

  /* Move markup as necessary. */
  if (dstleft > 0) {
    memmove(dst->markup + srcend, dst->markup + (loc + count),
            dstleft * sizeof(int16_t));
  }

  /* If, and only if, mis and mie have a markup_information in common,
   * use that as basemi for the _entire_ inserted string. */
  basemi = NULL;
  if (mis && mie) {
    while (mie) {
      basemi = mis;
      while (basemi) {
        if (basemi->idx == mie->idx) {
          break;
        }
        basemi = MI_FOR(dst, basemi->parentIdx);
      }
      if (basemi)
        break;
      mie = MI_FOR(dst, mie->parentIdx);
    }
    /* basemi is either NULL or set at this point. */
  }
  baseidx = NOMARKUP;
  if (basemi) {
    baseidx = basemi->idx;
  }

  /* Copy the markup info of src over. */
  idx = dst->micount;
  if (src->markup) {
    for (sidx = 0; sidx < src->micount; sidx++) {
      mi = grow_mi(dst, src->mi[sidx].type);
      mi->start_code = as_get_tag(dst, src->mi[sidx].start_code);
      mi->end_code = as_get_tag(dst, src->mi[sidx].end_code);
      mi->standalone = src->mi[sidx].standalone;
      mi->start = src->mi[sidx].start + loc;
      if (src->mi[sidx].parentIdx >= 0) {
        mi->parentIdx = src->mi[sidx].parentIdx + idx;
      } else {
        mi->parentIdx = baseidx;
      }
    }

    /* Copy src's markup over, updating to new idx. */
    memcpy(dst->markup + loc, src->markup, srclen * sizeof(uint32_t));
    for (i = loc, j = 0; i < srcend; i++, j++) {
      if (src->markup[j] >= 0) {
        dst->markup[i] = src->markup[j] + idx;
      } else {
        dst->markup[i] = baseidx;
      }
    }
  } else {
    for (i = loc; i < srcend; i++) {
      if ((i - loc) > (count - 1))
        dst->markup[i] = (count || (loc > 0 && loc < oldlen))
                           ? dst->markup[loc + count - 1]
                           : NOMARKUP;
    }
  }
  return truncated;
}

/** Scrambles an ansi_string in place.
 */
void
scramble_ansi_string(ansi_string *as)
{
  int i, j;
  char tmp;
  uint32_t idxtmp;

  for (i = 0; i < as->len; i++) {
    j = get_random_u32(0, as->len - 1);
    tmp = as->text[i];
    as->text[i] = as->text[j];
    as->text[j] = tmp;
    if (as->markup) {
      idxtmp = as->markup[i];
      as->markup[i] = as->markup[j];
      as->markup[j] = idxtmp;
    }
  }
}

/** Safely append markup tags onto a buffer
 * \param mi markup_information to write
 * \param end If true, end_code. Otherwise start_code.
 * \param buff buffer to write to
 * \param bp where to write to
 * \retval 0 safely written
 * \retval 1 unable to safely write.
 */
int
safe_markup_codes(new_markup_information *mi, int end, char *buff, char **bp)
{
  if (end) {
    if (mi->end_code)
      return safe_str(mi->end_code, buff, bp);
  } else {
    if (mi->start_code)
      return safe_str(mi->start_code, buff, bp);
  }
  return 0;
}

/** Safely append markup changes between one idx and the other
 * \param as the ansi string
 * \param lastidx idx to close
 * \param nextidx idx to open
 * \param buff buffer to write to
 * \param bp where to write to
 * \retval 0 safely written
 * \retval 1 unable to safely write.
 */
static int
safe_markup_change(ansi_string *as, int lastidx, int nextidx, int pos,
                   char *buff, char **bp)
{
  new_markup_information *lastmi, *nextmi;
  new_markup_information *mil = NULL, *mir = NULL;
  int i = 0;
  new_markup_information *endbuff[BUFFER_LEN];
  bool right_side = 0;

  if (lastidx >= 0) {
    lastmi = &as->mi[lastidx];
  } else {
    lastmi = NULL;
  }

  if (nextidx >= 0) {
    nextmi = &as->mi[nextidx];
  } else {
    nextmi = NULL;
  }

  /* dump closing tags for that which is in mil that isn't in mir. */
  /* Look for the highest mil that exists in mir. */
  for (mil = lastmi; mil; mil = MI_FOR(as, mil->parentIdx)) {
    for (mir = nextmi; mir; mir = MI_FOR(as, mir->parentIdx)) {
      if (mil == mir)
        break;
    }
    if (mir)
      break;
  }
  /* Dump the end codes for everything from lastmi down to mil. */
  for (; lastmi && lastmi != mil; lastmi = MI_FOR(as, lastmi->parentIdx)) {
    if (safe_end_code(lastmi, buff, bp))
      return 1;
  }
  /* Now we do the start codes for everything on the right. We have to
   * do this from the bottom of the stack (or rmi)-up, though. */
  for (i = 0; nextmi && nextmi != mir;
       nextmi = MI_FOR(as, nextmi->parentIdx), i += 1) {
    endbuff[i] = nextmi;
    right_side = 1;
  }
  if (right_side) {
    while (i--) {
      if (!(endbuff[i]->standalone && pos != endbuff[i]->start)) {
        if (safe_start_code(endbuff[i], buff, bp))
          return 1;
      }
    }
  }
  return 0;
}

/** Sanitize an @moniker string, by removing any Pueblo,
 * flashing ANSI or underline ANSI.
 * \param input the string to sanitize.
 * \param buff buffer to write sanitized output to
 * \param bp position in buffer to write to
 */
void
sanitize_moniker(char *input, char *buff, char **bp)
{
  char orig[BUFFER_LEN];
  char *p, *colstr;
  bool in_markup = false;

  /* So we can destructively modify it safely */
  strcpy(orig, input);

  for (p = orig; *p; p++) {
    if (*p == TAG_START) {
      p++;
      if (!*p)
        break;
      if (*p == MARKUP_COLOR) {
        ansi_data ad;
        *p++ = '\0';
        colstr = p;
        while (*p && *p != TAG_END)
          p++;
        *p = '\0';
        if (*colstr == '/') {
          if (in_markup) {
            write_ansi_close(buff, bp);
            in_markup = 0;
          }
        } else {
          define_ansi_data(&ad, colstr);
          ad.bits &= ~(CBIT_FLASH | CBIT_UNDERSCORE);
          if (HAS_ANSI(ad)) {
            write_ansi_data(&ad, buff, bp);
            in_markup = 1;
          } else
            in_markup = 0;
        }
      } else {
        /* HTML */
        while (*p && *p != TAG_END)
          p++;
      }
    } else
      safe_chr(*p, buff, bp);
  }
}

/** Safely append an ansi_string into a buffer as a real string,
 * \param as pointer to ansi_string to append.
 * \param start position in as to start copying from.
 * \param len length in characters to copy from as.
 * \param buff buffer to insert into.
 * \param bp pointer to pointer to insertion point of buff.
 * \retval 0 success.
 * \retval 1 failure.
 */
int
safe_ansi_string(ansi_string *as, int start, int len, char *buff, char **bp)
{
  int i;
  int end;
  int retval = 0;
  int lastidx;
  char *buffend = buff + BUFFER_LEN - 1;

  if (!as)
    return 0;

  if (start == 0 && as->len == 0 && (as->flags & AS_HAS_STANDALONE)) {
    for (i = 0; i < as->micount; i++) {
      if (!as->mi[i].standalone)
        continue;
      if (as->mi[i].start_code) {
        safe_start_code(&as->mi[i], buff, bp);
      }
      if (as->mi[i].end_code) {
        safe_end_code(&as->mi[i], buff, bp);
      }
    }
  }
  if (start >= as->len || start < 0 || len < 1) {
    return 0;
  }

  if (start + len >= as->len) {
    len = (as->len - start);
  }

  /* Quick check: If no markup, no markup =). */
  if (!(as->flags & AS_HAS_MARKUP)) {
    return safe_strl(as->text + start, len, buff, bp);
  }

  end = start + len;

  lastidx = NOMARKUP;

  /* The string has markup. Let's dump it. */
  for (i = start; i < end;) {
    while (lastidx == as->markup[i] && (i < end) && ((*bp) < buffend)) {
      *((*bp)++) = as->text[i++];
    }
    if ((*bp) >= buffend) {
      return 1;
    }
    if (i < end) {
      if (lastidx != as->markup[i]) {
        if (safe_markup_change(as, lastidx, as->markup[i], i, buff, bp)) {
          return 1;
        }
        lastidx = as->markup[i];
      }
    } else if (lastidx != NOMARKUP) {
      if (safe_markup_change(as, lastidx, NOMARKUP, i, buff, bp)) {
        return 1;
      }
    }
  }
  return retval;
}

/* Following functions are used for
 * decompose_str()
 */

extern char escaped_chars[UCHAR_MAX + 1];

static int
escape_marked_str(char **str, char *buff, char **bp)
{
  char *in;
  int retval = 0;
  int dospace = 1;
  int spaces = 0;
  int i;

  if (!str || !*str || !**str)
    return 0;
  in = *str;
  for (; *in && *in != ESC_CHAR && *in != TAG_START; in++) {
    if (*in == ' ') {
      spaces++;
    } else {
      if (spaces) {
        if (spaces >= 5) {
          retval += safe_str("[space(", buff, bp);
          retval += safe_number(spaces, buff, bp);
          retval += safe_str(")]", buff, bp);
        } else {
          if (dospace) {
            spaces--;
            retval += safe_str("%b", buff, bp);
          }
          while (spaces) {
            retval += safe_chr(' ', buff, bp);
            if (--spaces) {
              --spaces;
              retval += safe_str("%b", buff, bp);
            }
          }
        }
      }
      spaces = 0;
      dospace = 0;
      switch (*in) {
      case '\n':
        retval += safe_str("%r", buff, bp);
        break;
      case '\t':
        retval += safe_str("%t", buff, bp);
        break;
      case BEEP_CHAR:
        for (i = 1; *(in + 1) == BEEP_CHAR && i < 5; in++, i++)
          ;
        retval += safe_format(buff, bp, "[beep(%d)]", i);
        break;
      default:
        if (escaped_chars[*in])
          retval += safe_chr('\\', buff, bp);
        retval += safe_chr(*in, buff, bp);
        break;
      }
    }
  }
  if (spaces) {
    if (spaces >= 5) {
      retval += safe_str("[space(", buff, bp);
      retval += safe_number(spaces, buff, bp);
      retval += safe_str(")]", buff, bp);
    } else {
      spaces--; /* This is for the final %b space */
      if (spaces && dospace) {
        spaces--;
        retval += safe_str("%b", buff, bp);
      }
      while (spaces) {
        safe_chr(' ', buff, bp);
        if (--spaces) {
          --spaces;
          retval += safe_str("%b", buff, bp);
        }
      }
      retval += safe_str("%b", buff, bp);
    }
  }
  *str = (char *) in;
  return retval;
}

/* Does the work of decompose_str, which is found in look.c.
 * Even handles ANSI and Pueblo, which is why it's so ugly.
 */
int
safe_decompose_str(char *orig, char *buff, char **bp)
{
  int i;
  char *str = orig;
  char *tmp;
  char *pstr;
  char type;

  ansi_data ansistack[BUFFER_LEN];
  ansi_data oldansi;
  ansi_data tmpansi;
  int ansitop = 0;
  int ansiheight = 0;
  int howmanyopen = 0;
  int oldcodes = 0;

  char *pueblostack[BUFFER_LEN];
  char tagbuff[BUFFER_LEN];
  int pueblotop = -1;

  int retval = 0;

  ansistack[0] = ansi_null;

  if (!str || !*str)
    return 0;

  retval += escape_marked_str(&str, buff, bp);

  while (str && *str && *str != '\0') {
    oldansi = ansistack[ansitop];
    ansiheight = ansitop;
    while (*str == TAG_START || *str == ESC_CHAR) {
      switch (*str) {
      case TAG_START:
        for (tmp = str; *tmp && *tmp != TAG_END; tmp++)
          ;
        if (*tmp) {
          *tmp = '\0';
        } else {
          tmp--;
        }
        str++;
        type = *(str++);
        switch (type) {
        case MARKUP_COLOR:
          if (!*str)
            break;
          if (oldcodes == 1) {
            ansitop--;
            oldcodes = 0;
          }
          /* Start or end tag? */
          if (*str != '/') {
            define_ansi_data(&tmpansi, str);
            nest_ansi_data(&(ansistack[ansitop]), &tmpansi);
            ansitop++;
            ansistack[ansitop] = tmpansi;
          } else {
            if (*(str + 1) == 'a') {
              ansitop = 0;
            } else {
              if (ansitop > 0) {
                ansitop--;
              }
            }
          }
          break;
        case MARKUP_HTML:
          if (!*str)
            break;
          if (*str != '/') {
            pueblotop++;
            snprintf(tagbuff, BUFFER_LEN, "%s", parse_tagname(str));
            pueblostack[pueblotop] = mush_strdup(tagbuff, "markup_code");

            retval += safe_str("[tag(", buff, bp);
            retval += safe_str(tagbuff, buff, bp);
            str += strlen(tagbuff);
            if (str && *str) {
              while (str && str != tmp) {
                str++;
                pstr = strchr(str, '=');
                if (pstr) {
                  *pstr = '\0';
                  retval += safe_chr(',', buff, bp);
                  retval += safe_str(str, buff, bp);
                  retval += safe_chr('=', buff, bp);
                  str = pstr + 1;
                  pstr = strchr(str, '\"');

                  retval += safe_chr('\"', buff, bp);
                  if (str == pstr) {
                    str++;
                    pstr = strchr(str, '\"');
                  } else {
                    pstr = strchr(str, ' ');
                  }

                  if (!pstr)
                    pstr = tmp;

                  *pstr = '\0';
                  retval += safe_str(str, buff, bp);
                  retval += safe_chr('\"', buff, bp);
                  str = pstr;
                } else {
                  safe_str(str, buff, bp);
                  break;
                }
              }
            }
            retval += safe_str(")]", buff, bp);

          } else {
            if (pueblotop > -1) {
              i = (*(str + 1) == 'a') ? 0 : pueblotop;
              for (i--; pueblotop > i; pueblotop--) {
                retval += safe_str("[endtag(", buff, bp);
                retval += safe_str(pueblostack[pueblotop], buff, bp);
                retval += safe_str(")]", buff, bp);
                mush_free(pueblostack[pueblotop], "markup_code");
              }
            }
          }
          break;
        }
        tmp++;
        str = tmp;
        break;
      case ESC_CHAR:
        /* It SHOULD be impossible to get here... */
        for (tmp = str; *tmp && *tmp != 'm'; tmp++)
          ;

        /* Store the "background" colors */
        tmpansi = ansistack[ansitop];
        if (oldcodes == 0) {
          oldcodes = 1;
          ansitop++;
          ansistack[ansitop] = tmpansi;
          ansistack[ansitop].offbits = 0;
        }

        read_raw_ansi_data(&tmpansi, str);
        ansistack[ansitop].bits |= tmpansi.bits;
        ansistack[ansitop].bits &= ~(tmpansi.offbits); /* ANSI_RAW_NORMAL */
        if (tmpansi.fg[0]) {
          strncpy(ansistack[ansitop].fg, tmpansi.fg, COLOR_NAME_LEN);
        }
        if (tmpansi.bg[0]) {
          strncpy(ansistack[ansitop].bg, tmpansi.bg, COLOR_NAME_LEN);
        }

        str = tmp;
        if (*tmp)
          str++;
        break;
      }
    }

    /* Handle ANSI/Text */
    tmpansi = ansistack[ansitop];
    if (ansitop > 0 || ansiheight > 0) {
      /* Close existing tags as necessary to cleanly open the next. */
      /*  oldansi = ansistack[ansiheight]; */
      if (!ansi_equal(&oldansi, &tmpansi)) {
        while (ansiheight > 0) {
          if (howmanyopen > 0) {
            howmanyopen--;
            retval += safe_str(")]", buff, bp);
          }
          ansiheight--;
        }
      }
      if (!ansi_isnull(tmpansi) && !ansi_equal(&oldansi, &tmpansi)) {
        retval += safe_str("[ansi(", buff, bp);
        retval += write_ansi_letters(&tmpansi, buff, bp);
        retval += safe_chr(',', buff, bp);
        howmanyopen++;
      }
    }
    retval += escape_marked_str(&str, buff, bp);
  }

  for (; howmanyopen > 0; howmanyopen--)
    retval += safe_str(")]", buff, bp);
  for (; pueblotop > -1; pueblotop--) {
    retval += safe_str("[endtag(", buff, bp);
    retval += safe_str(pueblostack[pueblotop], buff, bp);
    retval += safe_str(")]", buff, bp);
  }

  return retval;
}

/** Our version of pcre_copy_substring, with ansi-safeness.
 * \param as the ansi_string whose .text value was matched against.
 * \param ovector the offset vectors
 * \param stringcount the number of subpatterns
 * \param stringnumber the number of the desired subpattern
 * \param nonempty if true, copy empty registers as well.
 * \param buff buffer to copy the subpattern to
 * \param bp pointer to the end of buffer
 * \return size of subpattern, or -1 if unknown pattern
 */
int
ansi_pcre_copy_substring(ansi_string *as, pcre2_match_data *md, int stringcount,
                         int stringnumber, int nonempty, char *buff, char **bp)
{
  int yield;
  PCRE2_SIZE *ovector;

  if (stringnumber < 0 || stringnumber >= stringcount) {
    return -1;
  }
  stringnumber *= 2;
  ovector = pcre2_get_ovector_pointer(md);
  yield = ovector[stringnumber + 1] - ovector[stringnumber];
  if (!nonempty || yield) {
    safe_ansi_string(as, ovector[stringnumber], yield, buff, bp);
    **bp = '\0';
  }
  return yield;
}

/** Our version of pcre_copy_named_substring, with ansi-safeness.
 * \param code the pcre compiled code
 * \param as the ansi_string whose .text value was matched against.
 * \param ovector the offset vectors
 * \param stringcount the number of subpatterns
 * \param stringname the name of the desired subpattern
 * \param ne if true, copy empty registers as well.
 * \param buff buffer to copy the subpattern to
 * \param bp pointer to the end of buffer
 * \return size of subpattern, or -1 if unknown pattern
 */
int
ansi_pcre_copy_named_substring(const pcre2_code *re, ansi_string *as,
                               pcre2_match_data *md, int stringcount,
                               const char *stringname, int ne, char *buff,
                               char **bp)
{
  int n =
    pcre2_substring_number_from_name(re, (const PCRE2_UCHAR *) stringname);
  if (n <= 0) {
    return -1;
  }
  return ansi_pcre_copy_substring(as, md, stringcount, n, ne, buff, bp);
}

/** Safely add a tag into a buffer.
 * If we support pueblo, this function adds the tag start token,
 * the tag, and the tag end token. If not, it does nothing.
 * If we can't fit the tag in, we don't put any of it in.
 * \param a_tag the html tag to add.
 * \param buf the buffer to append to.
 * \param bp pointer to address in buf to insert.
 * \param type type of markup, one of MARKUP_HTML or MARKUP_COLOR
 * \retval 0, successfully added.
 * \retval 1, tag wouldn't fit in buffer.
 */
static int
safe_markup(char const *a_tag, char *buf, char **bp, char type)
{
  int result;
  char *save = *bp;
  safe_chr(TAG_START, buf, bp);
  safe_chr(type, buf, bp);
  safe_str(a_tag, buf, bp);
  result = safe_chr(TAG_END, buf, bp);
  /* If it didn't all fit, rewind. */
  if (result)
    memset(save, '\0', *bp - save);
  return result;
}

int
safe_tag(char const *a_tag, char *buff, char **bp)
{
  if (SUPPORT_HTML)
    return safe_markup(a_tag, buff, bp, MARKUP_HTML);
  return 0;
}

/** Safely add a closing tag into a buffer.
 * If we support pueblo, this function adds the tag start token,
 * a slash, the tag, and the tag end token. If not, it does nothing.
 * If we can't fit the tag in, we don't put any of it in.
 * \param a_tag the html tag to add.
 * \param buf the buffer to append to.
 * \param bp pointer to address in buf to insert.
 * \param type type of markup, one of MARKUP_HTML or MARKUP_COLOR
 * \retval 0, successfully added.
 * \retval 1, tag wouldn't fit in buffer.
 */
static int
safe_markup_cancel(char const *a_tag, char *buf, char **bp, char type)
{
  int result;
  char *save = *bp;
  safe_chr(TAG_START, buf, bp);
  safe_chr(type, buf, bp);
  safe_chr('/', buf, bp);
  safe_str(a_tag, buf, bp);
  result = safe_chr(TAG_END, buf, bp);
  /* If it didn't all fit, rewind. */
  if (result)
    memset(save, '\0', *bp - save);
  return result;
}

int
safe_tag_cancel(char const *a_tag, char *buf, char **bp)
{
  if (SUPPORT_HTML)
    return safe_markup_cancel(a_tag, buf, bp, MARKUP_HTML);
  return 0;
}

/** Safely add a tag, some text, and a matching closing tag into a buffer.
 * If we can't fit the stuff, we don't put any of it in.
 * \param a_tag the html tag to add.
 * \param params tag parameters.
 * \param data the text to wrap the tag around.
 * \param buf the buffer to append to.
 * \param bp pointer to address in buf to insert.
 * \param player the player involved in all this, or NOTHING if internal.
 * \retval 0, successfully added.
 * \retval 1, tagged text wouldn't fit in buffer.
 */
int
safe_tag_wrap(char const *a_tag, char const *params, char const *data,
              char *buf, char **bp, dbref player)
{
  int result = 0;
  char *save = *bp;
  if (SUPPORT_HTML) {
    safe_chr(TAG_START, buf, bp);
    safe_chr(MARKUP_HTML, buf, bp);
    safe_str(a_tag, buf, bp);
    if (params && *params && ok_tag_attribute(player, params)) {
      safe_chr(' ', buf, bp);
      safe_str(params, buf, bp);
    }
    safe_chr(TAG_END, buf, bp);
  }
  result = safe_str(data, buf, bp);
  if (SUPPORT_HTML) {
    result = safe_tag_cancel(a_tag, buf, bp);
  }
  /* If it didn't all fit, rewind. */
  if (result)
    memset(save, '\0', *bp - save);
  return result;
}

char *
open_tag(const char *x)
{
  static char buff[BUFFER_LEN + 3];
  snprintf(buff, sizeof buff, "%c%c%s%c", TAG_START, MARKUP_HTML, x, TAG_END);
  return buff;
}

char *
close_tag(const char *x)
{
  static char buff[BUFFER_LEN + 4];
  snprintf(buff, sizeof buff, "%c%c/%s%c", TAG_START, MARKUP_HTML, x, TAG_END);
  return buff;
}

char *
wrap_tag(const char *x, const char *y)
{
  static char buff[(BUFFER_LEN * 2) + 8];
  snprintf(buff, sizeof buff, "%c%c%s%c%s%c%c/%s%c", TAG_START, MARKUP_HTML, x,
           TAG_END, y, TAG_START, MARKUP_HTML, x, TAG_END);
  return buff;
}
