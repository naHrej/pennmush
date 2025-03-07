/**
 * \file parse.c
 *
 * \brief The PennMUSH function/expression parser
 *
 * The most important function in this file is process_expression.
 *
 */

#include "copyrite.h"
#include "parse.h"

#include <math.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#include <stdio.h>

#include "ansi.h"
#include "attrib.h"
#include "case.h"
#include "conf.h"
#include "dbdefs.h"
#include "externs.h"
#include "flags.h"
#include "function.h"
#include "log.h"
#include "match.h"
#include "memcheck.h"
#include "mushdb.h"
#include "mushtype.h"
#include "mymalloc.h"
#include "mypcre.h"
#include "notify.h"
#include "strtree.h"
#include "strutil.h"
#include "tests.h"

extern char *absp[], *obj[], *poss[], *subj[]; /* fundb.c */
int global_fun_invocations;
int global_fun_recursions;
/* extern int re_subpatterns; */
/* extern int *re_offsets; */
/* extern ansi_string *re_from; */
extern int cpu_limit_warning_sent;

/** Structure for storing DEBUG output in a linked list */
struct debug_info {
  char *string;     /**< A DEBUG string */
  dbref executor;   /**< Object executing code */
  Debug_Info *prev; /**< Previous node in the linked list */
  Debug_Info *next; /**< Next node in the linked list */
};

FUNCTION_PROTO(fun_gfun);

#ifndef DOXYGEN_SHOULD_SKIP_THIS

/* Common error messages */
const char e_int[] = "#-1 ARGUMENT MUST BE INTEGER";
const char e_ints[] = "#-1 ARGUMENTS MUST BE INTEGERS";
const char e_uint[] = "#-1 ARGUMENT MUST BE POSITIVE INTEGER";
const char e_uints[] = "#-1 ARGUMENTS MUST BE POSITIVE INTEGERS";
const char e_num[] = "#-1 ARGUMENT MUST BE NUMBER";
const char e_nums[] = "#-1 ARGUMENTS MUST BE NUMBERS";
const char e_invoke[] = "#-1 FUNCTION INVOCATION LIMIT EXCEEDED";
const char e_call[] = "#-1 CALL LIMIT EXCEEDED";
const char e_perm[] = "#-1 PERMISSION DENIED";
const char e_atrperm[] = "#-1 NO PERMISSION TO GET ATTRIBUTE";
const char e_match[] = "#-1 NO MATCH";
const char e_notvis[] = "#-1 NO SUCH OBJECT VISIBLE";
const char e_disabled[] = "#-1 FUNCTION DISABLED";
const char e_range[] = "#-1 OUT OF RANGE";
const char e_argrange[] = "#-1 ARGUMENT OUT OF RANGE";
const char e_badregname[] = "#-1 REGISTER NAME INVALID";
const char e_toomanyregs[] = "#-1 TOO MANY REGISTERS";

#endif

#if 0
static void dummy_errors(void);

static void
dummy_errors()
{
  /* Just to make sure the error messages are in the translation
     tables. */
  char *temp;
  temp = T("#-1 ARGUMENT MUST BE INTEGER");
  temp = T("#-1 ARGUMENTS MUST BE INTEGERS");
  temp = T("#-1 ARGUMENT MUST BE POSITIVE INTEGER");
  temp = T("#-1 ARGUMENTS MUST BE POSITIVE INTEGERS");
  temp = T("#-1 ARGUMENT MUST BE NUMBER");
  temp = T("#-1 ARGUMENTS MUST BE NUMBERS");
  temp = T("#-1 FUNCTION INVOCATION LIMIT EXCEEDED");
  temp = T("#-1 CALL LIMIT EXCEEDED");
  temp = T("#-1 PERMISSION DENIED");
  temp = T("#-1 NO PERMISSION TO GET ATTRIBUTE");
  temp = T("#-1 NO MATCH");
  temp = T("#-1 NO SUCH OBJECT VISIBLE");
  temp = T("#-1 FUNCTION DISABLED");
  temp = T("#-1 OUT OF RANGE");
  temp = T("#-1 ARGUMENT OUT OF RANGE");
  temp = T("#-1 REGISTER NAME INVALID");
  temp = T("#-1 TOO MANY REGISTERS");
}

#endif

/** Given a string, parse out a dbref.
 * \param str string to parse.
 * \return dbref contained in the string, or NOTHING if not a valid dbref.
 */
dbref
parse_dbref(char const *str)
{
  /* Make sure string is strictly in format "#nnn".
   * Otherwise, possesives will be fouled up.
   */
  char const *p;
  dbref num;

  if (!str || (*str != NUMBER_TOKEN) || !*(str + 1))
    return NOTHING;
  for (p = str + 1; isdigit(*p); p++) {
  }
  if (*p)
    return NOTHING;

  num = atoi(str + 1);
  if (!GoodObject(num))
    return NOTHING;
  return num;
}

/** Version of parse_dbref() that doesn't do GoodObject checks */
dbref
qparse_dbref(const char *s)
{

  if (!s || (*s != NUMBER_TOKEN) || !*(s + 1))
    return NOTHING;
  return parse_integer(s + 1);
}

/** Given a thing, return a buffer with its objid.
 * This thing cheats, it uses a static buffer so it can safely return
 * around 400-ish objids before rewinding back to start. This is not
 * intended for use with large lists, but for queue_event() or for calling
 * functions that need multiple char *s
 *
 * \param thing thing to the return the objid of.
 * \return The objid string.
 */
const char *
unparse_objid(dbref thing)
{
  static char obuff[BUFFER_LEN];
  static char *obp = obuff;
  char *retval;

  if (!GoodObject(thing)) {
    return "#-1";
  }

  if ((obp - obuff) >= (BUFFER_LEN - 40)) {
    obp = obuff;
  }
  retval = obp;

  safe_dbref(thing, obuff, &obp);
  safe_chr(':', obuff, &obp);
  safe_integer(CreTime(thing), obuff, &obp);
  *(obp++) = '\0';
  return retval;
}

/** Given a string, parse out an object id or dbref.
 * \param str string to parse.
 * \param strict Require a full objid (with :ctime) instead of a plain dbref?
 * \return dbref of object referenced by string, or NOTHING if not a valid
 * string or not an existing dbref.
 */
dbref
real_parse_objid(char const *str, bool strict)
{
  const char *p;
  if ((p = strchr(str, ':'))) {
    char tbuf1[BUFFER_LEN];
    dbref it;
    /* A unique id, probably */
    mush_strncpy(tbuf1, str, (p - str) + 1);
    it = parse_dbref(tbuf1);
    if (GoodObject(it)) {
      time_t matchtime;
      p++;
      if (!is_strict_integer(p))
        return NOTHING;
      matchtime = parse_integer(p);
      return (CreTime(it) == matchtime) ? it : NOTHING;
    } else
      return NOTHING;
  } else if (strict) {
    return NOTHING;
  } else {
    return parse_dbref(str);
  }
}

/** Given a string, parse out a boolean value.
 * The meaning of boolean is fuzzy. To TinyMUSH, any string that begins with
 * a non-zero number is true, and everything else is false.
 * To PennMUSH, negative dbrefs are false, non-negative dbrefs are true,
 * 0 is false, all other numbers are true, empty or blank strings are false,
 * and all other strings are true.
 * \param str string to parse.
 * \retval 1 str represents a true value.
 * \retval 0 str represents a false value.
 */
bool
parse_boolean(char const *str)
{
  char clean[BUFFER_LEN];
  int i = 0;
  strcpy(clean, remove_markup(str, NULL));
  if (TINY_BOOLEANS) {
    return (atoi(clean) ? 1 : 0);
  } else {
    /* Turn a string into a boolean value.
     * All negative dbrefs are false, all non-negative dbrefs are true.
     * Zero is false, all other numbers are true.
     * Empty (or space only) strings are false, all other strings are true.
     */
    /* Null strings are false */
    if (!clean[0])
      return 0;
    /* Negative dbrefs are false - actually anything starting #-,
     * which will also cover our error messages. */
    if (*clean == '#' && *(clean + 1) && (*(clean + 1) == '-'))
      return 0;
    /* Non-zero numbers are true, zero is false */
    if (is_strict_number(clean))
      return parse_number(clean) != 0; /* avoid rounding problems */
    /* Skip blanks */
    while (clean[i] == ' ')
      i++;
    /* If there's any non-blanks left, it's true */
    return clean[i] != '\0'; /* force to 1 or 0 */
  }
}

/** Is a string a boolean value?
 * To TinyMUSH, any integer is a boolean value. To PennMUSH, any
 * string at all is boolean.
 * \param str string to check.
 * \retval 1 string is a valid boolean.
 * \retval 0 string is not a valid boolean.
 */
bool
is_boolean(char const *str)
{
  if (TINY_BOOLEANS)
    return is_integer(str);
  else
    return 1;
}

TEST_GROUP(is_boolean)
{
  // TEST is_boolean REQUIRES is_integer
  int saved = TINY_BOOLEANS;
  options.tiny_booleans = 1;
  TEST("is_boolean.tiny.1", is_boolean("0") == 1);
  TEST("is_boolean.tiny.2", is_boolean("") == 0);
  TEST("is_boolean.tiny.3", is_boolean("foo") == 0);
  options.tiny_booleans = 0;
  TEST("is_boolean.penn.1", is_boolean("0") == 1);
  TEST("is_boolean.penn.2", is_boolean("") == 1);
  TEST("is_boolean.penn.3", is_boolean("foo") == 1);
  options.tiny_booleans = saved;
}

/** Is a string a dbref?
 * A dbref is a string starting with a #, optionally followed by a -,
 * and then followed by at least one digit, and nothing else.
 * \param str string to check.
 * \retval 1 string is a dbref.
 * \retval 0 string is not a dbref.
 */
bool
is_dbref(char const *str)
{
  if (!str || (*str != NUMBER_TOKEN) || !*(str + 1)) {
    return 0;
  }
  str += 1;
  if (*str == '-') {
    str += 1;
  }
  if (!*str) {
    return 0;
  }
  for (; isdigit(*str); str += 1) {
  }
  return !*str;
}

TEST_GROUP(is_dbref)
{
  TEST("is_dbref.1", is_dbref("") == 0);
  TEST("is_dbref.2", is_dbref("foo") == 0);
  TEST("is_dbref.3", is_dbref("#foo") == 0);
  TEST("is_dbref.4", is_dbref("#-1") == 1);
  TEST("is_dbref.5", is_dbref("#1234") == 1);
  TEST("is_dbref.6", is_dbref("#12AB") == 0);
  TEST("is_dbref.7", is_dbref("#") == 0);
  TEST("is_dbref.8", is_dbref("#-") == 0);
  TEST("is_dbref.9", is_dbref("#-A") == 0);
}

/** Is a string an objid?
 * \verbatim
 * An objid is a string starting with a #, optionally followed by a -,
 * and then followed by at least one digit, then optionally followed
 * by a : and at least one digit, and nothing else.
 * In regex: ^#-?\d+(:\d+)?$
 * \endverbatim
 * \param str string to check.
 * \retval 1 string is an objid
 * \retval 0 string is not an objid.
 */
bool
is_objid(char const *str)
{
  static pcre2_code *re = NULL;
  static pcre2_match_data *md = NULL;
  const PCRE2_UCHAR *val;
  size_t vlen;

  if (!str) {
    return 0;
  }
  if (!re) {
    int errcode;
    PCRE2_SIZE erroffset;
    re = pcre2_compile((const PCRE2_UCHAR *) "^#-?\\d+(?::\\d+)?$",
                       PCRE2_ZERO_TERMINATED,
                       re_compile_flags | PCRE2_NO_UTF_CHECK, &errcode,
                       &erroffset, re_compile_ctx);
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    md = pcre2_match_data_create_from_pattern(re, NULL);
  }
  val = (const PCRE2_UCHAR *) remove_markup((const char *) str, &vlen);
  return pcre2_match(re, val, vlen - 1, 0, re_match_flags, md, re_match_ctx) >=
         0;
}

/** Is string an integer?
 * To TinyMUSH, any string is an integer. To PennMUSH, a string that
 * passes parse_int is an integer, and a blank string is an integer
 * if NULL_EQ_ZERO is turned on.
 * **Checks TINY_MATH**. Use is_strict_integer() for internal checks.
 * \param str string to check.
 * \retval 1 string is an integer.
 * \retval 0 string is not an integer.
 */
bool
is_integer(char const *str)
{
  char *end;

  /* If we're emulating Tiny, anything is an integer */
  if (TINY_MATH)
    return 1;
  if (!str)
    return 0;
  while (isspace(*str))
    str++;
  if (*str == '\0')
    return NULL_EQ_ZERO;
  errno = 0;
  parse_int(str, &end, 10);
  if (errno == ERANGE || *end != '\0')
    return 0;
  return 1;
}

