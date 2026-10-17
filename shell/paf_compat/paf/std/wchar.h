/* paf/std/wchar.h - override: include stdarg.h for va_list */
#ifndef _VDSUITE_USER_PAF_STD_WCHAR_H
#define _VDSUITE_USER_PAF_STD_WCHAR_H

#include <stdarg.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

wchar_t *sce_paf_wcscat(wchar_t *dest, const wchar_t *src);
wchar_t *sce_paf_wcsncat(wchar_t *dest, const wchar_t *src, size_t count);
wchar_t *sce_paf_wcscpy(wchar_t *dest, const wchar_t *src);
wchar_t *sce_paf_wcsncpy(wchar_t *dest, const wchar_t *src, size_t count);
int sce_paf_wcscasecmp(const wchar_t *s1, const wchar_t *s2);
wchar_t *sce_paf_wcschr(const wchar_t *str, wchar_t ch);
int sce_paf_wcscmp(const wchar_t *lhs, const wchar_t *rhs);
size_t sce_paf_wcscspn(const wchar_t *dest, const wchar_t *src);
size_t sce_paf_wcslen(const wchar_t *str);
int sce_paf_wcsncasecmp(const wchar_t *s1, const wchar_t *s2, size_t n);
int sce_paf_wcsncmp(const wchar_t *lhs, const wchar_t *rhs, size_t count);
size_t sce_paf_wcsnlen(const wchar_t *s, size_t maxlen);
wchar_t *sce_paf_wcspbrk(const wchar_t *dest, const wchar_t *str);
wchar_t *sce_paf_wcsrchr(const wchar_t *str, wchar_t ch);
size_t sce_paf_wcsspn(const wchar_t *dest, const wchar_t *src);
wchar_t *sce_paf_wcsstr(const wchar_t *dest, const wchar_t *src);
wchar_t *sce_paf_wmemchr(const wchar_t *ptr, wchar_t ch, size_t count);
int sce_paf_wmemcmp(const wchar_t *lhs, const wchar_t *rhs, size_t count);
wchar_t *sce_paf_wmemcpy(wchar_t *dest, const wchar_t *src, size_t count);
wchar_t *sce_paf_wmemmove(wchar_t *dest, const wchar_t *src, size_t count);
wchar_t *sce_paf_wmemset(wchar_t *ptr, wchar_t wc, size_t count);
int sce_paf_swprintf(wchar_t *buffer, size_t bufsz, const wchar_t *format, ...);
int sce_paf_vswprintf(wchar_t *buffer, size_t bufsz, const wchar_t *format, va_list vlist);

#ifdef __cplusplus
}
#endif

#endif /* _VDSUITE_USER_PAF_STD_WCHAR_H */
