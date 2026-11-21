/* a5_libc.c -- the bionic imports of libasphalt5.so that the android32
 * runtime has no shim for (gen_imports.py finds them by their b_ names).
 *
 *   FILE positions   rewind / fgetpos / fsetpos: a game FILE* is a newlib
 *                    FILE* or one of bionic's __sF std streams (see the
 *                    runtime's bionic_stdio.c); bionic's fpos_t is a 32-bit
 *                    off_t, so positions go through b_ftell / b_fseek.
 *   wchar_t          bionic's is 32-bit (uint32_t here, as the runtime's
 *                    bionic_wchar.c has it): wcscmp / wcscpy / wcsncpy are
 *                    done here rather than trusting newlib's wchar_t size.
 *   mbstowcs         through the runtime's UTF-8 b_mbsrtowcs (bionic's
 *                    multibyte encoding is always UTF-8).
 *   __dso_handle     the module's __cxa_atexit argument: any unique address.
 * MIT.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The runtime's (bionic_stdio.c, bionic_wchar.c). */
int b_fseek(void *fp, long off, int whence);
long b_ftell(void *fp);
typedef struct { uint8_t seq[4]; } a5_mbstate_t;
size_t b_mbsrtowcs(uint32_t *dst, const char **src, size_t len, void *ps);

void *b___dso_handle = &b___dso_handle;

void b_rewind(void *fp) {
  if (b_fseek(fp, 0, SEEK_SET) == 0)
    clearerr((FILE *)fp);
}

int b_fgetpos(void *fp, int32_t *pos) {
  long p = b_ftell(fp);
  if (p < 0)
    return -1;
  *pos = (int32_t)p;
  return 0;
}

int b_fsetpos(void *fp, const int32_t *pos) { return b_fseek(fp, (long)*pos, SEEK_SET); }

size_t b_mbstowcs(uint32_t *dst, const char *src, size_t n) {
  a5_mbstate_t st;
  memset(&st, 0, sizeof st);
  return b_mbsrtowcs(dst, &src, n, &st);
}

int b_wcscmp(const uint32_t *a, const uint32_t *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a < *b ? -1 : *a > *b;
}

uint32_t *b_wcscpy(uint32_t *dst, const uint32_t *src) {
  uint32_t *d = dst;
  while ((*d++ = *src++) != 0)
    ;
  return dst;
}

uint32_t *b_wcsncpy(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++)
    dst[i] = src[i];
  for (; i < n; i++)
    dst[i] = 0;
  return dst;
}

/* ------------------------------------------------------------ libasphalt5.so
 * Two more bionic imports this older build has that the runtime does not:
 *   fileno   a game FILE* is a newlib FILE*, or one of bionic's __sF
 *            standard streams (bionic_stdio.c): 0, 1, 2 for those.
 *   herror   prints the resolver's last error: logged.
 * (__aeabi_uidiv / __aeabi_uidivmod / __aeabi_unwind_cpp_pr0 / _pr1, which
 * an EABI4 build still takes from libc, are libgcc's: PASSTHROUGH in
 * tools/imports.cfg.) */
#include "bionic.h"
#include "util.h"

extern unsigned char b___sF[];

int b_fileno(void *fp) {
  const unsigned char *p = fp;
  for (int i = 0; i < 3; i++)
    if (p == &b___sF[i * B_FILE_SIZE])
      return i;
  return fp ? fileno((FILE *)fp) : -1;
}

void b_herror(const char *s) { debugPrintf("[net] herror: %s\n", s ? s : ""); }