TEST_GROUP(is_integer)
{
  int saved_math = TINY_MATH;
  int saved_null = NULL_EQ_ZERO;
  options.tiny_math = 1;
  TEST("is_integer.tiny.1", is_integer("") == 1);
  TEST("is_integer.tiny.2", is_integer("foo") == 1);
  TEST("is_integer.tiny.3", is_integer("5") == 1);
  TEST("is_integer.tiny.4", is_integer(NULL) == 1);
  options.tiny_math = 0;
  TEST("is_integer.penn.1", is_integer("foo") == 0);
  TEST("is_integer.penn.2", is_integer("5") == 1);
  TEST("is_integer.penn.3", is_integer("  12") == 1);
  TEST("is_integer.penn.4", is_integer(NULL) == 0);
  TEST("is_integer.penn.5", is_integer("-5") == 1);
  TEST("is_integer.penn.6", is_integer("2147483647") == 1); // INT_MAX
  TEST("is_integer.penn.7", is_integer("2147483648") == 0); // INT_MAX + 1
  TEST("is_integer.penn.8", is_integer("12foo") == 0);
  options.null_eq_zero = 1;
  TEST("is_integer.penn.9", is_integer("") == 1);
  options.null_eq_zero = 0;
  TEST("is_integer.penn.10", is_integer("") == 0);
  options.tiny_math = saved_math;
  options.null_eq_zero = saved_null;
}

/** Is string an unsigned integer?
 * To TinyMUSH, any string is an uinteger. To PennMUSH, a string that
 * passes parse_uint is an uinteger, and a blank string is an uinteger
 * if NULL_EQ_ZERO is turned on.
 * **Checks TINY_MATH**. Use is_strict_uinteger() for internal checks.
 * \param str string to check.
 * \retval 1 string is an uinteger.
 * \retval 0 string is not an uinteger.
 */
bool
is_uinteger(char const *str)
{
  char *end;

  /* If we're emulating Tiny, anything is an integer */
  if (TINY_MATH)
    return 1;
  if (!str)
    return 0;
  /* strtoul() accepts negative numbers, so we still have to do this check */
  while (isspace(*str))
    str++;
  if (*str == '\0')
    return NULL_EQ_ZERO;
  if (!(isdigit(*str) || *str == '+'))
    return 0;
  errno = 0;
  parse_uint(str, &end, 10);
  if (errno == ERANGE || *end != '\0')
    return 0;
  return 1;
}

TEST_GROUP(is_uinteger)
{
  int saved_math = TINY_MATH;
  int saved_null = NULL_EQ_ZERO;
  options.tiny_math = 1;
  TEST("is_uinteger.tiny.1", is_uinteger("") == 1);
  TEST("is_uinteger.tiny.2", is_uinteger("foo") == 1);
  TEST("is_uinteger.tiny.3", is_uinteger("5") == 1);
  TEST("is_uinteger.tiny.4", is_uinteger(NULL) == 1);
  TEST("is_uinteger.tiny.5", is_uinteger("-5") == 1);
  options.tiny_math = 0;
  TEST("is_uinteger.penn.1", is_uinteger("foo") == 0);
  TEST("is_uinteger.penn.2", is_uinteger("5") == 1);
  TEST("is_uinteger.penn.3", is_uinteger("  12") == 1);
  TEST("is_uinteger.penn.4", is_uinteger(NULL) == 0);
  TEST("is_uinteger.penn.5", is_uinteger("-5") == 0);
  TEST("is_uinteger.penn.6", is_uinteger("4294967295") == 1); // UINT_MAX
  TEST("is_uinteger.penn.7", is_uinteger("4294967296") == 0); // UINT_MAX + 1
  TEST("is_uinteger.penn.8", is_uinteger("12foo") == 0);
  options.null_eq_zero = 1;
  TEST("is_uinteger.penn.9", is_uinteger("") == 1);
  options.null_eq_zero = 0;
  TEST("is_uinteger.penn.10", is_uinteger("") == 0);
  options.tiny_math = saved_math;
  options.null_eq_zero = saved_null;
}

/** Is string really an unsigned integer?
 * \param str string to check.
 * \retval 1 string is an uinteger.
 * \retval 0 string is not an uinteger.
 */
bool
is_strict_uinteger(const char *str)
{
  char *end;

  if (!str)
    return 0;
  /* strtoul() accepts negative numbers, so we still have to do this check */
  while (isspace(*str))
    str++;
  if (*str == '\0')
    return 0;
  if (!(isdigit(*str) || *str == '+'))
    return 0;
  errno = 0;
  parse_uint(str, &end, 10);
  if (errno == ERANGE || *end != '\0')
    return 0;
  return 1;
}

/** Is string a number by the strict definition?
 * A strict number is a non-null string that passes strtod.
 * \param str string to check.
 * \retval 1 string is a strict number.
 * \retval 0 string is not a strict number.
 */
bool
is_strict_number(char const *str)
{
  char *end;
  int throwaway __attribute__((__unused__));
  if (!str)
    return 0;
  errno = 0;
  throwaway = strtod(str, &end);
  if (errno == ERANGE || *end != '\0')
    return 0;
  return end > str;
}

#ifndef HAVE_ISNORMAL
/** Is string a number that isn't inf or nan?
 * Only needed for systems without isnormal()
 * \param val NVAL
 * \retval 1 num is a good number.
 * \retval 0 num is not a good number.
 */
bool
is_good_number(NVAL val)
{
  char numbuff[128];
  char *p;
  snprintf(numbuff, 128, "%f", val);
  p = numbuff;
  /* Negative? */
  if (*p == '-')
    p++;
  /* Must start with a digit. */
  if (!*p || !isdigit(*p))
    return 0;
  return 1;
}
#endif

/** Is string an integer by the strict definition?
 * A strict integer is a non-null string that passes parse_int.
 * \param str string to check.
 * \retval 1 string is a strict integer.
 * \retval 0 string is not a strict integer.
 */
bool
is_strict_integer(char const *str)
{
  char *end;
  if (!str)
    return 0;
  errno = 0;
  parse_int(str, &end, 10);
  if (errno == ERANGE || *end != '\0')
    return 0;
  return end > str;
}

/** Is string an int64_t by the strict definition?
 * A strict integer is a non-null string that passes parse_int64.
 * \param str string to check.
 * \retval 1 string is a strict integer.
 * \retval 0 string is not a strict integer.
 */
bool
is_strict_int64(char const *str)
{
  char *end;
  if (!str)
    return 0;
  errno = 0;
  parse_int64(str, &end, 10);
  if (errno == ERANGE || *end != '\0') {
    return 0;
  }
  return end > str;
}

/** Does a string contain a list of space-separated valid signed integers?
 * Must contain at least one int. For internal use; ignores TINY_MATH.
 * \param str string to check
 * \retval 1 string is a list of integers
 * \retval 0 string is empty, or contains a non-space, non-integer char
 */
bool
is_integer_list(char const *str)
{
  char *start, *end;

  if (!str || !*str)
    return 0;

  start = (char *) str;
  do {
    while (*start && *start == ' ')
      start++;
    if (!*start)
      return 1;
    (void) strtol(start, &end, 10);
    if (!(*end == '\0' || *end == ' '))
      return 0;
    start = end;
  } while (*start);

  return 1;
}

/** Is string a number?
 * To TinyMUSH, any string is a number. To PennMUSH, a strict number is
 * a number, and a blank string is a number if NULL_EQ_ZERO is turned on.
 * \param str string to check.
 * \retval 1 string is a number.
 * \retval 0 string is not a number.
 */
bool
is_number(char const *str)
{
  /* If we're emulating Tiny, anything is a number */
  if (TINY_MATH)
    return 1;
  while (isspace(*str))
    str++;
  if (*str == '\0')
    return NULL_EQ_ZERO;
  return is_strict_number(str);
}

TEST_GROUP(is_number)
{
  int saved_tiny = TINY_MATH;
  int saved_null = NULL_EQ_ZERO;
  options.tiny_math = 1;
  TEST("is_number.tiny.1", is_number("") == 1);
  TEST("is_number.tiny.2", is_number("foo") == 1);
  TEST("is_number.tiny.3", is_number("5") == 1);
  TEST("is_number.tiny.4", is_number(NULL) == 1);
  TEST("is_number.tiny.5", is_number("-5.5") == 1);
  options.tiny_math = 0;
  TEST("is_number.penn.1", is_number("foo") == 0);
  TEST("is_number.penn.2", is_number("5") == 1);
  TEST("is_number.penn.3", is_number("  12.05") == 1);
  TEST("is_number.penn.5", is_number("-5") == 1);
  TEST("is_number.penn.7", is_number("2e6") == 1);
  TEST("is_number.penn.8", is_number("12foo") == 0);
  options.null_eq_zero = 1;
  TEST("is_number.penn.9", is_number("") == 1);
  options.null_eq_zero = 0;
  TEST("is_number.penn.10", is_number("") == 0);
  options.tiny_math = saved_tiny;
  options.null_eq_zero = saved_null;
}

/** Convert a string containing a signed integer into an int.
 * Does not do any format checking. Invalid strings will return 0.
 * Use this instead of strtol() when storing to an int to avoid problems
 * where sizeof(int) < sizeof(long).
 * \param s The string to convert
 * \param end pointer to store the end of the parsed part of the string in
 * if not NULL.
 * \param base the base to convert from.
 * \return the number, or INT_MIN on underflow, INT_MAX on overflow,
 * with errno set to ERANGE.
 */
int
parse_int(const char *s, char **end, int base)
{
  long x;

  x = strtol(s, end, base);

  if (sizeof(int) == sizeof(long))
    return x;
  else {
    /* These checks are only meaningful on 64-bit systems */
    if (x < INT_MIN) {
      errno = ERANGE;
      return INT_MIN;
    } else if (x > INT_MAX) {
      errno = ERANGE;
      return INT_MAX;
    } else
      return x;
  }
}

/** Convert a string containing a signed integer into an int32_t.
 * Does not do any format checking. Invalid strings will return 0.
 * \param s The string to convert
 * \param end pointer to store the end of the parsed part of the string in
 * if not NULL.
 * \param base the base to convert from.
 * \return the number, or INT32_MIN on underflow, INT32_MAX on overflow,
 * with errno set to ERANGE.
 */
int32_t
parse_int32(const char *s, char **end, int base)
{
  if (sizeof(int) == 4) {
    return parse_int(s, end, base);
  } else {
    /* This block will probably never happen, but just in case somebody's
     * trying to use Penn on a really odd hardware... */
    long n = strtol(s, end, base);
    if (n < INT32_MIN) {
      errno = ERANGE;
      return INT32_MIN;
    } else if (n > INT32_MAX) {
      errno = ERANGE;
      return INT32_MAX;
    } else {
      return n;
    }
  }
}

/** Convert a string containing an integer into an int64_t.
 * Does not do any format checking. Invalid strings will return 0.
 * \param s The string to convert
 * \param end pointer to store the end of the parsed part of the string in
 * if not NULL.
 * \param base the base to convert from.
 * \return the number, or INT64_MIN on underflow, INT64_MAX on overflow,
 * with errno set to ERANGE.
 */
int64_t
parse_int64(const char *s, char **end, int base)
{
#if defined(WIN32)
  return _strtoi64(s, end, base);
#else
  if (sizeof(long) == 8) {
    return strtol(s, end, base);
  } else if (sizeof(long long) == 8) {
    return strtoll(s, end, base);
  } else {
    /* long long greater than 64 bits? */
    long long val = strtoll(s, end, base);
    if (val > INT64_MAX) {
      errno = ERANGE;
      return INT64_MAX;
    } else if (val < INT64_MIN) {
      errno = ERANGE;
      return INT64_MIN;
    } else {
      return val;
    }
  }
#endif
}

/** Convert a string containing an unsigned integer into an int.
 * Does not do any format checking. Invalid strings will return 0.
 * Use this instead of strtoul() when storing to an int to avoid problems
 * where sizeof(int) < sizeof(long).
 * \param s The string to convert
 * \param end pointer to store the first invalid char at, or NULL
 * \param base base for conversion
 * \return the number, or UINT_MAX on overflow with errno set to ERANGE.
 */
unsigned int
parse_uint(const char *s, char **end, int base)
{
  unsigned long x;

  x = strtoul(s, end, base);

  if (sizeof(unsigned int) == sizeof(unsigned long)) {
    return x;
  } else {
    /* These checks are only meaningful on 64-bit systems */
    if (x > UINT_MAX) {
      errno = ERANGE;
      return UINT_MAX;
    } else
      return x;
  }
}

/** Convert a string containing an unsigned integer into an uint32_t.
 * Does not do any format checking. Invalid strings will return 0.
 * \param s The string to convert
 * \param end pointer to store the end of the parsed part of the string in
 * if not NULL.
 * \param base the base to convert from.
 * \return the number, or UINT32_MAX on overflow, with errno set to ERANGE.
 */
uint32_t
parse_uint32(const char *s, char **end, int base)
{
  unsigned long x;

  x = strtoul(s, end, base);

  if (sizeof(unsigned long) == 4) {
    return x;
  } else {
    if (x > UINT32_MAX) {
      errno = ERANGE;
      return UINT32_MAX;
    } else {
      return x;
    }
  }
}

/** Convert a string containing an unsigned integer into an uint64_t.
 * Does not do any format checking. Invalid strings will return 0.
 * \param s The string to convert
 * \param end pointer to store the end of the parsed part of the string in
 * if not NULL.
 * \param base the base to convert from.
 * \return the number, or UINT64_MAX on overflow, with errno set to ERANGE.
 */
uint64_t
parse_uint64(const char *s, char **end, int base)
{
#if defined(WIN32)
  return _strtoui64(s, end, base);
#else
  if (sizeof(unsigned long) == 8) {
    return strtoul(s, end, base);
  } else if (sizeof(unsigned long long) == 8) {
    return strtoull(s, end, base);
  } else {
    /* unsigned long long greater than 64 bits? */
    unsigned long long val = strtoull(s, end, base);
    if (val > UINT64_MAX) {
      errno = ERANGE;
      return UINT64_MAX;
    } else {
      return val;
    }
  }
#endif
}

