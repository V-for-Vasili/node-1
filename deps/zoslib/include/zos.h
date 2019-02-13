#ifndef __ZOS_H_
#define __ZOS_H_
//-------------------------------------------------------------------------------------
//
// external interface:
//
#include <_Nascii.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern int __debug_mode;
#define __ZOS_CC
#ifdef __cplusplus
extern "C" {
#endif
extern void *_convert_e2a(void *dst, const void *src, size_t size);
extern void *_convert_a2e(void *dst, const void *src, size_t size);
extern char **__get_environ_np(void);
extern void __xfer_env(void);
extern int __chgfdccsid(int fd, unsigned short ccsid);
extern size_t __e2a_l(char *bufptr, size_t szLen);
extern size_t __a2e_l(char *bufptr, size_t szLen);
extern size_t __e2a_s(char *string);
extern size_t __a2e_s(char *string);
extern int dprintf(int fd, const char *, ...);
extern void __xfer_env(void);
extern int __chgfdccsid(int fd, unsigned short ccsid);

#ifdef __cplusplus
}
#endif

#define _str_e2a(_str)                                                         \
  ({                                                                           \
    const char *src = (const char *)(_str);                                    \
    int len = strlen(src) + 1;                                                 \
    char *tgt = alloca(len);                                                   \
    _convert_e2a(tgt, src, len);                                               \
  })

#define AEWRAP(_rc, _x)                                                        \
  (__isASCII() ? ((_rc) = (_x), 0)                                             \
               : (__ae_thread_swapmode(__AE_ASCII_MODE), ((_rc) = (_x)),       \
                  __ae_thread_swapmode(__AE_EBCDIC_MODE), 1))

#define AEWRAP_VOID(_x)                                                        \
  (__isASCII() ? ((_x), 0)                                                     \
               : (__ae_thread_swapmode(__AE_ASCII_MODE), (_x),                 \
                  __ae_thread_swapmode(__AE_EBCDIC_MODE), 1))

#endif
