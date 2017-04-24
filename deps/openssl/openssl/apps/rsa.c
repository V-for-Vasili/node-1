/* apps/rsa.c */
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

#include <openssl/opensslconf.h>
#ifndef OPENSSL_NO_RSA
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <time.h>
# include "apps.h"
# include <openssl/bio.h>
# include <openssl/err.h>
# include <openssl/rsa.h>
# include <openssl/evp.h>
# include <openssl/x509.h>
# include <openssl/pem.h>
# include <openssl/bn.h>

# undef PROG
# define PROG    rsa_main

/*-
 * -inform arg  - input format - default PEM (one of DER, NET or PEM)
 * -outform arg - output format - default PEM
 * -in arg      - input file - default stdin
 * -out arg     - output file - default stdout
 * -des         - encrypt output if PEM format with DES in cbc mode
 * -des3        - encrypt output if PEM format
 * -idea        - encrypt output if PEM format
 * -seed        - encrypt output if PEM format
 * -aes128      - encrypt output if PEM format
 * -aes192      - encrypt output if PEM format
 * -aes256      - encrypt output if PEM format
 * -camellia128 - encrypt output if PEM format
 * -camellia192 - encrypt output if PEM format
 * -camellia256 - encrypt output if PEM format
 * -text        - print a text version
 * -modulus     - print the RSA key modulus
 * -check       - verify key consistency
 * -pubin       - Expect a public key in input file.
 * -pubout      - Output a public key.
 */

int MAIN(int, char **);