/** PE_REGS: Named Q-registers. We have two strtrees: One for names,
 * one for values.
 */
StrTree pe_reg_names;
StrTree pe_reg_vals;

/* Slabs for PE_REGS and PE_REG_VALs */
slab *pe_reg_slab;
slab *pe_reg_val_slab;

/* Lame speed-up so we don't constantly call tprintf :D */
static const char *envid[10] = {"0", "1", "2", "3", "4",
                                "5", "6", "7", "8", "9"};

void
init_pe_regs_trees()
{
  int i;
  char qv[2] = "0";

  pe_reg_slab = slab_create("PE_REGS", sizeof(PE_REGS));
  pe_reg_val_slab = slab_create("PE_REG_VAL", sizeof(PE_REG_VAL));

  st_init(&pe_reg_names, "pe_reg_names");
  st_init(&pe_reg_vals, "pe_reg_vals");

  /* Permanently insert 0-9 A-Z into the qreg name table.
   *
   * Since st_insert_perm isn't coded, we just insert these once.
   */
  for (i = 0; i < 10; i++) {
    qv[0] = '0' + i;
    st_insert(qv, &pe_reg_names);
  }
  for (i = 0; i < 26; i++) {
    qv[0] = 'A' + i;
    st_insert(qv, &pe_reg_names);
  }
}

void
free_pe_regs_trees()
{
  st_flush(&pe_reg_names);
  st_flush(&pe_reg_vals);
}

#ifdef DEBUG_PENNMUSH
FUNCTION(fun_pe_regs_dump)
{
  dbref who = caller;

  if (args[0] && *args[0]) {
    who = lookup_player(args[0]);
    if (!GoodObject(who)) {
      safe_str("#-1", buff, bp);
      return;
    }
  }

  pe_regs_dump(pe_info->regvals, who);
}
#endif

/* For debugging purposes. */
void
pe_regs_dump(PE_REGS *pe_regs, dbref who)
{
  int i = 0;
  PE_REG_VAL *val;

  notify(who, " ");
  while (pe_regs && i < 100) {
    notify_format(who, "%d: %.4X '%s'", i, pe_regs->flags, pe_regs->name);
    i++;
    if (!pe_regs->flags) {
      notify_format(who, "NULL pe_regs type found?! Quitting.");
      break;
    }
    for (val = pe_regs->vals; val; val = val->next) {
      if (val->type & PE_REGS_STR) {
        notify_format(who, " %.2X(%.2X) %-10s: %s", val->type & 0xFF,
                      (val->type & 0xFFFF00) >> 8, val->name, val->val.sval);
      } else {
        notify_format(who, " %.2X(%.2X) %-10s: %d", val->type & 0xFF,
                      (val->type & 0xFFFF00) >> 8, val->name, val->val.ival);
      }
    }
    pe_regs = pe_regs->prev;
  }
  notify(who, " ");
}

/** Create a PE_REGS context.
 *
 * \param pr_flags PR_* flags, bitwise or'd.
 * \param name A name to identify the pe_regs, for debugging
 * \return a newly created, allocated pe_regs
 */
PE_REGS *
pe_regs_create_real(int pr_flags, const char *name)
{
  PE_REGS *pe_regs = slab_malloc(pe_reg_slab, NULL);
  ADD_CHECK("pe_reg_slab");
  ADD_CHECK(name);

  pe_regs->name = name;
  pe_regs->qcount = 0;
  pe_regs->count = 0;
  pe_regs->flags = pr_flags;
  pe_regs->vals = NULL;
  pe_regs->prev = NULL;
  return pe_regs;
}

/* Delete the _value_ of a single val, leave its name alone */
void
pe_reg_val_free_val(PE_REG_VAL *val)
{
  /* Don't do anything if it's NOCOPY or if it's an integer. */
  if (val->type & (PE_REGS_INT | PE_REGS_NOCOPY))
    return;

  if (val->type & PE_REGS_STR) {
    st_delete(val->val.sval, &pe_reg_vals);
    DEL_CHECK("pe_reg_val-val");
  }
}

/* Delete a single val. */
void
pe_reg_val_free(PE_REG_VAL *val)
{
  pe_reg_val_free_val(val);
  st_delete(val->name, &pe_reg_names);
  DEL_CHECK("pe_reg_val-name");
  slab_free(pe_reg_val_slab, val);
  DEL_CHECK("pe_reg_val_slab");
}

/** Free all values from a PE_REGS context.
 *
 * \param pe_regs The pe_regs to clear
 */
void
pe_regs_clear(PE_REGS *pe_regs)
{
  PE_REG_VAL *val = pe_regs->vals;
  PE_REG_VAL *next;

  while (val) {
    next = val->next;
    pe_reg_val_free(val);
    val = next;
  }
  pe_regs->count = 0;
  pe_regs->qcount = 0;
  pe_regs->vals = NULL;
}

/** Free all values of a specific type from a PE_REGS context.
 *
 * \param pe_regs The pe_regs to clear
 * \param type     The type(s) of pe_reg_vals to clear.
 */
void
pe_regs_clear_type(PE_REGS *pe_regs, int type)
{
  PE_REG_VAL *val = pe_regs->vals;
  PE_REG_VAL *next;
  PE_REG_VAL *prev = NULL;

  while (val) {
    next = val->next;
    if (val->type & type) {
      if (prev) {
        prev->next = next;
      } else {
        pe_regs->vals = next;
      }
      pe_reg_val_free(val);
    } else {
      prev = val;
    }
    val = next;
  }
}

/** Free a PE_REGS context.
 *
 * \param pe_regs The pe_regs to free up.
 */
void
pe_regs_free(PE_REGS *pe_regs)
{
  pe_regs_clear(pe_regs);
  DEL_CHECK(pe_regs->name);
  slab_free(pe_reg_slab, pe_regs);
  DEL_CHECK("pe_reg_slab");
}

/** Create a new PE_REGS context, and return it for manipulation.
 *
 * \param pe_info The pe_info to push the new PE_REGS into.
 * \param pr_flags PR_* flags, bitwise or'd.
 * \param name name, used for memory checks
 */
PE_REGS *
pe_regs_localize_real(NEW_PE_INFO *pe_info, uint32_t pr_flags, const char *name)
{
  PE_REGS *pe_regs = pe_regs_create_real(pr_flags, name);
  pe_regs->prev = pe_info->regvals;
  pe_info->regvals = pe_regs;

  return pe_regs;
}

/** Restore a PE_REGS context.
 *
 * \param pe_info The pe_info to pop the pe_regs out of.
 * \param pe_regs The pe_regs that formed the 'level' to pop out of.
 */
void
pe_regs_restore(NEW_PE_INFO *pe_info, PE_REGS *pe_regs)
{
  pe_info->regvals = pe_regs->prev;
}

#define FIND_PVAL(pval, key, type)                                             \
  do {                                                                         \
    if (!pval)                                                                 \
      break;                                                                   \
    if ((pval->type & type & PE_REGS_TYPE) && !strcmp(pval->name, key)) {      \
      break;                                                                   \
    }                                                                          \
    pval = pval->next;                                                         \
  } while (pval)

/** Is the given key a named register (not A-Z or 0-9)?
 */
bool
is_named_register(const char *key)
{
  if (!key || !*key)
    return 1;

  if (key[1] != '\0')
    return 1;

  if ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= 'A' && key[0] <= 'Z') ||
      (key[0] >= '0' && key[0] <= '9'))
    return 0;

  return 1;
}

/** Set a string value in a PE_REGS structure.
 *
 * pe_regs_set is authoritative: it ignores flags set on the PE_REGS,
 * it doesn't recurse up the chain, etc. So it is intended to be used
 * only in the same function where the PE_REGS is created.
 *
 * \param pe_regs The pe_regs to set it in.
 * \param type The type (REG_QREG, REG-... etc) of the register to set.
 * \param lckey Register name
 * \param val Register value.
 * \param override If it already exists, then overwrite it.
 */
void
pe_regs_set_if(PE_REGS *pe_regs, int type, const char *lckey, const char *val,
               int override)
{
  /* pe_regs_set is authoritative: it ignores flags set on the PE_REGS,
   * it doesn't recurse up the chain, etc. */
  PE_REG_VAL *pval = pe_regs->vals;
  char key[PE_KEY_LEN];
  static const char noval[] = "";
  strupper_r(lckey, key, sizeof key);
  FIND_PVAL(pval, key, type);
  if (!(type & PE_REGS_NOCOPY)) {
    if (!val || !val[0]) {
      val = noval;
      type |= PE_REGS_NOCOPY;
    }
  }
  if (pval) {
    if (!override)
      return;
    /* Delete its value */
    pe_reg_val_free_val(pval);
  } else {
    pval = slab_malloc(pe_reg_val_slab, NULL);
    ADD_CHECK("pe_reg_val_slab");
    pval->name = st_insert(key, &pe_reg_names);
    ADD_CHECK("pe_reg_val-name");
    pval->next = pe_regs->vals;
    pe_regs->vals = pval;
    pe_regs->count++;
    if (type & PE_REGS_Q) {
      if (is_named_register(key)) {
        pe_regs->qcount++;
      }
    }
  }
  if (type & PE_REGS_NOCOPY) {
    pval->type = type | PE_REGS_STR;
    pval->val.sval = val;
  } else {
    pval->type = type | PE_REGS_STR;
    pval->val.sval = st_insert(val, &pe_reg_vals);
    ADD_CHECK("pe_reg_val-val");
  }
}

/** Set an integer value in a PE_REGS structure.
 *
 * pe_regs_set is authoritative: it ignores flags set on the PE_REGS,
 * it doesn't recurse up the chain, etc. So it is intended to be used
 * only in the same function where the PE_REGS is created.
 *
 * \param pe_regs The pe_regs to set it in.
 * \param type The type (REG_QREG, REG-... etc) of the register to set.
 * \param lckey Register name
 * \param val Register value.
 * \param override If 1, then replace any extant value for the register
 */
void
pe_regs_set_int_if(PE_REGS *pe_regs, int type, const char *lckey, int val,
                   int override)
{
  PE_REG_VAL *pval = pe_regs->vals;
  char key[PE_KEY_LEN];
  strupper_r(lckey, key, sizeof key);
  FIND_PVAL(pval, key, type);
  if (pval) {
    if (!override)
      return;
    pe_reg_val_free_val(pval);
  } else {
    pval = slab_malloc(pe_reg_val_slab, NULL);
    ADD_CHECK("pe_reg_val_slab");
    pval->name = st_insert(key, &pe_reg_names);
    ADD_CHECK("pe_reg_val-name");
    pval->next = pe_regs->vals;
    pe_regs->vals = pval;
    pe_regs->count++;
    if (type & PE_REGS_Q) {
      if (is_named_register(key)) {
        pe_regs->qcount++;
      }
    }
  }
  pval->type = type | PE_REGS_INT;
  pval->val.ival = val;
}

const char *
pe_regs_get(PE_REGS *pe_regs, int type, const char *lckey)
{
  PE_REG_VAL *pval = pe_regs->vals;
  char key[PE_KEY_LEN];
  strupper_r(lckey, key, sizeof key);
  FIND_PVAL(pval, key, type);
  if (!pval)
    return NULL;
  if (pval->type & PE_REGS_STR) {
    return pval->val.sval;
  } else if (pval->type & PE_REGS_INT) {
    return unparse_integer(pval->val.ival);
  }
  return NULL;
}

/** Get a typed value from a pe_regs structure, returned as an integer.
 *
 * \param pe_regs The PE_REGS to fetch from.
 * \param type The type of the value to get
 * \param lckey Q-register name.
 */
int
pe_regs_get_int(PE_REGS *pe_regs, int type, const char *lckey)
{
  PE_REG_VAL *pval = pe_regs->vals;
  char key[PE_KEY_LEN];
  strupper_r(lckey, key, sizeof key);
  FIND_PVAL(pval, key, type);
  if (!pval)
    return 0;
  if (pval->type & PE_REGS_STR) {
    return parse_integer(pval->val.sval);
  } else if (pval->type & PE_REGS_INT) {
    return pval->val.ival;
  }
  return 0;
}

/** Copy Q-reg values to one PE_REGS from another.
 * \param dst The PE_REGS to copy to.
 * \param src The PE_REGS to copy from.
 */
void
pe_regs_qcopy(PE_REGS *dst, PE_REGS *src)
{
  PE_REG_VAL *val;
  while (src) {
    for (val = src->vals; val; val = val->next) {
      if (val->type & PE_REGS_Q) {
        if (val->type & PE_REGS_STR) {
          pe_regs_set(dst, val->type, val->name, val->val.sval);
        } else {
          pe_regs_set_int(dst, val->type, val->name, val->val.ival);
        }
      }
    }
    src = src->prev;
  }
}

