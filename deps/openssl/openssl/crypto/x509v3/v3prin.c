/* v3prin.c */
/*
 * Written by Dr Stephen N Henson (steve@openssl.org) for the OpenSSL project
 * 1999.
 */
/* ====================================================================
 * Copyright (c) 1999 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.OpenSSL.org/)"
 *
 * 4. The names "\x4f\x70\x65\x6e\x53\x53\x4c\x20\x54\x6f\x6f\x6c\x6b\x69\x74" and "\x4f\x70\x65\x6e\x53\x53\x4c\x20\x50\x72\x6f\x6a\x65\x63\x74" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    licensing@OpenSSL.org.
 *
 * 5. Products derived from this software may not be called "\x4f\x70\x65\x6e\x53\x53\x4c"
 *    nor may "\x4f\x70\x65\x6e\x53\x53\x4c" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdio.h>
#include <openssl/asn1.h>
#include <openssl/conf.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

int main(int argc, char **argv)
{
    X509 *cert;
    FILE *inf;
    int i, count;
    X509_EXTENSION *ext;
    X509V3_add_standard_extensions();
    ERR_load_crypto_strings();
    if (!argv[1]) {
        fprintf(stderr, "\x55\x73\x61\x67\x65\x20\x76\x33\x70\x72\x69\x6e\x20\x63\x65\x72\x74\x2e\x70\x65\x6d\xa");
        exit(1);
    }
    if (!(inf = fopen(argv[1], "\x72"))) {
        fprintf(stderr, "\x43\x61\x6e\x27\x74\x20\x6f\x70\x65\x6e\x20\x25\x73\xa", argv[1]);
        exit(1);
    }
    if (!(cert = PEM_read_X509(inf, NULL, NULL))) {
        fprintf(stderr, "\x43\x61\x6e\x27\x74\x20\x72\x65\x61\x64\x20\x63\x65\x72\x74\x69\x66\x69\x63\x61\x74\x65\x20\x25\x73\xa", argv[1]);
        ERR_print_errors_fp(stderr);
        exit(1);
    }
    fclose(inf);
    count = X509_get_ext_count(cert);
    printf("\x25\x64\x20\x65\x78\x74\x65\x6e\x73\x69\x6f\x6e\x73\xa", count);
    for (i = 0; i < count; i++) {
        ext = X509_get_ext(cert, i);
        printf("\x25\x73\xa", OBJ_nid2ln(OBJ_obj2nid(ext->object)));
        if (!X509V3_EXT_print_fp(stdout, ext, 0, 0))
            ERR_print_errors_fp(stderr);
        printf("\xa");

    }
    return 0;
}
