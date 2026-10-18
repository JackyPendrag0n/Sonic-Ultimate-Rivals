/* Lets a binary built on glibc 2.39 run on glibc 2.35. Compiled as plain C99 (no _GNU_SOURCE) so the
   calls below bind to the classic symbols, not the C23 ones. */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/random.h>
long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
int __isoc23_vsscanf(const char *s, const char *f, va_list ap) { return vsscanf(s, f, ap); }
int __isoc23_sscanf(const char *s, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsscanf(s, f, ap); va_end(ap); return r; }
__asm__(".symver old_fmod, fmod@GLIBC_2.2.5");
__asm__(".symver old_fmodf, fmodf@GLIBC_2.2.5");
__asm__(".symver old_hypot, hypot@GLIBC_2.2.5");
double old_fmod(double, double); float old_fmodf(float, float); double old_hypot(double, double);
double __wrap_fmod(double x, double y) { return old_fmod(x, y); }
float __wrap_fmodf(float x, float y) { return old_fmodf(x, y); }
double __wrap_hypot(double x, double y) { return old_hypot(x, y); }
uint32_t __wrap_arc4random(void) { uint32_t v = 0; while (getrandom(&v, sizeof v, 0) != sizeof v) {} return v; }
/* strlcpy & co. appeared in glibc 2.38; SDL2 picks them up when built on newer systems. */
#include <wchar.h>
size_t strlcpy(char *d, const char *s, size_t n) { size_t l = 0; while (s[l]) l++; if (n) { size_t c = l < n - 1 ? l : n - 1; for (size_t i = 0; i < c; i++) d[i] = s[i]; d[c] = 0; } return l; }
size_t strlcat(char *d, const char *s, size_t n) { size_t dl = 0; while (dl < n && d[dl]) dl++; if (dl == n) { size_t l = 0; while (s[l]) l++; return n + l; } return dl + strlcpy(d + dl, s, n - dl); }
size_t wcslcpy(wchar_t *d, const wchar_t *s, size_t n) { size_t l = 0; while (s[l]) l++; if (n) { size_t c = l < n - 1 ? l : n - 1; for (size_t i = 0; i < c; i++) d[i] = s[i]; d[c] = 0; } return l; }
size_t wcslcat(wchar_t *d, const wchar_t *s, size_t n) { size_t dl = 0; while (dl < n && d[dl]) dl++; if (dl == n) { size_t l = 0; while (s[l]) l++; return n + l; } return dl + wcslcpy(d + dl, s, n - dl); }