void
pe_regs_copystack(PE_REGS *new_regs, PE_REGS *pe_regs, int copytypes,
                  int override)
{
  int scount = 0; /* stext counts */
  int icount = 0; /* itext counts */
  int smax = 0;
  int imax = 0;
  char itype;
  int inum;
  /* Disable PE_REGS_NOCOPY: If we're copying, we want to copy. */
  int andflags = 0xFF;
  char numbuff[10];
  PE_REG_VAL *val, *prev, *next;
  prev = NULL;

  if (!pe_regs)
    return;

  if (override && (copytypes & PE_REGS_ARG) && (pe_regs->flags & PE_REGS_ARG)) {
    /* Look for all PE_REGS_ARG flags in new_regs, and delete them. */
    for (val = new_regs->vals; val; val = next) {
      next = val->next;
      if (val->type & PE_REGS_ARG) {
        if (prev) {
          prev->next = next;
          val->next = NULL;
          pe_reg_val_free(val);
        } else {
          new_regs->vals = next;
          pe_reg_val_free(val);
        }
      } else {
        prev = val;
      }
    }
  }

  /* Whatever it is, it's copied for a QUEUE entry */
  for (; pe_regs; pe_regs = pe_regs->prev) {
    for (val = pe_regs->vals; val; val = val->next) {
      if (val->type & copytypes) {
        if (val->type & (PE_REGS_SWITCH | PE_REGS_ITER)) {
          /* It is t<num> or n<num>. Bump it up as necessary. */
          if (sscanf(val->name, "%c%d", &itype, &inum) == 2) {
            inum += (val->type & PE_REGS_SWITCH) ? smax : imax;
            if (*(val->name) == 'T') {
              if (val->type & PE_REGS_SWITCH) {
                if (inum >= scount)
                  scount = inum + 1;
              } else {
                if (inum >= icount)
                  icount = inum + 1;
              }
            }
            if (inum >= 0 && inum < MAX_ITERS) {
              snprintf(numbuff, sizeof numbuff, "%c%d", itype, inum);
              if (val->type & PE_REGS_STR) {
                pe_regs_set(new_regs, val->type & andflags, numbuff,
                            val->val.sval);
              } else {
                pe_regs_set_int(new_regs, val->type & andflags, numbuff,
                                val->val.ival);
              }
            }
          }
        } else {
          /* Set, but maybe don't override. */
          if (val->type & PE_REGS_STR) {
            pe_regs_set_if(new_regs, val->type & andflags, val->name,
                           val->val.sval, override);
          } else {
            pe_regs_set_int_if(new_regs, val->type & andflags, val->name,
                               val->val.ival, override);
          }
        }
      }
    }
    smax = scount;
    imax = icount;
    if (pe_regs->flags & PE_REGS_ARG) {
      /* Only the most recent %0-%9 count */
      copytypes &= ~PE_REGS_ARG;
    }
    override = 0;
  }
}

/** Does PE_REGS have a register stack of a given type, within the
 * appropriate scope? This checks three things: For a PE_REGS with the
 * appropriate type, for a stopping pe_regs (NEWATTR, etc), and for
 * a value with the appropriate type.
 *
 * \param pe_info The NEW_PE_INFO to check.
 * \param type The type to find.
 * \retval 1 Has the requested type.
 * \retval 0 Doesn't have it.
 */
int
pi_regs_has_type(NEW_PE_INFO *pe_info, int type)
{
  PE_REGS *pe_regs;
  PE_REG_VAL *val;
  int breaker;

  /* What flag will stop this search up? */
  switch (type) {
  case PE_REGS_Q:
    breaker = PE_REGS_QSTOP;
    break;
  default:
    breaker = PE_REGS_NEWATTR;
  }
  pe_regs = pe_info->regvals;

  while (pe_regs) {
    if (pe_regs->flags & type) {
      val = pe_regs->vals;
      while (val) {
        if (val->type & type)
          return 1;
        val = val->next;
      }
    }
    if (pe_regs->flags & breaker &&
        (type != PE_REGS_ARG || !(pe_regs->flags & PE_REGS_ARGPASS)))
      return 0;
    pe_regs = pe_regs->prev;
  }
  return 0;
}

/** Is a string a valid name for a Q-register?
 *
 * \param lckey The key to check
 * \return 0 not valid
 * \return 1 valid
 */
int
pi_regs_valid_key(const char *lckey)
{
  char key[PE_KEY_LEN];
  strupper_r(lckey, key, sizeof key);
  return (strlen(key) <= PE_KEY_LEN && *key && (key[0] != '-' || key[1]) &&
          good_atr_name(key));
}

extern char atr_name_table[UCHAR_MAX + 1];

void
pi_regs_normalize_key(char *lckey)
{
  if (!lckey || !*lckey)
    return;
  if (lckey[0] == '-' && !lckey[1]) {
    /* 1-character key that is only - ? */
    lckey[0] = '?';
  }

  for (; lckey && *lckey; lckey++) {
    if (islower(*lckey))
      *lckey = toupper(*lckey);
    if (!atr_name_table[*lckey]) {
      *lckey = '?';
    }
  }
}

/** Set a q-register value in the appropriate PE_REGS context.
 *
 * If there is no available appropriate context to set a Q-register in,
 * then create a toplevel one for the pe_info structure.
 *
 * \param pe_info The current PE_INFO struct.
 * \param key Q-register name.
 * \param val Q-register value.
 * \retval 1 No problems setting
 * \retval 0 Too many registers set.
 */
int
pi_regs_setq(NEW_PE_INFO *pe_info, const char *key, const char *val)
{
  PE_REGS *pe_regs = pe_info->regvals;
  PE_REGS *pe_tmp = NULL;
  int count = 0;
  while (pe_regs) {
    if ((pe_regs->flags & (PE_REGS_Q | PE_REGS_LET)) == PE_REGS_Q) {
      count = pe_regs->qcount;
      break;
    }
    pe_regs = pe_regs->prev;
  }
  /* Single-character keys ignore attrcount. */
  if ((count >= MAX_NAMED_QREGS) && is_named_register(key) &&
      !PE_Getq(pe_info, key)) {
    return 0;
  }
  /* Find the p_regs to setq() in. */
  pe_regs = pe_info->regvals;
  while (pe_regs) {
    pe_tmp = pe_regs;
    if (pe_regs->flags & PE_REGS_Q) {
      if (pe_regs->flags & PE_REGS_LET) {
        if (pe_regs_get(pe_regs, PE_REGS_Q, key)) {
          break;
        }
      } else {
        break;
      }
    }
    pe_regs = pe_regs->prev;
  }
  if (pe_regs == NULL) {
    pe_regs = pe_regs_create(PE_REGS_QUEUE, "pe_regs_setq");
    if (pe_tmp) {
      pe_tmp->prev = pe_regs;
    } else {
      pe_info->regvals = pe_regs;
    }
  }
  pe_regs_set(pe_regs, PE_REGS_Q, key, val);
  return 1;
}

const char *
pi_regs_getq(NEW_PE_INFO *pe_info, const char *key)
{
  const char *ret;
  PE_REGS *pe_regs = pe_info->regvals;
  while (pe_regs) {
    if (pe_regs->flags & PE_REGS_Q) {
      ret = pe_regs_get(pe_regs, PE_REGS_Q, key);
      if (ret)
        return ret;
    }
    /* If it's marked QSTOP, it stops. */
    if (pe_regs->flags & PE_REGS_QSTOP) {
      return NULL;
    }
    pe_regs = pe_regs->prev;
  }
  return NULL;
}

/* REGEXPS */
void
pe_regs_set_rx_context(PE_REGS *pe_regs, int pe_reg_flags, pcre2_code *re_code,
                       pcre2_match_data *md, int re_subpatterns)
{
  uint32_t i, namecount, entrysize;
  PCRE2_SPTR nametable;
  char buff[BUFFER_LEN];

  if (re_subpatterns < 0) {
    return;
  }

  if (!pe_reg_flags) {
    pe_reg_flags = PE_REGS_REGEXP;
  }

  /* We assume every captured pattern is used. */
  /* Copy all the numbered captures over */
  for (i = 0; i < (uint32_t) re_subpatterns && i < 1000; i++) {
    PCRE2_SIZE blen = BUFFER_LEN;
    buff[0] = '\0';
    pcre2_substring_copy_bynumber(md, i, (PCRE2_UCHAR *) buff, &blen);
    pe_regs_set(pe_regs, pe_reg_flags, pe_regs_intname(i), buff);
  }

  /* Copy all the named captures over. */
  if (pcre2_pattern_info(re_code, PCRE2_INFO_NAMECOUNT, &namecount) != 0) {
    return;
  }
  if (namecount == 0) {
    return;
  }
  if (pcre2_pattern_info(re_code, PCRE2_INFO_NAMEENTRYSIZE, &entrysize) != 0) {
    return;
  }
  if (pcre2_pattern_info(re_code, PCRE2_INFO_NAMETABLE, &nametable) != 0) {
    return;
  }

  for (i = 0; i < namecount; i++) {
    PCRE2_SIZE blen = BUFFER_LEN;
    PCRE2_SPTR entry = nametable + (entrysize * i);

    buff[0] = '\0';
    if (pcre2_substring_copy_byname(md, entry + 2, (PCRE2_UCHAR *) buff,
                                    &blen) == 0) {
      pe_regs_set(pe_regs, pe_reg_flags, (char *) entry + 2, buff);
    }
  }
}

void
pe_regs_set_rx_context_ansi(PE_REGS *pe_regs, int pe_reg_flags,
                            pcre2_code *re_code, pcre2_match_data *md,
                            int re_subpatterns, struct _ansi_string *re_from)
{
  uint32_t i, namecount, entrysize;
  PCRE2_SPTR nametable;
  int num;
  char buff[BUFFER_LEN], *bp;

  if (!re_from) {
    return;
  }
  if (re_subpatterns < 0) {
    return;
  }

  if (!pe_reg_flags) {
    pe_reg_flags = PE_REGS_REGEXP;
  }

  /* We assume every captured pattern is used. */
  /* Copy all the numbered captures over */
  for (i = 0; i < (uint32_t) re_subpatterns && i < 1000; i++) {
    bp = buff;
    ansi_pcre_copy_substring(re_from, md, re_subpatterns, i, 1, buff, &bp);
    *bp = '\0';
    pe_regs_set(pe_regs, pe_reg_flags, pe_regs_intname(i), buff);
  }

  /* Copy all the named captures over. */
  if (pcre2_pattern_info(re_code, PCRE2_INFO_NAMECOUNT, &namecount) != 0) {
    return;
  }
  if (namecount == 0) {
    return;
  }
  if (pcre2_pattern_info(re_code, PCRE2_INFO_NAMEENTRYSIZE, &entrysize) != 0) {
    return;
  }
  if (pcre2_pattern_info(re_code, PCRE2_INFO_NAMETABLE, &nametable) != 0) {
    return;
  }

  for (i = 0; i < namecount; i++) {
    PCRE2_SPTR entry = nametable + (entrysize * i);
    num = (entry[0] << 8) + entry[1];
    bp = buff;
    ansi_pcre_copy_substring(re_from, md, re_subpatterns, num, 1, buff, &bp);
    *bp = '\0';
    pe_regs_set(pe_regs, pe_reg_flags, (char *) entry + 2, buff);
  }
}

/* Used by PE_Get_re, to get regexp values */
const char *
pi_regs_get_rx(NEW_PE_INFO *pe_info, const char *key)
{
  const char *ret;
  PE_REGS *pe_regs = pe_info->regvals;
  while (pe_regs) {
    if (pe_regs->flags & PE_REGS_REGEXP) {
      ret = pe_regs_get(pe_regs, PE_REGS_REGEXP, key);
      if (ret)
        return ret;
      /* Only check the _first_ PE_REGS_REGEXP. */
      return NULL;
    }
    /* If it's marked NEWATTR, it's done. */
    if (pe_regs->flags & PE_REGS_NEWATTR) {
      return NULL;
    }
    pe_regs = pe_regs->prev;
  }
  return NULL;
}

/* PE_Get_Itext and PE_Get_Stext, for iter (%i0) and switch (%$0) values. */
const char *
pi_regs_get_itext(NEW_PE_INFO *pe_info, int type, int lev)
{
  PE_REGS *pe_regs;
  char numbuff[10];
  const char *ret;

  pe_regs = pe_info->regvals;

  while (pe_regs) {
    if (pe_regs->flags & type) {
      snprintf(numbuff, 10, "t%d", lev);
      ret = pe_regs_get(pe_regs, type, numbuff);
      if (ret) {
        return ret;
      }
      lev--; /* Not this level, go up one. */
    }
    /* NEWATTR halts switch and itext. */
    if (pe_regs->flags & PE_REGS_NEWATTR) {
      return NULL;
    }
    pe_regs = pe_regs->prev;
  }
  return NULL;
}

/* PE_Get_Inum, for inum() values */
int
pi_regs_get_inum(NEW_PE_INFO *pe_info, int type, int lev)
{
  PE_REGS *pe_regs;
  char numbuff[10];
  int ret;

  pe_regs = pe_info->regvals;

  while (pe_regs) {
    if (pe_regs->flags & type) {
      snprintf(numbuff, 10, "n%d", lev);
      ret = pe_regs_get_int(pe_regs, type, numbuff);
      if (ret) {
        return ret;
      }
      lev--; /* Not this level, go up one. */
    }
    /* NEWATTR halts switch and itext. */
    if (pe_regs->flags & PE_REGS_NEWATTR) {
      return 0;
    }
    pe_regs = pe_regs->prev;
  }
  return 0;
}