int MAIN(int argc, char **argv)
{
    ENGINE *e = NULL;
    int ret = 1;
    RSA *rsa = NULL;
    int i, badops = 0, sgckey = 0;
    const EVP_CIPHER *enc = NULL;
    BIO *out = NULL;
    int informat, outformat, text = 0, check = 0, noout = 0;
    int pubin = 0, pubout = 0;
    char *infile, *outfile, *prog;
    char *passargin = NULL, *passargout = NULL;
    char *passin = NULL, *passout = NULL;
    char *engine = NULL;
    int modulus = 0;

    int pvk_encr = 2;

    apps_startup();

    if (bio_err == NULL)
        if ((bio_err = BIO_new(BIO_s_file())) != NULL)
            BIO_set_fp(bio_err, stderr, BIO_NOCLOSE | BIO_FP_TEXT);

    if (!load_config(bio_err, NULL))
        goto end;

    infile = NULL;
    outfile = NULL;
    informat = FORMAT_PEM;
    outformat = FORMAT_PEM;

    prog = argv[0];
    argc--;
    argv++;
    while (argc >= 1) {
        if (strcmp(*argv, "\x2d\x69\x6e\x66\x6f\x72\x6d") == 0) {
            if (--argc < 1)
                goto bad;
            informat = str2fmt(*(++argv));
        } else if (strcmp(*argv, "\x2d\x6f\x75\x74\x66\x6f\x72\x6d") == 0) {
            if (--argc < 1)
                goto bad;
            outformat = str2fmt(*(++argv));
        } else if (strcmp(*argv, "\x2d\x69\x6e") == 0) {
            if (--argc < 1)
                goto bad;
            infile = *(++argv);
        } else if (strcmp(*argv, "\x2d\x6f\x75\x74") == 0) {
            if (--argc < 1)
                goto bad;
            outfile = *(++argv);
        } else if (strcmp(*argv, "\x2d\x70\x61\x73\x73\x69\x6e") == 0) {
            if (--argc < 1)
                goto bad;
            passargin = *(++argv);
        } else if (strcmp(*argv, "\x2d\x70\x61\x73\x73\x6f\x75\x74") == 0) {
            if (--argc < 1)
                goto bad;
            passargout = *(++argv);
        }
# ifndef OPENSSL_NO_ENGINE
        else if (strcmp(*argv, "\x2d\x65\x6e\x67\x69\x6e\x65") == 0) {
            if (--argc < 1)
                goto bad;
            engine = *(++argv);
        }
# endif
        else if (strcmp(*argv, "\x2d\x73\x67\x63\x6b\x65\x79") == 0)
            sgckey = 1;
        else if (strcmp(*argv, "\x2d\x70\x75\x62\x69\x6e") == 0)
            pubin = 1;
        else if (strcmp(*argv, "\x2d\x70\x75\x62\x6f\x75\x74") == 0)
            pubout = 1;
        else if (strcmp(*argv, "\x2d\x52\x53\x41\x50\x75\x62\x6c\x69\x63\x4b\x65\x79\x5f\x69\x6e") == 0)
            pubin = 2;
        else if (strcmp(*argv, "\x2d\x52\x53\x41\x50\x75\x62\x6c\x69\x63\x4b\x65\x79\x5f\x6f\x75\x74") == 0)
            pubout = 2;
        else if (strcmp(*argv, "\x2d\x70\x76\x6b\x2d\x73\x74\x72\x6f\x6e\x67") == 0)
            pvk_encr = 2;
        else if (strcmp(*argv, "\x2d\x70\x76\x6b\x2d\x77\x65\x61\x6b") == 0)
            pvk_encr = 1;
        else if (strcmp(*argv, "\x2d\x70\x76\x6b\x2d\x6e\x6f\x6e\x65") == 0)
            pvk_encr = 0;
        else if (strcmp(*argv, "\x2d\x6e\x6f\x6f\x75\x74") == 0)
            noout = 1;
        else if (strcmp(*argv, "\x2d\x74\x65\x78\x74") == 0)
            text = 1;
        else if (strcmp(*argv, "\x2d\x6d\x6f\x64\x75\x6c\x75\x73") == 0)
            modulus = 1;
        else if (strcmp(*argv, "\x2d\x63\x68\x65\x63\x6b") == 0)
            check = 1;
        else if ((enc = EVP_get_cipherbyname(&(argv[0][1]))) == NULL) {
            BIO_printf(bio_err, "\x75\x6e\x6b\x6e\x6f\x77\x6e\x20\x6f\x70\x74\x69\x6f\x6e\x20\x25\x73\xa", *argv);
            badops = 1;
            break;
        }
        argc--;
        argv++;
    }

    if (badops) {
 bad:
        BIO_printf(bio_err, "\x25\x73\x20\x5b\x6f\x70\x74\x69\x6f\x6e\x73\x5d\x20\x3c\x69\x6e\x66\x69\x6c\x65\x20\x3e\x6f\x75\x74\x66\x69\x6c\x65\xa", prog);
        BIO_printf(bio_err, "\x77\x68\x65\x72\x65\x20\x6f\x70\x74\x69\x6f\x6e\x73\x20\x61\x72\x65\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x69\x6e\x66\x6f\x72\x6d\x20\x61\x72\x67\x20\x20\x20\x20\x20\x69\x6e\x70\x75\x74\x20\x66\x6f\x72\x6d\x61\x74\x20\x2d\x20\x6f\x6e\x65\x20\x6f\x66\x20\x44\x45\x52\x20\x4e\x45\x54\x20\x50\x45\x4d\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x6f\x75\x74\x66\x6f\x72\x6d\x20\x61\x72\x67\x20\x20\x20\x20\x6f\x75\x74\x70\x75\x74\x20\x66\x6f\x72\x6d\x61\x74\x20\x2d\x20\x6f\x6e\x65\x20\x6f\x66\x20\x44\x45\x52\x20\x4e\x45\x54\x20\x50\x45\x4d\xa");
        BIO_printf(bio_err, "\x20\x2d\x69\x6e\x20\x61\x72\x67\x20\x20\x20\x20\x20\x20\x20\x20\x20\x69\x6e\x70\x75\x74\x20\x66\x69\x6c\x65\xa");
        BIO_printf(bio_err, "\x20\x2d\x73\x67\x63\x6b\x65\x79\x20\x20\x20\x20\x20\x20\x20\x20\x20\x55\x73\x65\x20\x49\x49\x53\x20\x53\x47\x43\x20\x6b\x65\x79\x20\x66\x6f\x72\x6d\x61\x74\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x70\x61\x73\x73\x69\x6e\x20\x61\x72\x67\x20\x20\x20\x20\x20\x69\x6e\x70\x75\x74\x20\x66\x69\x6c\x65\x20\x70\x61\x73\x73\x20\x70\x68\x72\x61\x73\x65\x20\x73\x6f\x75\x72\x63\x65\xa");
        BIO_printf(bio_err, "\x20\x2d\x6f\x75\x74\x20\x61\x72\x67\x20\x20\x20\x20\x20\x20\x20\x20\x6f\x75\x74\x70\x75\x74\x20\x66\x69\x6c\x65\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x70\x61\x73\x73\x6f\x75\x74\x20\x61\x72\x67\x20\x20\x20\x20\x6f\x75\x74\x70\x75\x74\x20\x66\x69\x6c\x65\x20\x70\x61\x73\x73\x20\x70\x68\x72\x61\x73\x65\x20\x73\x6f\x75\x72\x63\x65\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x64\x65\x73\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x6e\x63\x72\x79\x70\x74\x20\x50\x45\x4d\x20\x6f\x75\x74\x70\x75\x74\x20\x77\x69\x74\x68\x20\x63\x62\x63\x20\x64\x65\x73\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x64\x65\x73\x33\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x6e\x63\x72\x79\x70\x74\x20\x50\x45\x4d\x20\x6f\x75\x74\x70\x75\x74\x20\x77\x69\x74\x68\x20\x65\x64\x65\x20\x63\x62\x63\x20\x64\x65\x73\x20\x75\x73\x69\x6e\x67\x20\x31\x36\x38\x20\x62\x69\x74\x20\x6b\x65\x79\xa");
# ifndef OPENSSL_NO_IDEA
        BIO_printf(bio_err,
                   "\x20\x2d\x69\x64\x65\x61\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x6e\x63\x72\x79\x70\x74\x20\x50\x45\x4d\x20\x6f\x75\x74\x70\x75\x74\x20\x77\x69\x74\x68\x20\x63\x62\x63\x20\x69\x64\x65\x61\xa");
# endif
# ifndef OPENSSL_NO_SEED
        BIO_printf(bio_err,
                   "\x20\x2d\x73\x65\x65\x64\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x6e\x63\x72\x79\x70\x74\x20\x50\x45\x4d\x20\x6f\x75\x74\x70\x75\x74\x20\x77\x69\x74\x68\x20\x63\x62\x63\x20\x73\x65\x65\x64\xa");
# endif
# ifndef OPENSSL_NO_AES
        BIO_printf(bio_err, "\x20\x2d\x61\x65\x73\x31\x32\x38\x2c\x20\x2d\x61\x65\x73\x31\x39\x32\x2c\x20\x2d\x61\x65\x73\x32\x35\x36\xa");
        BIO_printf(bio_err,
                   "\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x6e\x63\x72\x79\x70\x74\x20\x50\x45\x4d\x20\x6f\x75\x74\x70\x75\x74\x20\x77\x69\x74\x68\x20\x63\x62\x63\x20\x61\x65\x73\xa");
# endif
# ifndef OPENSSL_NO_CAMELLIA
        BIO_printf(bio_err, "\x20\x2d\x63\x61\x6d\x65\x6c\x6c\x69\x61\x31\x32\x38\x2c\x20\x2d\x63\x61\x6d\x65\x6c\x6c\x69\x61\x31\x39\x32\x2c\x20\x2d\x63\x61\x6d\x65\x6c\x6c\x69\x61\x32\x35\x36\xa");
        BIO_printf(bio_err,
                   "\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x6e\x63\x72\x79\x70\x74\x20\x50\x45\x4d\x20\x6f\x75\x74\x70\x75\x74\x20\x77\x69\x74\x68\x20\x63\x62\x63\x20\x63\x61\x6d\x65\x6c\x6c\x69\x61\xa");
# endif
        BIO_printf(bio_err, "\x20\x2d\x74\x65\x78\x74\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x70\x72\x69\x6e\x74\x20\x74\x68\x65\x20\x6b\x65\x79\x20\x69\x6e\x20\x74\x65\x78\x74\xa");
        BIO_printf(bio_err, "\x20\x2d\x6e\x6f\x6f\x75\x74\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x64\x6f\x6e\x27\x74\x20\x70\x72\x69\x6e\x74\x20\x6b\x65\x79\x20\x6f\x75\x74\xa");
        BIO_printf(bio_err, "\x20\x2d\x6d\x6f\x64\x75\x6c\x75\x73\x20\x20\x20\x20\x20\x20\x20\x20\x70\x72\x69\x6e\x74\x20\x74\x68\x65\x20\x52\x53\x41\x20\x6b\x65\x79\x20\x6d\x6f\x64\x75\x6c\x75\x73\xa");
        BIO_printf(bio_err, "\x20\x2d\x63\x68\x65\x63\x6b\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x76\x65\x72\x69\x66\x79\x20\x6b\x65\x79\x20\x63\x6f\x6e\x73\x69\x73\x74\x65\x6e\x63\x79\xa");
        BIO_printf(bio_err,
                   "\x20\x2d\x70\x75\x62\x69\x6e\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x65\x78\x70\x65\x63\x74\x20\x61\x20\x70\x75\x62\x6c\x69\x63\x20\x6b\x65\x79\x20\x69\x6e\x20\x69\x6e\x70\x75\x74\x20\x66\x69\x6c\x65\xa");
        BIO_printf(bio_err, "\x20\x2d\x70\x75\x62\x6f\x75\x74\x20\x20\x20\x20\x20\x20\x20\x20\x20\x6f\x75\x74\x70\x75\x74\x20\x61\x20\x70\x75\x62\x6c\x69\x63\x20\x6b\x65\x79\xa");
# ifndef OPENSSL_NO_ENGINE
        BIO_printf(bio_err,
                   "\x20\x2d\x65\x6e\x67\x69\x6e\x65\x20\x65\x20\x20\x20\x20\x20\x20\x20\x75\x73\x65\x20\x65\x6e\x67\x69\x6e\x65\x20\x65\x2c\x20\x70\x6f\x73\x73\x69\x62\x6c\x79\x20\x61\x20\x68\x61\x72\x64\x77\x61\x72\x65\x20\x64\x65\x76\x69\x63\x65\x2e\xa");
# endif
        goto end;
    }

    ERR_load_crypto_strings();

    e = setup_engine(bio_err, engine, 0);

    if (!app_passwd(bio_err, passargin, passargout, &passin, &passout)) {
        BIO_printf(bio_err, "\x45\x72\x72\x6f\x72\x20\x67\x65\x74\x74\x69\x6e\x67\x20\x70\x61\x73\x73\x77\x6f\x72\x64\x73\xa");
        goto end;
    }

    if (check && pubin) {
        BIO_printf(bio_err, "\x4f\x6e\x6c\x79\x20\x70\x72\x69\x76\x61\x74\x65\x20\x6b\x65\x79\x73\x20\x63\x61\x6e\x20\x62\x65\x20\x63\x68\x65\x63\x6b\x65\x64\xa");
        goto end;
    }

    out = BIO_new(BIO_s_file());

    {
        EVP_PKEY *pkey;

        if (pubin) {
            int tmpformat = -1;
            if (pubin == 2) {
                if (informat == FORMAT_PEM)
                    tmpformat = FORMAT_PEMRSA;
                else if (informat == FORMAT_ASN1)
                    tmpformat = FORMAT_ASN1RSA;
            } else if (informat == FORMAT_NETSCAPE && sgckey)
                tmpformat = FORMAT_IISSGC;
            else
                tmpformat = informat;

            pkey = load_pubkey(bio_err, infile, tmpformat, 1,
                               passin, e, "\x50\x75\x62\x6c\x69\x63\x20\x4b\x65\x79");
        } else
            pkey = load_key(bio_err, infile,
                            (informat == FORMAT_NETSCAPE && sgckey ?
                             FORMAT_IISSGC : informat), 1,
                            passin, e, "\x50\x72\x69\x76\x61\x74\x65\x20\x4b\x65\x79");

        if (pkey != NULL)
            rsa = EVP_PKEY_get1_RSA(pkey);
        EVP_PKEY_free(pkey);
    }

    if (rsa == NULL) {
        ERR_print_errors(bio_err);
        goto end;
    }

    if (outfile == NULL) {
        BIO_set_fp(out, stdout, BIO_NOCLOSE);
# ifdef OPENSSL_SYS_VMS
        {
            BIO *tmpbio = BIO_new(BIO_f_linebuffer());
            out = BIO_push(tmpbio, out);
        }
# endif
    } else {
        if (BIO_write_filename(out, outfile) <= 0) {
            perror(outfile);
            goto end;
        }
    }

    if (text)
        if (!RSA_print(out, rsa, 0)) {
            perror(outfile);
            ERR_print_errors(bio_err);
            goto end;
        }

    if (modulus) {
        BIO_printf(out, "\x4d\x6f\x64\x75\x6c\x75\x73\x3d");
        BN_print(out, rsa->n);
        BIO_printf(out, "\xa");
    }

    if (check) {
        int r = RSA_check_key(rsa);

        if (r == 1)
            BIO_printf(out, "\x52\x53\x41\x20\x6b\x65\x79\x20\x6f\x6b\xa");
        else if (r == 0) {
            unsigned long err;

            while ((err = ERR_peek_error()) != 0 &&
                   ERR_GET_LIB(err) == ERR_LIB_RSA &&
                   ERR_GET_FUNC(err) == RSA_F_RSA_CHECK_KEY &&
                   ERR_GET_REASON(err) != ERR_R_MALLOC_FAILURE) {
                BIO_printf(out, "\x52\x53\x41\x20\x6b\x65\x79\x20\x65\x72\x72\x6f\x72\x3a\x20\x25\x73\xa",
                           ERR_reason_error_string(err));
                ERR_get_error(); /* remove e from error stack */
            }
        }

        if (r == -1 || ERR_peek_error() != 0) { /* should happen only if r ==
                                                 * -1 */
            ERR_print_errors(bio_err);
            goto end;
        }
    }

    if (noout) {
        ret = 0;
        goto end;
    }
    BIO_printf(bio_err, "\x77\x72\x69\x74\x69\x6e\x67\x20\x52\x53\x41\x20\x6b\x65\x79\xa");
    if (outformat == FORMAT_ASN1) {
        if (pubout || pubin) {
            if (pubout == 2)
                i = i2d_RSAPublicKey_bio(out, rsa);
            else
                i = i2d_RSA_PUBKEY_bio(out, rsa);
        } else
            i = i2d_RSAPrivateKey_bio(out, rsa);
    }
# ifndef OPENSSL_NO_RC4
    else if (outformat == FORMAT_NETSCAPE) {
        unsigned char *p, *pp;
        int size;

        i = 1;
        size = i2d_RSA_NET(rsa, NULL, NULL, sgckey);
        if ((p = (unsigned char *)OPENSSL_malloc(size)) == NULL) {
            BIO_printf(bio_err, "\x4d\x65\x6d\x6f\x72\x79\x20\x61\x6c\x6c\x6f\x63\x61\x74\x69\x6f\x6e\x20\x66\x61\x69\x6c\x75\x72\x65\xa");
            goto end;
        }
        pp = p;
        i2d_RSA_NET(rsa, &p, NULL, sgckey);
        BIO_write(out, (char *)pp, size);
        OPENSSL_free(pp);
    }
# endif
    else if (outformat == FORMAT_PEM) {
        if (pubout || pubin) {
            if (pubout == 2)
                i = PEM_write_bio_RSAPublicKey(out, rsa);
            else
                i = PEM_write_bio_RSA_PUBKEY(out, rsa);
        } else
            i = PEM_write_bio_RSAPrivateKey(out, rsa,
                                            enc, NULL, 0, NULL, passout);
# if !defined(OPENSSL_NO_DSA) && !defined(OPENSSL_NO_RC4)
    } else if (outformat == FORMAT_MSBLOB || outformat == FORMAT_PVK) {
        EVP_PKEY *pk;
        pk = EVP_PKEY_new();
        EVP_PKEY_set1_RSA(pk, rsa);
        if (outformat == FORMAT_PVK)
            i = i2b_PVK_bio(out, pk, pvk_encr, 0, passout);
        else if (pubin || pubout)
            i = i2b_PublicKey_bio(out, pk);
        else
            i = i2b_PrivateKey_bio(out, pk);
        EVP_PKEY_free(pk);
# endif
    } else {
        BIO_printf(bio_err, "\x62\x61\x64\x20\x6f\x75\x74\x70\x75\x74\x20\x66\x6f\x72\x6d\x61\x74\x20\x73\x70\x65\x63\x69\x66\x69\x65\x64\x20\x66\x6f\x72\x20\x6f\x75\x74\x66\x69\x6c\x65\xa");
        goto end;
    }
    if (i <= 0) {
        BIO_printf(bio_err, "\x75\x6e\x61\x62\x6c\x65\x20\x74\x6f\x20\x77\x72\x69\x74\x65\x20\x6b\x65\x79\xa");
        ERR_print_errors(bio_err);
    } else
        ret = 0;
 end:
    release_engine(e);
    if (out != NULL)
        BIO_free_all(out);
    if (rsa != NULL)
        RSA_free(rsa);
    if (passin)
        OPENSSL_free(passin);
    if (passout)
        OPENSSL_free(passout);
    apps_shutdown();
    OPENSSL_EXIT(ret);
}
#else                           /* !OPENSSL_NO_RSA */

# if PEDANTIC
static void *dummy = &dummy;
# endif

#endif
