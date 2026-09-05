/* PAL <locale.h> — MiniOS Phase 0
 * MiniOS 只有 "C" locale。setlocale 永远返回 "C"，localeconv 返回一份
 * 静态的 C-locale lconv。CLR 的 CultureInfo 走的是 BCL 内置表，不依赖这里。
 */
#ifndef PAL_LOCALE_H
#define PAL_LOCALE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_ALL      6
#define LC_COLLATE  3
#define LC_CTYPE    0
#define LC_MONETARY 4
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_MESSAGES 5

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char  int_frac_digits;
    char  frac_digits;
    char  p_cs_precedes;
    char  p_sep_by_space;
    char  n_cs_precedes;
    char  n_sep_by_space;
    char  p_sign_posn;
    char  n_sign_posn;
};

char        *setlocale  (int category, const char *locale);
struct lconv *localeconv (void);

#ifdef __cplusplus
}
#endif
#endif /* PAL_LOCALE_H */