/* PE_Get_Ilev and PE_Get_Slev */
int
pi_regs_get_ilev(NEW_PE_INFO *pe_info, int type)
{
  PE_REGS *pe_regs;
  PE_REG_VAL *val;

  int count = -1;

  pe_regs = pe_info->regvals;

  while (pe_regs) {
    if (pe_regs->flags & type) {
      val = pe_regs->vals;
      while (val) {
        if ((val->type & type) && *(val->name) == 'T') {
          count++;
        }
        val = val->next;
      }
    }
    /* NEWATTR halts switch and itext. */
    if (pe_regs->flags & PE_REGS_NEWATTR) {
      return count;
    }
    pe_regs = pe_regs->prev;
  }
  return count;
}

/**
 * Fast way to turn 0-9 into a string, while falling back on tprintf for
 * non-single-digit ones.
 * \param num Number of the name
 * \retval char * representation of the name.
 */
const char *
pe_regs_intname(int num)
{
  static char buff[32];
  if (num < 10 && num >= 0) {
    return envid[num];
  } else {
    snprintf(buff, sizeof buff, "%d", num);
    return buff;
  }
}

/* Set %0-%9 */
void
pe_regs_setenv(PE_REGS *pe_regs, int num, const char *val)
{
  const char *name = pe_regs_intname(num);
  pe_regs_set(pe_regs, PE_REGS_ARG, name, val);
}

/* Set %0-%9 without copying value */
void
pe_regs_setenv_nocopy(PE_REGS *pe_regs, int num, const char *val)
{
  const char *name = pe_regs_intname(num);
  pe_regs_set(pe_regs, PE_REGS_ARG | PE_REGS_NOCOPY, name, val);
}

/* Get %0-%9. Only the bottommost PE_REGS_ARG is checked. It stops at NEWATTR */
const char *
pi_regs_get_env(NEW_PE_INFO *pe_info, const char *name)
{
  PE_REGS *pe_regs;
  const char *ret;

  pe_regs = pe_info->regvals;

  while (pe_regs) {
    if (pe_regs->flags & PE_REGS_ARG) {
      ret = pe_regs_get(pe_regs, PE_REGS_ARG, name);
      return ret;
    }
    /* NEWATTR without ARGPASS halts switch and itext. */
    if ((pe_regs->flags & (PE_REGS_NEWATTR | PE_REGS_ARGPASS)) ==
        (PE_REGS_NEWATTR)) {
      return NULL;
    }
    pe_regs = pe_regs->prev;
  }
  return NULL;
}

/* Return number of stack args (%0-%9) present, for %+. Powers PE_Get_Envc */
int
pi_regs_get_envc(NEW_PE_INFO *pe_info)
{
  PE_REGS *pe_regs;
  PE_REG_VAL *val;
  int max, num;
  max = 0;

  pe_regs = pe_info->regvals;

  while (pe_regs) {
    if (pe_regs->flags & PE_REGS_ARG) {
      for (val = pe_regs->vals; val; val = val->next) {
        if (val->type & PE_REGS_ARG) {
          if (sscanf(val->name, "%d", &num) == 1) {
            /* only check numeric args, ignore named ones */
            if (num >= max) {
              max = num + 1; /* %0 is 1 arg */
            }
          }
        }
      }
      return max;
    }
    /* NEWATTR without ARGPASS halts switch and itext. */
    if ((pe_regs->flags & (PE_REGS_NEWATTR | PE_REGS_ARGPASS)) ==
        (PE_REGS_NEWATTR)) {
      return 0;
    }
    pe_regs = pe_regs->prev;
  }
  return 0;
}

/* Table of interesting characters for process_expression() */
extern char active_table[UCHAR_MAX + 1];

#ifdef WIN32
#pragma warning(disable : 4761) /* NJG: disable warning re conversion */
#endif

/** Free a pe_info at the end of its use. Note that a pe_info may be in use
 ** in more than one place, in which case this function simply decrements the
 ** refcounter. Memory is only freed when the counter hits 0
 * \param pe_info the pe_info to free
 */
void
free_pe_info(NEW_PE_INFO *pe_info)
{
  PE_REGS *pe_regs;

  if (!pe_info)
    return;

  pe_info->refcount--;
  if (pe_info->refcount > 0)
    return; /* Still in use */

  while (pe_info->regvals) {
    pe_regs = pe_info->regvals;
    pe_info->regvals = pe_regs->prev;
    pe_regs_free(pe_regs);
  }

  if (pe_info->cmd_raw) {
    mush_free(pe_info->cmd_raw, "string");
  }
  if (pe_info->cmd_evaled) {
    mush_free(pe_info->cmd_evaled, "string");
  }
  if (pe_info->attrname) {
    mush_free(pe_info->attrname, "string");
  }

#ifdef DEBUG
  mush_free(pe_info, pe_info->name);
#else
  mush_free(pe_info, "new_pe_info");
#endif

  return;
}

/** Create a new pe_info
 * \param name name of the calling function, for memory checking
 */
NEW_PE_INFO *
make_pe_info(char *name __attribute__((__unused__)))
{
  NEW_PE_INFO *pe_info;

#ifdef DEBUG
  pe_info = mush_malloc(sizeof(NEW_PE_INFO), name);
#else
  pe_info = mush_malloc(sizeof(NEW_PE_INFO), "new_pe_info");
#endif
  if (!pe_info)
    mush_panic("Unable to allocate memory in make_pe_info");

  pe_info->fun_invocations = 0;
  pe_info->fun_recursions = 0;
  pe_info->call_depth = 0;

  pe_info->debug_strings = NULL;
  pe_info->debugging = 0;
  pe_info->nest_depth = 0;

  pe_info->attrname = NULL;

  pe_info->regvals = pe_regs_create(PE_REGS_QUEUE, "make_pe_info");

  pe_info->cmd_raw = NULL;
  pe_info->cmd_evaled = NULL;

  pe_info->refcount = 1;
#ifdef DEBUG
  strcpy(pe_info->name, name);
#endif

  return pe_info;
}

/** Create a new pe_info based on an existing pe_info. Depending on flags, we
 ** may simply increase the refcount of the existing pe_info and return
 ** that, or we may create a new pe_info, possibly copying some information
 ** from the existing pe_info into the new one
 * \param old_pe_info the original pe_info to use as a base for the new one
 * \param flags PE_INFO_* flags to determine exactly what to do
 * \param pe_regs a pe_regs struct with environment for the new pe_info
 * \retval a new pe_info
 */
NEW_PE_INFO *
pe_info_from(NEW_PE_INFO *old_pe_info, int flags, PE_REGS *pe_regs)
{
  NEW_PE_INFO *pe_info;

  if (flags & PE_INFO_SHARE) {
    /* Don't create a new pe_info, just increase the refcount for the existing
       one
       and return that. Used for inplace queue entries */
    /* Warning: Any function calling this with pe_regs also needs to set
     * MQUE->regvals to pe_regs. */
    if (!old_pe_info) {
      /* No existing one to share, so make a new one */
      pe_info = make_pe_info("pe_info-from_old-share");
    } else {
      pe_info = old_pe_info;
      pe_info->refcount++;
    }
    return pe_info;
  }

  if (flags & PE_INFO_CLONE) {
    /* Clone all the pertinent information in the original pe_info (env, q-reg,
       itext/stext info),
       reset all the counters (like function invocation limit). Used for cmds
       that queue a
       new actionlist for the current executor */
    pe_info = make_pe_info("pe_info-from_old-clone");
    if (!old_pe_info) {
      if (pe_regs) {
        pe_regs->prev = NULL;
        pe_regs_copystack(pe_info->regvals, pe_regs, PE_REGS_QUEUE, 0);
      }
      return pe_info; /* nothing to do */
    }

    /* OK, copy everything over */
    /* Copy the Q-registers, @switch, @dol and env over to the new pe_info. */
    if (pe_regs) {
      if (old_pe_info)
        pe_regs->prev = old_pe_info->regvals;
      else
        pe_regs->prev = NULL;
      pe_regs_copystack(pe_info->regvals, pe_regs, PE_REGS_QUEUE, 0);
      pe_regs->prev = NULL;
    } else if (old_pe_info) {
      pe_regs_copystack(pe_info->regvals, old_pe_info->regvals, PE_REGS_QUEUE,
                        0);
    }

    return pe_info;
  }

  /* Make a new pe_info, and possibly add stack or q-registers to it, either
   * from the old pe_info or from the args passed. Used for most things queued
   * by the hardcode like @a-attributes, or queue entries with a different
   * executor (@trigger) */

  pe_info = make_pe_info("pe_info-from_old-generic");
  if (old_pe_info) {
    if (flags & PE_INFO_COPY_ENV) {
      pe_regs_copystack(pe_info->regvals, old_pe_info->regvals, PE_REGS_ARG, 0);
    }
    /* Copy Q-registers. */
    if (flags & PE_INFO_COPY_QREG) {
      pe_regs_copystack(pe_info->regvals, old_pe_info->regvals, PE_REGS_Q, 0);
    }
    if (flags & PE_INFO_COPY_CMDS) {
      if (old_pe_info->cmd_raw) {
        pe_info->cmd_raw = mush_strdup(old_pe_info->cmd_raw, "string");
      }
      if (old_pe_info->cmd_evaled) {
        pe_info->cmd_evaled = mush_strdup(old_pe_info->cmd_evaled, "string");
      }
    }
  }

  /* Whatever we do, we copy the passed pe_regs. */
  if (pe_regs) {
    pe_regs_copystack(pe_info->regvals, pe_regs, PE_REGS_QUEUE, 1);
  }

  return pe_info;
}

/** Function and other substitution evaluation.
 * This is the PennMUSH function/expression parser. Big stuff.
 *
 * All results are returned in buff, at the point *bp. bp is likely
 * not equal to buff, so make no assumptions about writing at the
 * start of the buffer.  *bp must be updated to point at the next
 * place to be filled (ala safe_str() and safe_chr()).  Be very
 * careful about not overflowing buff; use of safe_str() and safe_chr()
 * for all writes into buff is highly recommended.
 *
 * nargs is the count of the number of arguments passed to the function,
 * and args is an array of pointers to them.  args will have at least
 * nargs elements, or 10 elements, whichever is greater.  The first ten
 * elements are initialized to NULL for ease of porting functions from
 * the old style, but relying on such is considered bad form.
 * The argument strings are stored in BUFFER_LEN buffers, but reliance
 * on that size is also considered bad form.  The argument strings may
 * be modified, but modifying the pointers to the argument strings will
 * cause crashes.
 *
 * executor corresponds to %!, the object invoking the function.
 * caller   corresponds to %@, the last object to do a U() or similar.
 * enactor  corresponds to %#, the object that started the whole mess.
 * Note that fun_ufun() and similar must swap around these parameters
 * in calling process_expression(); no checks are made in the parser
 * itself to maintain these values.
 *
 * called_as contains a pointer to the name of the function called
 * (taken from the function table).  This may be used to distinguish
 * multiple functions which use the same C function for implementation.
 *
 * pe_info holds context information used by the parser.  It should
 * be passed untouched to process_expression(), if it is called.
 * pe_info should be treated as a black box; its structure and contents
 * may change without notice.
 *
 * Normally, p_e() returns 0. It returns 1 upon hitting the CPU time limit.
 *
 * \param buff buffer to store returns of parsing.
 * \param bp pointer to pointer into buff marking insert position.
 * \param str string to parse.
 * \param executor dbref of the object invoking the function.
 * \param caller dbref of  the last object to use u()
 * \param enactor dbref of the enactor.
 * \param eflags flags to control what is evaluated.
 * \param tflags flags to control what terminates an expression.
 * \param pe_info pointer to parser context data.
 * \retval 0 success.
 * \retval 1 CPU time limit exceeded.
 */
