/* crypto/bio/b_dump.c */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "\x54\x68\x69\x73\x20\x70\x72\x6f\x64\x75\x63\x74\x20\x69\x6e\x63\x6c\x75\x64\x65\x73\x20\x73\x6f\x66\x74\x77\x61\x72\x65\x20\x77\x72\x69\x74\x74\x65\x6e\x20\x62\x79\x20\x54\x69\x6d\x20\x48\x75\x64\x73\x6f\x6e\x20\x28\x74\x6a\x68\x40\x63\x72\x79\x70\x74\x73\x6f\x66\x74\x2e\x63\x6f\x6d\x29"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */

/*
 * Stolen from tjh's ssl/ssl_trc.c stuff.
 */

#include <stdio.h>
#include "cryptlib.h"
#include "bio_lcl.h"

#define TRUNCATE
#define DUMP_WIDTH      16
#define DUMP_WIDTH_LESS_INDENT(i) (DUMP_WIDTH-((i-(i>6?6:i)+3)/4))

int BIO_dump_cb(int (*cb) (const void *data, size_t len, void *u),
                void *u, const char *s, int len)
{
    return BIO_dump_indent_cb(cb, u, s, len, 0);
}

int BIO_dump_indent_cb(int (*cb) (const void *data, size_t len, void *u),
                       void *u, const char *s, int len, int indent)
{
    int ret = 0;
    char buf[288 + 1], tmp[20], str[128 + 1];
    int i, j, rows, trc;
    unsigned char ch;
    int dump_width;

    trc = 0;

#ifdef TRUNCATE
    for (; (len > 0) && ((s[len - 1] == '\x20') || (s[len - 1] == '\x0')); len--)
        trc++;
#endif

    if (indent < 0)
        indent = 0;
    if (indent) {
        if (indent > 128)
            indent = 128;
        memset(str, '\x20', indent);
    }
    str[indent] = '\x0';

    dump_width = DUMP_WIDTH_LESS_INDENT(indent);
    rows = (len / dump_width);
    if ((rows * dump_width) < len)
        rows++;
    for (i = 0; i < rows; i++) {
        BUF_strlcpy(buf, str, sizeof buf);
        BIO_snprintf(tmp, sizeof tmp, "\x25\x30\x34\x78\x20\x2d\x20", i * dump_width);
        BUF_strlcat(buf, tmp, sizeof buf);
        for (j = 0; j < dump_width; j++) {
            if (((i * dump_width) + j) >= len) {
                BUF_strlcat(buf, "\x20\x20\x20", sizeof buf);
            } else {
                ch = ((unsigned char)*(s + i * dump_width + j)) & 0xff;
                BIO_snprintf(tmp, sizeof tmp, "\x25\x30\x32\x78\x25\x63", ch,
                             j == 7 ? '\x2d' : '\x20');
                BUF_strlcat(buf, tmp, sizeof buf);
            }
        }
        BUF_strlcat(buf, "\x20\x20", sizeof buf);
        for (j = 0; j < dump_width; j++) {
            if (((i * dump_width) + j) >= len)
                break;
            ch = ((unsigned char)*(s + i * dump_width + j)) & 0xff;
#ifndef CHARSET_EBCDIC
            BIO_snprintf(tmp, sizeof tmp, "\x25\x63",
                         ((ch >= '\x20') && (ch <= '\x7e')) ? ch : '\x2e');
#else
            BIO_snprintf(tmp, sizeof tmp, "\x25\x63",
                         ((ch >= os_toascii['\x20']) && (ch <= os_toascii['\x7e']))
                         ? os_toebcdic[ch]
                         : '\x2e');
#endif
            BUF_strlcat(buf, tmp, sizeof buf);
        }
        BUF_strlcat(buf, "\xa", sizeof buf);
        /*
         * if this is the last call then update the ddt_dump thing so that we
         * will move the selection point in the debug window
         */
        ret += cb((void *)buf, strlen(buf), u);
    }
#ifdef TRUNCATE
    if (trc > 0) {
        BIO_snprintf(buf, sizeof buf, "\x25\x73\x25\x30\x34\x78\x20\x2d\x20\x3c\x53\x50\x41\x43\x45\x53\x2f\x4e\x55\x4c\x53\x3e\xa", str,
                     len + trc);
        ret += cb((void *)buf, strlen(buf), u);
    }
#endif
    return (ret);
}

#ifndef OPENSSL_NO_FP_API
static int write_fp(const void *data, size_t len, void *fp)
{
    return UP_fwrite(data, len, 1, fp);
}

int BIO_dump_fp(FILE *fp, const char *s, int len)
{
    return BIO_dump_cb(write_fp, fp, s, len);
}

int BIO_dump_indent_fp(FILE *fp, const char *s, int len, int indent)
{
    return BIO_dump_indent_cb(write_fp, fp, s, len, indent);
}
#endif

static int write_bio(const void *data, size_t len, void *bp)
{
    return BIO_write((BIO *)bp, (const char *)data, len);
}

int BIO_dump(BIO *bp, const char *s, int len)
{
    return BIO_dump_cb(write_bio, bp, s, len);
}

int BIO_dump_indent(BIO *bp, const char *s, int len, int indent)
{
    return BIO_dump_indent_cb(write_bio, bp, s, len, indent);
}

int BIO_hex_string(BIO *out, int indent, int width, unsigned char *data,
                   int datalen)
{
    int i, j = 0;

    if (datalen < 1)
        return 1;

    for (i = 0; i < datalen - 1; i++) {
        if (i && !j)
            BIO_printf(out, "\x25\x2a\x73", indent, "");

        BIO_printf(out, "\x25\x30\x32\x58\x3a", data[i]);

        j = (j + 1) % width;
        if (!j)
            BIO_printf(out, "\xa");
    }

    if (i && !j)
        BIO_printf(out, "\x25\x2a\x73", indent, "");
    BIO_printf(out, "\x25\x30\x32\x58", data[datalen - 1]);
    return 1;
}