int
process_expression(char *buff, char **bp, char const **str, dbref executor,
                   dbref caller, dbref enactor, int eflags, int tflags,
                   NEW_PE_INFO *pe_info)
{
  int debugging = 0, made_info = 0;
  char *debugstr = NULL, *sourcestr = NULL;
  char *realbuff = NULL, *realbp = NULL;
  int gender = -1;
  int inum_this;
  char *startpos = *bp;
  int had_space = 0;
  char temp[3];
  char qv[2] = "a";
  const char *qval;
  int temp_eflags;
  int retval = 0;
  int old_debugging = 0;
  PE_REGS *pe_regs;
  const char *stmp;
  int itmp;
  int tags = 0;
  /* Part of r1628's deprecation of unescaped commas as the final arg of a
   * function,
   * added 17 Sep 2012. Remove when this behaviour is removed. */
  static char *lca_func_name = NULL;
  /* End of r1628's deprecation */

  if (!buff || !bp || !str || !*str)
    return 0;
  if (cpu_time_limit_hit) {
    if (!cpu_limit_warning_sent) {
      cpu_limit_warning_sent = 1;
      /* Can't just put #-1 CPU USAGE EXCEEDED in buff here, because
       * it might never get displayed.
       */
      if (GoodObject(enactor) && !Quiet(enactor))
        notify(enactor, T("CPU usage exceeded."));
      do_rawlog(
        LT_TRACE,
        "CPU time limit exceeded. enactor=#%d executor=#%d caller=#%d code=%s",
        enactor, executor, caller, *str);
    }
    return 1;
  }
  if (Halted(executor))
    eflags = PE_NOTHING;
  if (eflags & PE_COMPRESS_SPACES)
    while (**str == ' ')
      (*str)++;
  if (!*str)
    return 0;

  if (!pe_info) {
    made_info = 1;
    pe_info = make_pe_info("pe_info-p_e");
  } else {
    old_debugging = pe_info->debugging;
    if (caller != executor)
      pe_info->debugging = 0;
  }

  /* If we've been asked to evaluate, log the expression if:
   * (a) the last thing we logged wasn't an expression, and
   * (b) this expression isn't a substring of the last thing we logged
   */
  if ((eflags & PE_EVALUATE) &&
      ((last_activity_type() != LA_PE) || !strstr(last_activity(), *str))) {
    log_activity(LA_PE, executor, *str);
  }

  if (eflags != PE_NOTHING) {
    if (((*bp) - buff) > (BUFFER_LEN - SBUF_LEN)) {
      realbuff = buff;
      realbp = *bp;
      buff = mush_malloc(BUFFER_LEN, "process_expression.buffer_extension");
      *bp = buff;
      startpos = buff;
    }
  }

  if (CALL_LIMIT && (pe_info->call_depth++ > CALL_LIMIT)) {
    const char *e_msg;
    size_t e_len;
    e_msg = T(e_call);
    e_len = strlen(e_msg);
    if ((buff + e_len > *bp) || strcmp(e_msg, *bp - e_len))
      safe_strl(e_msg, e_len, buff, bp);
    goto exit_sequence;
  }

  if (eflags & PE_DEBUG)
    pe_info->debugging = 1;
  else if (eflags & PE_NODEBUG)
    pe_info->debugging = -1;

  if (eflags != PE_NOTHING) {
    debugging =
      ((Debug(executor) && pe_info->debugging != -1) ||
       (pe_info->debugging == 1)) &&
      (Connected(Owner(executor)) || atr_get(executor, "DEBUGFORWARDLIST"));
    if (debugging) {
      int j;
      char *debugp;
      char const *mark;
      Debug_Info *node;

      debugstr = mush_malloc(BUFFER_LEN, "process_expression.debug_source");
      debugp = debugstr;
      safe_dbref(executor, debugstr, &debugp);
      safe_chr('!', debugstr, &debugp);
      for (j = 0; j <= pe_info->nest_depth; j++)
        safe_chr(' ', debugstr, &debugp);
      sourcestr = debugp;
      mark = *str;
      process_expression(debugstr, &debugp, str, executor, caller, enactor,
                         PE_NOTHING, tflags, pe_info);
      *str = mark;
      if (eflags & PE_COMPRESS_SPACES)
        while ((debugp > sourcestr) && (debugp[-1] == ' '))
          debugp--;
      *debugp = '\0';
      node = mush_malloc(sizeof(Debug_Info), "process_expression.debug_node");
      node->string = debugstr;
      node->executor = executor;
      node->prev = pe_info->debug_strings;
      node->next = NULL;
      if (node->prev)
        node->prev->next = node;
      pe_info->debug_strings = node;
      pe_info->nest_depth++;
    }
  }

  /* Only strip command braces if the first character is a brace. */
  if (**str != '{')
    eflags &= ~PE_COMMAND_BRACES;

  for (;;) {
    /* Find the first "interesting" character */
    {
      char const *pos = *str;
      int len, len2;

#ifdef HAVE_SSE42
      /* Characters that the parser looks for. Same as active_table from
       * tables.c */
      static const char interesting[16] __attribute__((__aligned__(16))) =
        "%{[(\\ }>]),;=$\x1B";
      __m128i a = _mm_load_si128((const __m128i *) interesting);

      while (1) {
        __m128i b = _mm_loadu_si128((__m128i *) *str);
        int z = _mm_cmpistrz(a, b,
                             _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY |
                               _SIDD_LEAST_SIGNIFICANT);
        int i = _mm_cmpistri(a, b,
                             _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY |
                               _SIDD_LEAST_SIGNIFICANT);
        if (i != 16) {
          *str += i;
          break;
        }
        if (z) {
          /* At end of the string with no interesting characters remaining. Find
           * the 0 byte */
          while (**str)
            (*str)++;
          break;
        }
        *str += 16;
      }

      // fprintf(stderr, "Skipped over '%.*s' to '%c'\n", (int)(*str - pos),
      // pos,
      // **str);

#else
      /* Inlined strcspn() equivalent, to save on overhead and portability */
      while (!active_table[**str])
        (*str)++;
#endif

      /* Inlined safe_str(), since the source string
       * may not be null terminated */
      len = *str - pos;
      len2 = BUFFER_LEN - 1 - (*bp - buff);
      if (len > len2)
        len = len2;
      if (len >= 0) {
        memcpy(*bp, pos, len);
        *bp += len;
      }
    }

    switch (**str) {
    /* Possible terminators */
    case '}':
      if (tflags & PT_BRACE)
        goto exit_sequence;
      break;
    case ']':
      if (tflags & PT_BRACKET)
        goto exit_sequence;
      break;
    case ')':
      if (tflags & PT_PAREN)
        goto exit_sequence;
      break;
    case ',':
      if (tflags & PT_COMMA)
        goto exit_sequence;
      /* Part of r1628's deprecation of unescaped commas as the final arg of a
       * function,
       * added 17 Sep 2012. Remove when this behaviour is removed. */
      else if (tflags & PT_NOT_COMMA) {
        if (pe_info && pe_info->attrname)
          notify_format(Owner(executor),
                        "Unescaped comma in final arg of %s "
                        "by #%d in %s. This behavior is "
                        "deprecated.",
                        lca_func_name, executor, pe_info->attrname);
        else
          notify_format(Owner(executor),
                        "Unescaped comma in final arg of %s "
                        "by #%d. This behavior is deprecated.",
                        lca_func_name, executor);
        tflags &= ~PT_NOT_COMMA;
      }
      /* End of r1628's deprecation */
      break;
    case ';':
      if (tflags & PT_SEMI)
        goto exit_sequence;
      break;
    case '=':
      if (tflags & PT_EQUALS)
        goto exit_sequence;
      break;
    case ' ':
      if (tflags & PT_SPACE)
        goto exit_sequence;
      break;
    case '>':
      if (tflags & PT_GT)
        goto exit_sequence;
      break;
    case '\0':
      goto exit_sequence;
    }

    switch (**str) {
    case TAG_START:
      /* Skip over until TAG_END. */
      for (; *str && **str && **str != TAG_END; (*str)++)
        safe_chr(**str, buff, bp);
      if (*str && **str) {
        safe_chr(**str, buff, bp);
        (*str)++;
      }
      break;
    case ESC_CHAR: /* ANSI escapes. */
      /* Skip over until the 'm' that matches the end. */
      for (; *str && **str && **str != 'm'; (*str)++)
        safe_chr(**str, buff, bp);
      if (*str && **str) {
        safe_chr(**str, buff, bp);
        (*str)++;
      }
      break;
    case '$': /* Dollar subs for regedit() */
      if ((eflags & (PE_DOLLAR | PE_EVALUATE)) == (PE_DOLLAR | PE_EVALUATE) &&
          PE_HAS_REGTYPE(pe_info, PE_REGS_REGEXP)) {
        char subspace[BUFFER_LEN];

        (*str)++;
        /* Check the first character after the $ for a number */
        if (isdigit(**str)) {
          subspace[0] = **str;
          subspace[1] = '\0';
          (*str)++;
          safe_str(PE_Get_re(pe_info, subspace), buff, bp);
        } else if (**str == '<') {
          /* Look for a named or numbered subexpression */
          char *nbp = subspace;
          (*str)++;
          if (process_expression(subspace, &nbp, str, executor, caller, enactor,
                                 eflags & ~PE_STRIP_BRACES, PT_GT, pe_info)) {
            retval = 1;
            break;
          }
          *nbp = '\0';
          safe_str(PE_Get_re(pe_info, subspace), buff, bp);
          if (**str == '>')
            (*str)++;
        } else {
          safe_chr('$', buff, bp);
        }
      } else {
        safe_chr('$', buff, bp);
        (*str)++;
        if ((**str) == '<') {
          if (process_expression(buff, bp, str, executor, caller, enactor,
                                 eflags & ~PE_STRIP_BRACES, PT_GT, pe_info)) {
            retval = 1;
            break;
          }
        }
      }
      break;
    case '%': /* Percent substitutions */
      if (eflags & PE_LITERAL) {
        /* Show literal % in lit() */
        safe_chr('%', buff, bp);
        (*str)++;
        break;
      }
      if (!(eflags & PE_EVALUATE) || (*bp - buff >= BUFFER_LEN - 1)) {
        /* peak -- % escapes (at least) one character */
        char savec;

        safe_chr('%', buff, bp);
        (*str)++;
        savec = **str;
        if (!savec)
          goto exit_sequence;
        safe_chr(savec, buff, bp);
        (*str)++;
        switch (savec) {
        case 'Q':
        case 'q':
          /* Two characters, or if there's a <, then up until the next > */
          savec = **str;
          if (!savec)
            goto exit_sequence;
          safe_chr(savec, buff, bp);
          if (savec == '<') {
            (*str)++;
            process_expression(buff, bp, str, executor, caller, enactor,
                               eflags & ~PE_STRIP_BRACES, PT_GT, pe_info);
          } else {
            (*str)++;
          }
          break;
        case 'V':
        case 'v':
        case 'W':
        case 'w':
        case 'X':
        case 'x':
          /* These sequences escape two characters */
          savec = **str;
          if (!savec)
            goto exit_sequence;
          safe_chr(savec, buff, bp);
          (*str)++;
        }
        break;
      } else {
        char savec, nextc;
        char *savepos;
        ATTR *attrib;

        (*str)++;
        savec = **str;
        if (!savec) {
          /* Line ended in %, so treat it as literal */
          safe_chr('%', buff, bp);
          goto exit_sequence;
        }
        savepos = *bp;
        (*str)++;

        switch (savec) {
        case '%': /* %% - a real % */
          safe_chr('%', buff, bp);
          break;
        case ' ': /* "% " for more natural typing */
          safe_str("% ", buff, bp);
          break;
        case '!': /* executor dbref */
          safe_dbref(executor, buff, bp);
          break;
        case '@': /* caller dbref */
          safe_dbref(caller, buff, bp);
          break;
        case '#': /* enactor dbref */
          safe_dbref(enactor, buff, bp);
          break;
        case ':': /* enactor unique id */
          if (GoodObject(enactor)) {
            safe_dbref(enactor, buff, bp);
            safe_chr(':', buff, bp);
            safe_integer(CreTime(enactor), buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case '?': /* function limits */
          if (pe_info) {
            safe_integer(pe_info->fun_invocations, buff, bp);
            safe_chr(' ', buff, bp);
            safe_integer(pe_info->fun_recursions, buff, bp);
          } else {
            safe_str("0 0", buff, bp);
          }
          break;
        case '~': /* enactor accented name */
          if (GoodObject(enactor)) {
            safe_str(accented_name(enactor), buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case '+': /* argument count */
          if (pe_info) {
            safe_integer(PE_Get_Envc(pe_info), buff, bp);
          } else {
            safe_integer(0, buff, bp);
          }
          break;
        case '=':
          if (pe_info)
            safe_str(pe_info->attrname, buff, bp);
          break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': /* positional argument */
          stmp = PE_Get_Env(pe_info, savec - '0');
          if (stmp)
            safe_str(stmp, buff, bp);
          break;
        case 'A':
        case 'a': /* enactor absolute possessive pronoun */
          if (GoodObject(enactor)) {
            if (gender < 0)
              gender = get_gender(enactor);
            safe_str(absp[gender], buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case 'B':
        case 'b': /* blank space */
          safe_chr(' ', buff, bp);
          break;
        case 'C':
        case 'c': /* command line */
          safe_str(pe_info->cmd_raw, buff, bp);
          break;
        case 'e':
          nextc = **str;
          char atrName[BUFFER_LEN];
          char tmpBuff[BUFFER_LEN];
          memset(tmpBuff, 0, sizeof(tmpBuff));
          memset(atrName, 0, sizeof(atrName));
          char *nbp = atrName;
          if (!nextc)
            goto exit_sequence;
          (*str)++;
          if (nextc == '<') {

            if (process_expression(atrName, &nbp, str, executor, caller,
                                   enactor, eflags & ~PE_STRIP_BRACES, PT_GT,
                                   pe_info)) {
              retval = 1;
              break;
            }
            //*nbp = '\0';

            if (call_attrib(executor, remove_markup(upcasestr(atrName), NULL),
                            tmpBuff, executor, pe_info, NULL))
              safe_str(tmpBuff, buff, bp);

            if (**str == '>')
              (*str)++;
          }
          break;
        case 'I':
        case 'i':
          nextc = **str;
          if (!nextc)
            goto exit_sequence;
          (*str)++;
          itmp = PE_Get_Ilev(pe_info);
          if (itmp >= 0) {
            if (nextc == 'l' || nextc == 'L') {
              safe_str(PE_Get_Itext(pe_info, itmp), buff, bp);
              break;
            }
            if (!isdigit(nextc)) {
              safe_str(T(e_int), buff, bp);
              break;
            }
            inum_this = nextc - '0';
            if (inum_this < 0 || inum_this > itmp) {
              safe_str(T(e_argrange), buff, bp);
            } else {
              safe_str(PE_Get_Itext(pe_info, inum_this), buff, bp);
            }
          } else {
            safe_str(T(e_argrange), buff, bp);
          }
          break;
        case '$':
          nextc = **str;
          if (!nextc)
            goto exit_sequence;
          (*str)++;
          itmp = PE_Get_Slev(pe_info);
          if (itmp >= 0) {
            if (nextc == 'l' || nextc == 'L') {
              inum_this = itmp;
            } else if (!isdigit(nextc)) {
              safe_str(T(e_int), buff, bp);
              break;
            } else {
              inum_this = nextc - '0';
            }
            if (inum_this < 0 || inum_this > itmp) {
              safe_str(T(e_argrange), buff, bp);
            } else {
              safe_str(PE_Get_Stext(pe_info, inum_this), buff, bp);
            }
          } else {
            safe_str(T(e_argrange), buff, bp);
          }
          break;
        case 'U':
        case 'u':
          safe_str(pe_info->cmd_evaled, buff, bp);
          break;
        case 'L':
        case 'l': /* enactor location dbref */
          if (GoodObject(enactor)) {
            /* The security implications of this have
             * already been talked to death.  Deal. */
            safe_dbref(Location(enactor), buff, bp);
          } else {
            safe_str("#-1", buff, bp);
          }
          break;
        case 'N':
        case 'n': /* enactor name */
          if (GoodObject(enactor)) {
            safe_str(Name(enactor), buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case 'k':
        case 'K': /* enactor moniker (ansi'd name) */
          if (GoodObject(enactor))
            safe_str(ansi_name(enactor, 0, NULL, 0), buff, bp);
          else
            safe_str(T(e_notvis), buff, bp);
          break;
        case 'O':
        case 'o': /* enactor objective pronoun */
          if (GoodObject(enactor)) {
            if (gender < 0)
              gender = get_gender(enactor);
            safe_str(obj[gender], buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case 'P':
        case 'p': /* enactor possessive pronoun */
          if (GoodObject(enactor)) {
            if (gender < 0)
              gender = get_gender(enactor);
            safe_str(poss[gender], buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case 'Q':
        case 'q': /* temporary storage */
          nextc = **str;
          if (!nextc)
            goto exit_sequence;
          (*str)++;
          if (nextc == '<') {
            char subspace[BUFFER_LEN];
            char *nbp = subspace;
            if (process_expression(subspace, &nbp, str, executor, caller,
                                   enactor, eflags & ~PE_STRIP_BRACES, PT_GT,
                                   pe_info)) {
              retval = 1;
              break;
            }
            *nbp = '\0';
            qval = PE_Getq(pe_info, subspace);
            if (qval) {
              safe_str(qval, buff, bp);
            }
            if (**str == '>')
              (*str)++;
          } else {
            qv[0] = UPCASE(nextc);
            qval = PE_Getq(pe_info, qv);
            if (qval) {
              safe_str(qval, buff, bp);
            }
          }
          break;
        case 'R':
        case 'r': /* newline */
          safe_chr('\n', buff, bp);
          break;
        case 'S':
        case 's': /* enactor subjective pronoun */
          if (GoodObject(enactor)) {
            if (gender < 0)
              gender = get_gender(enactor);
            safe_str(subj[gender], buff, bp);
          } else {
            safe_str(T(e_notvis), buff, bp);
          }
          break;
        case 'T':
        case 't': /* tab */
          safe_chr('\t', buff, bp);
          break;
        case 'V':
        case 'v':
        case 'W':
        case 'w':
        case 'X':
        case 'x': /* attribute substitution */
          nextc = **str;
          if (!nextc)
            goto exit_sequence;

          temp[0] = UPCASE(savec);
          temp[1] = UPCASE(nextc);
          temp[2] = '\0';
          attrib = atr_get(executor, temp);
          if (attrib)
            safe_str(atr_value(attrib), buff, bp);

          break;
        case 'Z':
          nextc = **str;
          if (!nextc)
            goto exit_sequence;
          qv[0] = UPCASE(nextc);
          (*str)++;
          char tmpBuf[BUFFER_LEN];
          memset(tmpBuf, 0, sizeof(tmpBuf));
          if (nextc == UPCASE(nextc)) {
            snprintf(tmpBuf, BUFFER_LEN, "COLOR`%s`HI", qv);
          } else {
            snprintf(tmpBuf, BUFFER_LEN, "COLOR`%s", qv);
          }
          attrib = atr_get(MASTER_ROOM, tmpBuf);
          if (attrib) {
            safe_str(atr_value(attrib), buff, bp);
          }
          break;
        case 'z':
          nextc = **str;
          if (!nextc)
            goto exit_sequence;

          // %zn returns the string to neutral by closing all tags
          if (UPCASE(nextc) == 'N') {
            while (tags > 0) {
              safe_str(close_tag("font"), buff, bp);
              tags--;
              break;
            }
          }
          qv[0] = nextc;

          (*str)++;
          memset(tmpBuf, 0, sizeof(tmpBuf));
          char *fgAtr;
          char *bgAtr;
          // Looking for the character to denote fg/bg set
          if (nextc == '*') {
            char tmpBuf2[BUFFER_LEN];
            memset(tmpBuf2,0,sizeof(tmpBuf2));
            // move the pointer forward

            // reallocate to the next character, check if its upper or lower
            // case
            //(*str)++;
            qv[0] = **str;
            if (qv[0] == UPCASE(qv[0])) {
              snprintf(tmpBuf, BUFFER_LEN, "COLOR`%s`HI", upcasestr(qv));
            } else {
              snprintf(tmpBuf, BUFFER_LEN, "COLOR`%s", upcasestr(qv));
            }
            // Foreground should be done, same again for the background

            // Get the appropriate fg attribute
            attrib = atr_get(MASTER_ROOM, tmpBuf);

            (*str)++;
            temp[0] = **str;
            if (temp[0] == UPCASE(temp[0])) {
              snprintf(tmpBuf2, BUFFER_LEN, "COLOR`%s`HI", upcasestr(temp));
            } else {
              snprintf(tmpBuf2, BUFFER_LEN, "COLOR`%s", upcasestr(temp));
            }
            // advance the pointer, we don't want that char any more.
            (*str)++;


            if (attrib) {
              fgAtr = safe_atr_value(attrib, "parse_atr");
              // Get the appropriate bg attribute
              ATTR *bgAttrib = atr_get(MASTER_ROOM, tmpBuf2);
              if (bgAttrib) {
                bgAtr = safe_atr_value(bgAttrib, "parse_atr");
                snprintf(tmpBuf, BUFFER_LEN, "font color=%s bgcolor=%s",
                         fgAtr, bgAtr);
                mush_free(bgAtr, "parse_atr");
              } else {
                // If we don't find anything, we want to default.
                snprintf(tmpBuf, BUFFER_LEN, "font color=%s bgcolor=#000000",
                         atr_value(attrib));
                
                
              }
              tags++;
              safe_str(open_tag(tmpBuf), buff, bp);
              mush_free(fgAtr, "parse_atr");
              break;
            }          
          }
          // Same as above but for single foreground colour.
          if (nextc == UPCASE(nextc)) {
            snprintf(tmpBuf, BUFFER_LEN, "COLOR`%s`HI", upcasestr(qv));
          } else {
            snprintf(tmpBuf, BUFFER_LEN, "COLOR`%s", upcasestr(qv));
          }
          attrib = atr_get(MASTER_ROOM, tmpBuf);
          if (attrib) {

            snprintf(tmpBuf, BUFFER_LEN, "font color=%s bgcolor=#000000",
                     atr_value(attrib));
            safe_str(open_tag(tmpBuf), buff, bp);
            tags++;
          }
          break;

        default: /* just copy */
          safe_chr(savec, buff, bp);
        }

        if (isupper(savec)) {
          savepos = skip_leading_ansi(savepos, *bp);
          if (savepos) {
            *savepos = UPCASE(*savepos);
          }
        }
      }
      break;
    case '{': /* "{}" parse group; recurse with no function check */
      if (CALL_LIMIT && (pe_info->call_depth > CALL_LIMIT)) {
        (*str)++;
        break;
      }
      if (eflags & PE_LITERAL) {
        safe_chr('{', buff, bp);
        (*str)++;
        break;
      }
      if (!(eflags & (PE_STRIP_BRACES | PE_COMMAND_BRACES)))
        safe_chr('{', buff, bp);
      (*str)++;
      if (process_expression(
            buff, bp, str, executor, caller, enactor,
            eflags & PE_COMMAND_BRACES
              ? (eflags & ~PE_COMMAND_BRACES)
              : (eflags & ~(PE_STRIP_BRACES | PE_FUNCTION_CHECK)),
            PT_BRACE, pe_info)) {
        retval = 1;
        break;
      }

      if (**str == '}') {
        if (!(eflags & (PE_STRIP_BRACES | PE_COMMAND_BRACES)))
          safe_chr('}', buff, bp);
        (*str)++;
      }
      /* Only strip one set of braces for commands */
      eflags &= ~PE_COMMAND_BRACES;
      break;
    case '[': /* "[]" parse group; recurse with mandatory function check */
      if (CALL_LIMIT && (pe_info->call_depth > CALL_LIMIT)) {
        (*str)++;
        break;
      }
      if (eflags & PE_LITERAL) {
        safe_chr('[', buff, bp);
        (*str)++;
        break;
      }
      if (!(eflags & PE_EVALUATE)) {
        safe_chr('[', buff, bp);
        temp_eflags = eflags & ~PE_STRIP_BRACES;
      } else
        temp_eflags = eflags | PE_FUNCTION_CHECK | PE_FUNCTION_MANDATORY;
      (*str)++;
      if (process_expression(buff, bp, str, executor, caller, enactor,
                             temp_eflags, PT_BRACKET, pe_info)) {
        retval = 1;
        break;
      }
      if (**str == ']') {
        if (!(eflags & PE_EVALUATE))
          safe_chr(']', buff, bp);
        (*str)++;
      }
      break;
    case '(': /* Function call */
      if (CALL_LIMIT && (pe_info->call_depth > CALL_LIMIT)) {
        (*str)++;
        break;
      }
      (*str)++;
      if (!(eflags & PE_EVALUATE) || !(eflags & PE_FUNCTION_CHECK)) {
        safe_chr('(', buff, bp);
        if (**str == ' ') {
          safe_chr(**str, buff, bp);
          (*str)++;
        }
        if (process_expression(buff, bp, str, executor, caller, enactor,
                               eflags & ~PE_STRIP_BRACES, PT_PAREN, pe_info))
          retval = 1;
        if (**str == ')') {
          if (eflags & PE_COMPRESS_SPACES && (*str)[-1] == ' ')
            safe_chr(' ', buff, bp);
          safe_chr(')', buff, bp);
          (*str)++;
        }
        break;
      } else {
        char *onearg;
        char *sargs[10];
        char **fargs;
        int sarglens[10];
        int *arglens;
        int args_alloced;
        int nfargs;
        int j;
        static char name[BUFFER_LEN];
        char *sp, *tp;
        FUN *fp;
        int temp_tflags;
        int denied;

        fargs = sargs;
        arglens = sarglens;
        for (j = 0; j < 10; j++) {
          fargs[j] = NULL;
          arglens[j] = 0;
        }
        args_alloced = 10;
        eflags &= ~PE_FUNCTION_CHECK;
        /* Get the function name */
        for (sp = startpos, tp = name; sp < *bp; sp++)
          safe_chr(UPCASE(*sp), name, &tp);
        *tp = '\0';
        fp = (eflags & PE_BUILTINONLY) ? builtin_func_hash_lookup(name)
                                       : func_hash_lookup(name);
        eflags &= ~PE_BUILTINONLY; /* Only applies to the outermost call */
        if (!fp) {
          if (eflags & PE_FUNCTION_MANDATORY) {
            char *suggestion;
            *bp = startpos;
            safe_str(T("#-1 FUNCTION ("), buff, bp);
            safe_str(name, buff, bp);
            safe_str(T(") NOT FOUND"), buff, bp);
            suggestion = suggest_name(name, "FUNCTIONS");
            if (suggestion) {
              safe_format(buff, bp, " DID YOU MEAN '%s'", suggestion);
              mush_free(suggestion, "string");
            }
            if (process_expression(name, &tp, str, executor, caller, enactor,
                                   PE_NOTHING, PT_PAREN, pe_info))
              retval = 1;
            if (**str == ')')
              (*str)++;
            break;
          }
          safe_chr('(', buff, bp);
          if (**str == ' ') {
            safe_chr(**str, buff, bp);
            (*str)++;
          }
          if (process_expression(buff, bp, str, executor, caller, enactor,
                                 eflags, PT_PAREN, pe_info)) {
            retval = 1;
            break;
          }
          if (**str == ')') {
            if (eflags & PE_COMPRESS_SPACES && (*str)[-1] == ' ')
              safe_chr(' ', buff, bp);
            safe_chr(')', buff, bp);
            (*str)++;
          }
          break;
        }
        *bp = startpos;

        /* Check for the invocation limit */
        if ((pe_info->fun_invocations >= FUNCTION_LIMIT) ||
            (global_fun_invocations >= FUNCTION_LIMIT * 5)) {
          const char *e_msg;
          size_t e_len;
          e_msg = T(e_invoke);
          e_len = strlen(e_msg);
          if ((buff + e_len > *bp) || strcmp(e_msg, *bp - e_len))
            safe_strl(e_msg, e_len, buff, bp);
          if (process_expression(name, &tp, str, executor, caller, enactor,
                                 PE_NOTHING, PT_PAREN, pe_info))
            retval = 1;
          if (**str == ')')
            (*str)++;
          break;
        }
        /* Check for the recursion limit */
        if ((pe_info->fun_recursions + 1 >= RECURSION_LIMIT) ||
            (global_fun_recursions + 1 >= RECURSION_LIMIT * 5)) {
          safe_str(T("#-1 FUNCTION RECURSION LIMIT EXCEEDED"), buff, bp);
          if (process_expression(name, &tp, str, executor, caller, enactor,
                                 PE_NOTHING, PT_PAREN, pe_info))
            retval = 1;
          if (**str == ')')
            (*str)++;
          break;
        }
        /* Get the arguments */
        temp_eflags = (eflags & ~PE_FUNCTION_MANDATORY) | PE_COMPRESS_SPACES |
                      PE_EVALUATE | PE_FUNCTION_CHECK;
        switch (fp->flags & FN_ARG_MASK) {
        case FN_LITERAL:
          temp_eflags |= PE_LITERAL;
        /* FALL THROUGH */
        case FN_NOPARSE:
          temp_eflags &=
            ~(PE_COMPRESS_SPACES | PE_EVALUATE | PE_FUNCTION_CHECK);
          break;
        }
        denied = !check_func(executor, fp);
        denied = denied || ((fp->flags & FN_USERFN) && !(eflags & PE_USERFN));
        if (denied)
          temp_eflags &=
            ~(PE_COMPRESS_SPACES | PE_EVALUATE | PE_FUNCTION_CHECK);
        temp_tflags = PT_COMMA | PT_PAREN;
        nfargs = 0;
        onearg = mush_malloc(BUFFER_LEN,
                             "process_expression.single_function_argument");
        do {
          char *argp;
          char *lca_safe_func_name = NULL;
          if ((fp->maxargs < 0) && ((nfargs + 1) >= -fp->maxargs)) {
            /* Part of r1628's deprecation of unescaped commas as the final arg
             * of a function,
             * added 17 Sep 2012. Remove when this behaviour is removed. */
            if (lca_func_name != NULL) {
              lca_safe_func_name = mush_strdup(lca_func_name, "lca_func_name");
            } else {
              lca_func_name = malloc(BUFFER_LEN);
            }
            if (fp->flags & FN_LITERAL)
              temp_tflags = PT_PAREN;
            else
              temp_tflags = PT_PAREN | PT_NOT_COMMA;
            strcpy(lca_func_name, fp->name);
            // temp_tflags = PT_PAREN;
            /* End of r1628's deprecation */
          }
          if (nfargs >= args_alloced) {
            char **nargs;
            int *narglens;
            nargs = mush_calloc(nfargs + 10, sizeof(char *),
                                "process_expression.function_arglist");
            narglens = mush_calloc(nfargs + 10, sizeof(int),
                                   "process_expression.function_arglens");
            for (j = 0; j < nfargs; j++) {
              nargs[j] = fargs[j];
              narglens[j] = arglens[j];
            }
            if (fargs != sargs)
              mush_free(fargs, "process_expression.function_arglist");
            if (arglens != sarglens)
              mush_free(arglens, "process_expression.function_arglens");
            fargs = nargs;
            arglens = narglens;
            args_alloced += 10;
          }
          fargs[nfargs] = mush_malloc_zero(
            BUFFER_LEN + SSE_OFFSET, "process_expression.function_argument");
          argp = onearg;
          if (process_expression(onearg, &argp, str, executor, caller, enactor,
                                 temp_eflags, temp_tflags, pe_info)) {
            retval = 1;
            nfargs++;
            /* Part of r1628's deprecation of unescaped commas as the final arg
             * of a function,
             * added 17 Sep 2012. Remove when this behaviour is removed. */
            if (lca_safe_func_name) {
              strcpy(lca_func_name, lca_safe_func_name);
              mush_free(lca_safe_func_name, "lca_func_name");
              lca_safe_func_name = NULL;
            }
            /* End of r1628's deprecation */
            goto free_func_args;
          }
          *argp = '\0';
          if (fp->flags & FN_STRIPANSI) {
            strcpy(fargs[nfargs], remove_markup(onearg, NULL));
          } else {
            strcpy(fargs[nfargs], onearg);
          }
          arglens[nfargs] = strlen(fargs[nfargs]);
          /* Part of r1628's deprecation of unescaped commas as the final arg of
           * a function,
           * added 17 Sep 2012. Remove when this behaviour is removed. */
          if (lca_safe_func_name) {
            strcpy(lca_func_name, lca_safe_func_name);
            mush_free(lca_safe_func_name, "lca_func_name");
            lca_safe_func_name = NULL;
          }
          /* End of r1628's deprecation */
          (*str)++;
          nfargs++;
        } while ((*str)[-1] == ',');
        if ((*str)[-1] != ')')
          (*str)--;

        /* Warn about deprecated functions */
        if (fp->flags & FN_DEPRECATED)
          notify_format(Owner(executor),
                        T("Deprecated function %s being used on object #%d."),
                        fp->name, executor);

        /* See if this function is enabled */
        /* Can't do this check earlier, because of possible side effects
         * from the functions.  Bah. */
        if (denied) {
          if (fp->flags & FN_DISABLED)
            safe_str(T(e_disabled), buff, bp);
          else
            safe_str(T(e_perm), buff, bp);
          goto free_func_args;
        } else {
          /* If we have the right number of args, eval the function.
           * Otherwise, return an error message.
           * Special case: zero args is recognized as one null arg.
           */
          if ((fp->minargs == 0) && (nfargs == 1) && !*fargs[0]) {
            mush_free(fargs[0], "process_expression.function_argument");
            fargs[0] = NULL;
            arglens[0] = 0;
            nfargs = 0;
          }
          if ((nfargs < fp->minargs) || (nfargs > abs(fp->maxargs))) {
            safe_format(buff, bp, T("#-1 FUNCTION (%s) EXPECTS "), fp->name);
            if (fp->minargs == abs(fp->maxargs)) {
              safe_integer(fp->minargs, buff, bp);
            } else if ((fp->minargs + 1) == abs(fp->maxargs)) {
              safe_integer(fp->minargs, buff, bp);
              safe_str(T(" OR "), buff, bp);
              safe_integer(abs(fp->maxargs), buff, bp);
            } else if (fp->maxargs == INT_MAX) {
              safe_str(T("AT LEAST "), buff, bp);
              safe_integer(fp->minargs, buff, bp);
            } else {
              safe_str(T("BETWEEN "), buff, bp);
              safe_integer(fp->minargs, buff, bp);
              safe_str(T(" AND "), buff, bp);
              safe_integer(abs(fp->maxargs), buff, bp);
            }
            safe_str(T(" ARGUMENTS BUT GOT "), buff, bp);
            safe_integer(nfargs, buff, bp);
          } else {
            char *fbuff, *fbp;

            global_fun_recursions++;
            pe_info->fun_recursions++;
            if (fp->flags & FN_LOCALIZE) {
              pe_regs =
                pe_regs_localize(pe_info, PE_REGS_LOCALQ, "process_expression");
            } else {
              pe_regs = NULL;
            }

            if (realbuff) {
              fbuff = realbuff;
              fbp = realbp;
            } else {
              fbuff = buff;
              fbp = *bp;
            }

            if (fp->flags & FN_BUILTIN) {
              global_fun_invocations++;
              pe_info->fun_invocations++;
              fp->where.fun(fp, fbuff, &fbp, nfargs, fargs, arglens, executor,
                            caller, enactor, fp->name, pe_info,
                            ((eflags & ~PE_FUNCTION_MANDATORY) | PE_DEFAULT));
              if (fp->flags & FN_LOGARGS) {
                char logstr[BUFFER_LEN];
                char *logp;
                int logi;
                logp = logstr;
                safe_str(fp->name, logstr, &logp);
                safe_chr('(', logstr, &logp);
                for (logi = 0; logi < nfargs; logi++) {
                  safe_str(fargs[logi], logstr, &logp);
                  if (logi + 1 < nfargs)
                    safe_chr(',', logstr, &logp);
                }
                safe_chr(')', logstr, &logp);
                *logp = '\0';
                do_log(LT_CMD, executor, caller, "%s", logstr);
              } else if (fp->flags & FN_LOGNAME)
                do_log(LT_CMD, executor, caller, "%s()", fp->name);
            } else {
              dbref thing;
              ATTR *attrib;
              global_fun_invocations++;
              pe_info->fun_invocations++;
              thing = fp->where.ufun->thing;
              attrib = atr_get(thing, fp->where.ufun->name);
              if (!attrib) {
                do_rawlog(LT_ERR,
                          "ERROR: @function (%s) without attribute (#%d/%s)",
                          fp->name, thing, fp->where.ufun->name);
                safe_str(T("#-1 @FUNCTION ("), buff, bp);
                safe_str(fp->name, buff, bp);
                safe_str(T(") MISSING ATTRIBUTE ("), buff, bp);
                safe_dbref(thing, buff, bp);
                safe_chr('/', buff, bp);
                safe_str(fp->where.ufun->name, buff, bp);
                safe_chr(')', buff, bp);
              } else {
                do_userfn(fbuff, &fbp, thing, attrib, nfargs, fargs, executor,
                          caller, enactor, pe_info, PE_USERFN);
              }
            }
            if (realbuff)
              realbp = fbp;
            else
              *bp = fbp;

            if (pe_regs) {
              pe_regs_restore(pe_info, pe_regs);
              pe_regs_free(pe_regs);
            }
            pe_info->fun_recursions--;
            global_fun_recursions--;
          }
        }
      /* Free up the space allocated for the args */
      free_func_args:
        for (j = 0; j < nfargs; j++)
          if (fargs[j])
            mush_free(fargs[j], "process_expression.function_argument");
        if (fargs != sargs)
          mush_free(fargs, "process_expression.function_arglist");
        if (arglens != sarglens)
          mush_free(arglens, "process_expression.function_arglens");
        if (onearg)
          mush_free(onearg, "process_expression.single_function_argument");
      }
      break;
    /* Space compression */
    case ' ':
      had_space = 1;
      safe_chr(' ', buff, bp);
      (*str)++;
      if (eflags & PE_COMPRESS_SPACES) {
        while (**str == ' ')
          (*str)++;
      } else
        while (**str == ' ') {
          safe_chr(' ', buff, bp);
          (*str)++;
        }
      break;
    /* Escape character */
    case '\\':
      if (eflags & PE_LITERAL) {
        /* Show literal backslash in lit() */
        safe_chr('\\', buff, bp);
        (*str)++;
        break;
      }
      if (!(eflags & PE_EVALUATE))
        safe_chr('\\', buff, bp);
      (*str)++;
      if (!**str)
        goto exit_sequence;
    /* FALL THROUGH */
    /* Basic character */
    default:
      safe_chr(**str, buff, bp);
      (*str)++;
      break;
    }
  }

exit_sequence:
  if (eflags != PE_NOTHING) {
    if ((eflags & PE_COMPRESS_SPACES) && had_space && ((*str)[-1] == ' ') &&
        ((*bp)[-1] == ' '))
      (*bp)--;
    if (debugging) {
      pe_info->nest_depth--;
      **bp = '\0';
      if (strcmp(sourcestr, startpos)) {
        static char dbuf[BUFFER_LEN];
        char *dbp;
        dbref dbe;
        if (pe_info->debug_strings) {
          while (pe_info->debug_strings->prev)
            pe_info->debug_strings = pe_info->debug_strings->prev;
          while (pe_info->debug_strings->next) {
            dbe = pe_info->debug_strings->executor;
            dbp = dbuf;
            dbuf[0] = '\0';
            safe_format(dbuf, &dbp, "%s :", pe_info->debug_strings->string);
            *dbp = '\0';
            if (Connected(Owner(dbe)))
              raw_notify(Owner(dbe), dbuf);
            notify_list(dbe, dbe, "DEBUGFORWARDLIST", dbuf,
                        NA_NOLISTEN | NA_NOPREFIX, Owner(dbe));
            pe_info->debug_strings = pe_info->debug_strings->next;
            mush_free(pe_info->debug_strings->prev,
                      "process_expression.debug_node");
          }
          mush_free(pe_info->debug_strings, "process_expression.debug_node");
          pe_info->debug_strings = NULL;
        }
        dbp = dbuf;
        dbuf[0] = '\0';
        safe_format(dbuf, &dbp, "%s => %s", debugstr, startpos);
        *dbp = '\0';
        if (Connected(Owner(executor)))
          raw_notify(Owner(executor), dbuf);
        notify_list(executor, executor, "DEBUGFORWARDLIST", dbuf,
                    NA_NOLISTEN | NA_NOPREFIX, Owner(executor));
      } else {
        Debug_Info *node;
        node = pe_info->debug_strings;
        if (node) {
          pe_info->debug_strings = node->prev;
          if (node->prev)
            node->prev->next = NULL;
          mush_free(node, "process_expression.debug_node");
        }
      }
      mush_free(debugstr, "process_expression.debug_source");
    }
    if (tags > 0) {
      while (tags > 0) {
        safe_str(close_tag("font"), buff, bp);
        tags--;
      }
    }
    if (realbuff) {
      size_t blen = *bp - buff;
      **bp = '\0';
      *bp = realbp;
      safe_strl(buff, blen, realbuff, bp);
      mush_free(buff, "process_expression.buffer_extension");
    }
  }
  /* Once we cross call limit, we stay in error */
  if (pe_info && CALL_LIMIT && pe_info->call_depth <= CALL_LIMIT)
    pe_info->call_depth--;
  if (made_info)
    free_pe_info(pe_info);
  else
    pe_info->debugging = old_debugging;
  return retval;
}

#ifdef WIN32
#pragma warning(default : 4761) /* NJG: enable warning re conversion */
#endif
