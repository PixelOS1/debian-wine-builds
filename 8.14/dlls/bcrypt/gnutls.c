/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
 * Copyright 2018 Hans Leidekker for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>

#ifdef HAVE_GNUTLS_CIPHER_INIT

#include <stdlib.h>
#include <assert.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <gnutls/abstract.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntsecapi.h"
#include "wincrypt.h"
#include "bcrypt.h"

#include "bcrypt_internal.h"

#include "wine/debug.h"

#include <assert.h>

#ifdef HAVE_GMP_H
#include <gmp.h>
#endif

#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_GCRYPT_H
#include <gcrypt.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(bcrypt);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

#if GNUTLS_VERSION_MAJOR < 3
#define GNUTLS_CIPHER_AES_192_CBC 92
#define GNUTLS_CIPHER_AES_128_GCM 93
#define GNUTLS_CIPHER_AES_256_GCM 94
#define GNUTLS_PK_ECC 4

#define GNUTLS_CURVE_TO_BITS(curve) (unsigned int)(((unsigned int)1<<31)|((unsigned int)(curve)))

typedef enum
{
    GNUTLS_ECC_CURVE_INVALID,
    GNUTLS_ECC_CURVE_SECP224R1,
    GNUTLS_ECC_CURVE_SECP256R1,
    GNUTLS_ECC_CURVE_SECP384R1,
    GNUTLS_ECC_CURVE_SECP521R1,
} gnutls_ecc_curve_t;
#endif

#if GNUTLS_VERSION_MAJOR < 3 || (GNUTLS_VERSION_MAJOR == 3 && GNUTLS_VERSION_MINOR < 6)
#define GNUTLS_CIPHER_AES_128_CFB8 29
#define GNUTLS_CIPHER_AES_192_CFB8 30
#define GNUTLS_CIPHER_AES_256_CFB8 31

#define GNUTLS_PK_RSA_PSS 6
#define GNUTLS_PRIVKEY_SIGN_FLAG_RSA_PSS (1 << 7)
typedef struct gnutls_x509_spki_st *gnutls_x509_spki_t;
#endif

struct dh_key_data
{
    UCHAR            *pubkey;     /* Used for DH private key only. */
    UCHAR            *privkey;    /* Used for DH private key only. */
};

union key_data
{
    gnutls_cipher_hd_t cipher;
    struct
    {
        gnutls_privkey_t privkey;
        gnutls_pubkey_t  pubkey;
    } a;
    struct /* DH */
    {
        UCHAR            *privkey;
        UCHAR            *pubkey;
    } d;
};
C_ASSERT( sizeof(union key_data) <= sizeof(((struct key *)0)->private) );

static union key_data *key_data( struct key *key )
{
    return (union key_data *)key->private;
}

static unsigned int dh_pubkey_len( struct key *key )
{
    return sizeof(BCRYPT_DH_KEY_BLOB) + key->u.a.bitlen / 8 * 3;
}

static void dh_key_free( struct key *key )
{
    free( key_data(key)->d.privkey );
    free( key_data(key)->d.pubkey );
}

static void dh_key_alloc( struct key *key )
{
    unsigned int bitlen = key->u.a.bitlen;

    if (key_data(key)->d.pubkey) return;

    key_data(key)->d.privkey = calloc( 1, bitlen / 8 );
    key_data(key)->d.pubkey = calloc( 1, dh_pubkey_len( key ));
}

/* Not present in gnutls version < 3.0 */
static int (*pgnutls_cipher_tag)(gnutls_cipher_hd_t, void *, size_t);
static int (*pgnutls_cipher_add_auth)(gnutls_cipher_hd_t, const void *, size_t);
static gnutls_sign_algorithm_t (*pgnutls_pk_to_sign)(gnutls_pk_algorithm_t, gnutls_digest_algorithm_t);
static int (*pgnutls_pubkey_import_ecc_raw)(gnutls_pubkey_t, gnutls_ecc_curve_t,
                                            const gnutls_datum_t *, const gnutls_datum_t *);
static int (*pgnutls_pubkey_export_ecc_raw)(gnutls_pubkey_t key, gnutls_ecc_curve_t *curve,
                                            gnutls_datum_t *x, gnutls_datum_t *y);
static int (*pgnutls_privkey_import_ecc_raw)(gnutls_privkey_t, gnutls_ecc_curve_t, const gnutls_datum_t *,
                                             const gnutls_datum_t *, const gnutls_datum_t *);
static int (*pgnutls_pubkey_verify_hash2)(gnutls_pubkey_t, gnutls_sign_algorithm_t, unsigned int,
                                          const gnutls_datum_t *, const gnutls_datum_t *);
static int (*pgnutls_pubkey_encrypt_data)(gnutls_pubkey_t, unsigned int flags, const gnutls_datum_t *,
                                          gnutls_datum_t *);

/* Not present in gnutls version < 2.11.0 */
static int (*pgnutls_pubkey_import_rsa_raw)(gnutls_pubkey_t, const gnutls_datum_t *, const gnutls_datum_t *);

/* Not present in gnutls version < 2.12.0 */
static int (*pgnutls_pubkey_import_dsa_raw)(gnutls_pubkey_t, const gnutls_datum_t *, const gnutls_datum_t *,
                                            const gnutls_datum_t *, const gnutls_datum_t *);
static int (*pgnutls_pubkey_import_privkey)(gnutls_pubkey_t, gnutls_privkey_t, unsigned int, unsigned int);
static int (*pgnutls_privkey_decrypt_data)(gnutls_privkey_t, unsigned int flags, const gnutls_datum_t *,
                                           gnutls_datum_t *);

/* Not present in gnutls version < 3.3.0 */
static int (*pgnutls_pubkey_export_dsa_raw)(gnutls_pubkey_t, gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *,
                                            gnutls_datum_t *);
static int (*pgnutls_pubkey_export_rsa_raw)(gnutls_pubkey_t, gnutls_datum_t *, gnutls_datum_t *);
static int (*pgnutls_privkey_export_ecc_raw)(gnutls_privkey_t, gnutls_ecc_curve_t *,
                                             gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *);
static int (*pgnutls_privkey_export_rsa_raw)(gnutls_privkey_t, gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *,
                                             gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *,
                                             gnutls_datum_t *);
static int (*pgnutls_privkey_export_dsa_raw)(gnutls_privkey_t, gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *,
                                             gnutls_datum_t *, gnutls_datum_t *);
static int (*pgnutls_privkey_generate)(gnutls_privkey_t, gnutls_pk_algorithm_t, unsigned int, unsigned int);
static int (*pgnutls_privkey_import_rsa_raw)(gnutls_privkey_t, const gnutls_datum_t *, const gnutls_datum_t *,
                                             const gnutls_datum_t *, const gnutls_datum_t *, const gnutls_datum_t *,
                                             const gnutls_datum_t *, const gnutls_datum_t *, const gnutls_datum_t *);

/* Not present in gnutls version < 3.6.0 */
static int (*pgnutls_decode_rs_value)(const gnutls_datum_t *, gnutls_datum_t *, gnutls_datum_t *);
static int (*pgnutls_x509_spki_init)(gnutls_x509_spki_t *);
static void (*pgnutls_x509_spki_deinit)(gnutls_x509_spki_t);
static void (*pgnutls_x509_spki_set_rsa_pss_params)(gnutls_x509_spki_t, gnutls_digest_algorithm_t, unsigned int);
static int (*pgnutls_pubkey_set_spki)(gnutls_pubkey_t, const gnutls_x509_spki_t, unsigned int);
static int (*pgnutls_privkey_set_spki)(gnutls_privkey_t, const gnutls_x509_spki_t, unsigned int);

static int (*pgnutls_dh_params_init)(gnutls_dh_params_t * dh_params);
static void (*pgnutls_dh_params_deinit)(gnutls_dh_params_t dh_params);
static int (*pgnutls_dh_params_generate2)(gnutls_dh_params_t dparams, unsigned int bits);
static int (*pgnutls_dh_params_import_raw2)(gnutls_dh_params_t dh_params, const gnutls_datum_t * prime,
        const gnutls_datum_t * generator, unsigned key_bits);
static int (*pgnutls_dh_params_export_raw)(gnutls_dh_params_t params, gnutls_datum_t * prime,
        gnutls_datum_t * generator, unsigned int *bits);

static int (*pgnutls_ecdh_compute_key)(gnutls_ecc_curve_t curve,
        const gnutls_datum_t *x, const gnutls_datum_t *y,
        const gnutls_datum_t *k,
        const gnutls_datum_t *peer_x, const gnutls_datum_t *peer_y,
        gnutls_datum_t *Z);

static void *libgnutls_handle;

#define MAKE_FUNCPTR(f) static typeof(f) * p##f
MAKE_FUNCPTR(gnutls_cipher_decrypt2);
MAKE_FUNCPTR(gnutls_cipher_deinit);
MAKE_FUNCPTR(gnutls_cipher_encrypt2);
MAKE_FUNCPTR(gnutls_cipher_init);
MAKE_FUNCPTR(gnutls_global_deinit);
MAKE_FUNCPTR(gnutls_global_init);
MAKE_FUNCPTR(gnutls_global_set_log_function);
MAKE_FUNCPTR(gnutls_global_set_log_level);
MAKE_FUNCPTR(gnutls_perror);
MAKE_FUNCPTR(gnutls_privkey_decrypt_data);
MAKE_FUNCPTR(gnutls_privkey_deinit);
MAKE_FUNCPTR(gnutls_privkey_import_dsa_raw);
MAKE_FUNCPTR(gnutls_privkey_init);
MAKE_FUNCPTR(gnutls_privkey_sign_hash);
MAKE_FUNCPTR(gnutls_pubkey_deinit);
MAKE_FUNCPTR(gnutls_pubkey_encrypt_data);
MAKE_FUNCPTR(gnutls_pubkey_import_privkey);
MAKE_FUNCPTR(gnutls_pubkey_init);

#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
static BOOL dh_supported;
static void *libgmp_handle;

MAKE_FUNCPTR(mpz_init);
MAKE_FUNCPTR(mpz_clear);
MAKE_FUNCPTR(mpz_cmp);
MAKE_FUNCPTR(_mpz_cmp_ui);
MAKE_FUNCPTR(mpz_sizeinbase);
MAKE_FUNCPTR(mpz_import);
MAKE_FUNCPTR(mpz_export);
MAKE_FUNCPTR(mpz_mod);
MAKE_FUNCPTR(mpz_powm);
MAKE_FUNCPTR(mpz_sub_ui);
#endif

#if defined(HAVE_GCRYPT_H) && defined(SONAME_LIBGCRYPT)
static BOOL gcrypt_available;
static void *libgcrypt_handle;

MAKE_FUNCPTR(gcry_check_version);
MAKE_FUNCPTR(gcry_sexp_build);
MAKE_FUNCPTR(gcry_pk_encrypt);
MAKE_FUNCPTR(gcry_mpi_new);
MAKE_FUNCPTR(gcry_mpi_print);
MAKE_FUNCPTR(gcry_sexp_release);
MAKE_FUNCPTR(gcry_mpi_release);
MAKE_FUNCPTR(gcry_strsource);
MAKE_FUNCPTR(gcry_strerror);
MAKE_FUNCPTR(gcry_sexp_find_token);
MAKE_FUNCPTR(gcry_sexp_nth_mpi);
MAKE_FUNCPTR(gcry_sexp_nth_data);
#endif

#undef MAKE_FUNCPTR

static int compat_gnutls_cipher_tag(gnutls_cipher_hd_t handle, void *tag, size_t tag_size)
{
    return GNUTLS_E_UNKNOWN_CIPHER_TYPE;
}

static int compat_gnutls_cipher_add_auth(gnutls_cipher_hd_t handle, const void *ptext, size_t ptext_size)
{
    return GNUTLS_E_UNKNOWN_CIPHER_TYPE;
}

static int compat_gnutls_pubkey_import_ecc_raw(gnutls_pubkey_t key, gnutls_ecc_curve_t curve,
                                               const gnutls_datum_t *x, const gnutls_datum_t *y)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_pubkey_export_ecc_raw(gnutls_pubkey_t key, gnutls_ecc_curve_t *curve,
                                               gnutls_datum_t *x, gnutls_datum_t *y)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_pubkey_export_dsa_raw(gnutls_pubkey_t key, gnutls_datum_t *p, gnutls_datum_t *q,
                                               gnutls_datum_t *g, gnutls_datum_t *y)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_pubkey_export_rsa_raw(gnutls_pubkey_t key, gnutls_datum_t *m, gnutls_datum_t *e)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_export_rsa_raw(gnutls_privkey_t key, gnutls_datum_t *m, gnutls_datum_t *e,
                                                gnutls_datum_t *d, gnutls_datum_t *p, gnutls_datum_t *q,
                                                gnutls_datum_t *u, gnutls_datum_t *e1, gnutls_datum_t *e2)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_export_ecc_raw(gnutls_privkey_t key, gnutls_ecc_curve_t *curve,
                                                gnutls_datum_t *x, gnutls_datum_t *y, gnutls_datum_t *k)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_import_ecc_raw(gnutls_privkey_t key, gnutls_ecc_curve_t curve,
                                                const gnutls_datum_t *x, const gnutls_datum_t *y,
                                                const gnutls_datum_t *k)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_export_dsa_raw(gnutls_privkey_t key, gnutls_datum_t *p, gnutls_datum_t *q,
                                                gnutls_datum_t *g, gnutls_datum_t *y, gnutls_datum_t *x)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static gnutls_sign_algorithm_t compat_gnutls_pk_to_sign(gnutls_pk_algorithm_t pk, gnutls_digest_algorithm_t hash)
{
    return GNUTLS_SIGN_UNKNOWN;
}

static int compat_gnutls_pubkey_verify_hash2(gnutls_pubkey_t key, gnutls_sign_algorithm_t algo,
                                             unsigned int flags, const gnutls_datum_t *hash,
                                             const gnutls_datum_t *signature)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_pubkey_import_rsa_raw(gnutls_pubkey_t key, const gnutls_datum_t *m, const gnutls_datum_t *e)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_pubkey_import_dsa_raw(gnutls_pubkey_t key, const gnutls_datum_t *p, const gnutls_datum_t *q,
                                               const gnutls_datum_t *g, const gnutls_datum_t *y)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_generate(gnutls_privkey_t key, gnutls_pk_algorithm_t algo, unsigned int bits,
                                          unsigned int flags)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_decode_rs_value(const gnutls_datum_t * sig_value, gnutls_datum_t * r, gnutls_datum_t * s)
{
    return GNUTLS_E_INTERNAL_ERROR;
}

static int compat_gnutls_privkey_import_rsa_raw(gnutls_privkey_t key, const gnutls_datum_t *m, const gnutls_datum_t *e,
                                                const gnutls_datum_t *d, const gnutls_datum_t *p, const gnutls_datum_t *q,
                                                const gnutls_datum_t *u, const gnutls_datum_t *e1, const gnutls_datum_t *e2)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_decrypt_data(gnutls_privkey_t key, unsigned int flags, const gnutls_datum_t *cipher_text,
                                              gnutls_datum_t *plain_text)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_pubkey_encrypt_data(gnutls_pubkey_t key, unsigned int flags, const gnutls_datum_t *cipher_text,
                                              gnutls_datum_t *plain_text)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_x509_spki_init(gnutls_x509_spki_t *spki)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static void compat_gnutls_x509_spki_deinit(gnutls_x509_spki_t spki)
{
}

static void compat_gnutls_x509_spki_set_rsa_pss_params(gnutls_x509_spki_t spki, gnutls_digest_algorithm_t dig,
                                                       unsigned int salt_size)
{
}

static int compat_gnutls_pubkey_set_spki(gnutls_pubkey_t key, const gnutls_x509_spki_t spki, unsigned int flags)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static int compat_gnutls_privkey_set_spki(gnutls_privkey_t key, const gnutls_x509_spki_t spki, unsigned int flags)
{
    return GNUTLS_E_UNKNOWN_PK_ALGORITHM;
}

static void gnutls_log( int level, const char *msg )
{
    TRACE( "<%d> %s", level, msg );
}

static NTSTATUS gnutls_process_attach( void *args )
{
    const char *env_str;
    int ret;

    if ((env_str = getenv("GNUTLS_SYSTEM_PRIORITY_FILE")))
    {
        WARN("GNUTLS_SYSTEM_PRIORITY_FILE is %s.\n", debugstr_a(env_str));
    }
    else
    {
        WARN("Setting GNUTLS_SYSTEM_PRIORITY_FILE to \"/dev/null\".\n");
        setenv("GNUTLS_SYSTEM_PRIORITY_FILE", "/dev/null", 0);
    }

    if (!(libgnutls_handle = dlopen( SONAME_LIBGNUTLS, RTLD_NOW )))
    {
        ERR_(winediag)( "failed to load libgnutls, no support for encryption\n" );
        return STATUS_DLL_NOT_FOUND;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p##f = dlsym( libgnutls_handle, #f ))) \
    { \
        ERR( "failed to load %s\n", #f ); \
        goto fail; \
    }

    LOAD_FUNCPTR(gnutls_cipher_decrypt2)
    LOAD_FUNCPTR(gnutls_cipher_deinit)
    LOAD_FUNCPTR(gnutls_cipher_encrypt2)
    LOAD_FUNCPTR(gnutls_cipher_init)
    LOAD_FUNCPTR(gnutls_global_deinit)
    LOAD_FUNCPTR(gnutls_global_init)
    LOAD_FUNCPTR(gnutls_global_set_log_function)
    LOAD_FUNCPTR(gnutls_global_set_log_level)
    LOAD_FUNCPTR(gnutls_perror)
    LOAD_FUNCPTR(gnutls_privkey_deinit);
    LOAD_FUNCPTR(gnutls_privkey_import_dsa_raw);
    LOAD_FUNCPTR(gnutls_privkey_init);
    LOAD_FUNCPTR(gnutls_privkey_sign_hash);
    LOAD_FUNCPTR(gnutls_pubkey_deinit);
    LOAD_FUNCPTR(gnutls_pubkey_import_privkey);
    LOAD_FUNCPTR(gnutls_pubkey_init);
#undef LOAD_FUNCPTR

#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
#define LOAD_FUNCPTR_STR(f) #f
#define LOAD_FUNCPTR(f) \
    if (!(p##f = dlsym( libgmp_handle, LOAD_FUNCPTR_STR(f) ))) \
    { \
        ERR( "failed to load %s\n", LOAD_FUNCPTR_STR(f) ); \
        goto fail; \
    }

    if ((libgmp_handle = dlopen( SONAME_LIBGMP, RTLD_NOW )))
    {
        LOAD_FUNCPTR(mpz_init);
        LOAD_FUNCPTR(mpz_clear);
        LOAD_FUNCPTR(mpz_cmp);
        LOAD_FUNCPTR(_mpz_cmp_ui);
        LOAD_FUNCPTR(mpz_sizeinbase);
        LOAD_FUNCPTR(mpz_import);
        LOAD_FUNCPTR(mpz_export);
        LOAD_FUNCPTR(mpz_mod);
        LOAD_FUNCPTR(mpz_powm);
        LOAD_FUNCPTR(mpz_sub_ui);
    }
    else
    {
        ERR_(winediag)( "failed to load libgmp, no support for DH\n" );
        goto fail;
    }
#undef LOAD_FUNCPTR
#undef LOAD_FUNCPTR_STR
#endif

#if defined(HAVE_GCRYPT_H) && defined(SONAME_LIBGCRYPT)
#define LOAD_FUNCPTR(f) \
    if (!(p##f = dlsym( libgcrypt_handle, #f ))) \
    { \
        WARN( "failed to load %s\n", #f ); \
        gcrypt_available = FALSE; \
    }

    if ((libgcrypt_handle = dlopen( SONAME_LIBGCRYPT, RTLD_NOW )))
    {
        gcrypt_available = TRUE;

        LOAD_FUNCPTR(gcry_check_version);
        LOAD_FUNCPTR(gcry_sexp_build);
        LOAD_FUNCPTR(gcry_pk_encrypt);
        LOAD_FUNCPTR(gcry_mpi_new);
        LOAD_FUNCPTR(gcry_mpi_print);
        LOAD_FUNCPTR(gcry_sexp_release);
        LOAD_FUNCPTR(gcry_mpi_release);
        LOAD_FUNCPTR(gcry_strsource);
        LOAD_FUNCPTR(gcry_strerror);
        LOAD_FUNCPTR(gcry_sexp_find_token);
        LOAD_FUNCPTR(gcry_sexp_nth_mpi);
        LOAD_FUNCPTR(gcry_sexp_nth_data);
    }
    else
        WARN("failed to load gcrypt, no support for ECC secret agreement\n");

#undef LOAD_FUNCPTR
#endif

#define LOAD_FUNCPTR_OPT(f) \
    if (!(p##f = dlsym( libgnutls_handle, #f ))) \
    { \
        WARN( "failed to load %s\n", #f ); \
        p##f =  compat_##f; \
    }

    LOAD_FUNCPTR_OPT(gnutls_cipher_tag)
    LOAD_FUNCPTR_OPT(gnutls_cipher_add_auth)
    LOAD_FUNCPTR_OPT(gnutls_decode_rs_value)
    LOAD_FUNCPTR_OPT(gnutls_pk_to_sign)
    LOAD_FUNCPTR_OPT(gnutls_privkey_decrypt_data)
    LOAD_FUNCPTR_OPT(gnutls_privkey_export_dsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_privkey_export_ecc_raw)
    LOAD_FUNCPTR_OPT(gnutls_privkey_export_rsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_privkey_generate)
    LOAD_FUNCPTR_OPT(gnutls_privkey_import_ecc_raw)
    LOAD_FUNCPTR_OPT(gnutls_privkey_import_rsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_privkey_set_spki)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_encrypt_data)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_export_dsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_export_ecc_raw)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_export_rsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_import_dsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_import_ecc_raw)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_import_rsa_raw)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_set_spki)
    LOAD_FUNCPTR_OPT(gnutls_pubkey_verify_hash2)
    LOAD_FUNCPTR_OPT(gnutls_x509_spki_deinit)
    LOAD_FUNCPTR_OPT(gnutls_x509_spki_init)
    LOAD_FUNCPTR_OPT(gnutls_x509_spki_set_rsa_pss_params)

#undef LOAD_FUNCPTR_OPT

    if ((ret = pgnutls_global_init()) != GNUTLS_E_SUCCESS)
    {
        pgnutls_perror( ret );
        goto fail;
    }
    if (!(pgnutls_dh_params_init = dlsym( libgnutls_handle, "gnutls_dh_params_init" )))
    {
        WARN("gnutls_dh_params_init not found\n");
    }
    if (!(pgnutls_dh_params_deinit = dlsym( libgnutls_handle, "gnutls_dh_params_deinit" )))
    {
        WARN("gnutls_dh_params_deinit not found\n");
    }
    if (!(pgnutls_dh_params_generate2 = dlsym( libgnutls_handle, "gnutls_dh_params_generate2" )))
    {
        WARN("gnutls_dh_params_generate2 not found\n");
    }
    if (!(pgnutls_dh_params_import_raw2 = dlsym( libgnutls_handle, "gnutls_dh_params_import_raw2" )))
    {
        WARN("gnutls_dh_params_import_raw2 not found\n");
    }
    if (!(pgnutls_dh_params_export_raw = dlsym( libgnutls_handle, "gnutls_dh_params_export_raw" )))
    {
        WARN("gnutls_dh_params_export_raw not found\n");
    }

#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
    dh_supported = pgnutls_dh_params_init && pgnutls_dh_params_generate2 && pgnutls_dh_params_import_raw2
            && libgmp_handle;
#else
    ERR_(winediag)("Compiled without DH support.\n");
#endif

    if (!(pgnutls_ecdh_compute_key = dlsym( libgnutls_handle, "_gnutls_ecdh_compute_key" ))
            && !(pgnutls_ecdh_compute_key = dlsym( libgnutls_handle, "gnutls_ecdh_compute_key" )))
    {
        WARN("gnutls_ecdh_compute_key not found\n");
    }

    if (TRACE_ON( bcrypt ))
    {
        pgnutls_global_set_log_level( 4 );
        pgnutls_global_set_log_function( gnutls_log );
    }

    return STATUS_SUCCESS;

fail:
    dlclose( libgnutls_handle );
    libgnutls_handle = NULL;

#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
    if (libgmp_handle)
    {
        dlclose( libgmp_handle );
        libgmp_handle = NULL;
    }
#endif
    return STATUS_DLL_NOT_FOUND;
}

static NTSTATUS gnutls_process_detach( void *args )
{
    if (libgnutls_handle)
    {
        pgnutls_global_deinit();
        dlclose( libgnutls_handle );
        libgnutls_handle = NULL;
    }
    return STATUS_SUCCESS;

#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
    dlclose( libgmp_handle );
    libgmp_handle = NULL;
#endif

#if defined(HAVE_GCRYPT_H) && defined(SONAME_LIBGCRYPT)
    dlclose( libgcrypt_handle );
    libgcrypt_handle = NULL;
#endif
}

struct buffer
{
    BYTE  *buffer;
    DWORD  length;
    DWORD  pos;
    BOOL   error;
};

static void buffer_init( struct buffer *buffer )
{
    buffer->buffer = NULL;
    buffer->length = 0;
    buffer->pos    = 0;
    buffer->error  = FALSE;
}

static void buffer_free( struct buffer *buffer )
{
    free( buffer->buffer );
}

static void buffer_append( struct buffer *buffer, BYTE *data, DWORD len )
{
    if (!len) return;

    if (buffer->pos + len > buffer->length)
    {
        DWORD new_length = max( max( buffer->pos + len, buffer->length * 2 ), 64 );
        BYTE *new_buffer;

        if (!(new_buffer = realloc( buffer->buffer, new_length )))
        {
            ERR( "out of memory\n" );
            buffer->error = TRUE;
            return;
        }

        buffer->buffer = new_buffer;
        buffer->length = new_length;
    }

    memcpy( &buffer->buffer[buffer->pos], data, len );
    buffer->pos += len;
}

static void buffer_append_byte( struct buffer *buffer, BYTE value )
{
    buffer_append( buffer, &value, sizeof(value) );
}

static void buffer_append_asn1_length( struct buffer *buffer, DWORD length )
{
    DWORD num_bytes;

    if (length < 128)
    {
        buffer_append_byte( buffer, length );
        return;
    }

    if (length <= 0xff) num_bytes = 1;
    else if (length <= 0xffff) num_bytes = 2;
    else if (length <= 0xffffff) num_bytes = 3;
    else num_bytes = 4;

    buffer_append_byte( buffer, 0x80 | num_bytes );
    while (num_bytes--) buffer_append_byte( buffer, length >> (num_bytes * 8) );
}

static void buffer_append_asn1_integer( struct buffer *buffer, BYTE *data, DWORD len )
{
    DWORD leading_zero = (*data & 0x80) != 0;

    buffer_append_byte( buffer, 0x02 );  /* tag */
    buffer_append_asn1_length( buffer, len + leading_zero );
    if (leading_zero) buffer_append_byte( buffer, 0 );
    buffer_append( buffer, data, len );
}

static void buffer_append_asn1_sequence( struct buffer *buffer, struct buffer *content )
{
    if (content->error)
    {
        buffer->error = TRUE;
        return;
    }

    buffer_append_byte( buffer, 0x30 );  /* tag */
    buffer_append_asn1_length( buffer, content->pos );
    buffer_append( buffer, content->buffer, content->pos );
}

static void buffer_append_asn1_r_s( struct buffer *buffer, BYTE *r, DWORD r_len, BYTE *s, DWORD s_len )
{
    struct buffer value;

    buffer_init( &value );
    buffer_append_asn1_integer( &value, r, r_len );
    buffer_append_asn1_integer( &value, s, s_len );
    buffer_append_asn1_sequence( buffer, &value );
    buffer_free( &value );
}

static gnutls_cipher_algorithm_t get_gnutls_cipher( const struct key *key )
{
    switch (key->alg_id)
    {
    case ALG_ID_3DES:
        WARN( "handle block size\n" );
        switch (key->u.s.mode)
        {
        case CHAIN_MODE_CBC:
            return GNUTLS_CIPHER_3DES_CBC;
        default:
            break;
        }
        FIXME( "3DES mode %u with key length %u not supported\n", key->u.s.mode, key->u.s.secret_len );
        return GNUTLS_CIPHER_UNKNOWN;

    case ALG_ID_AES:
        WARN( "handle block size\n" );
        switch (key->u.s.mode)
        {
        case CHAIN_MODE_GCM:
            if (key->u.s.secret_len == 16) return GNUTLS_CIPHER_AES_128_GCM;
            if (key->u.s.secret_len == 32) return GNUTLS_CIPHER_AES_256_GCM;
            break;
        case CHAIN_MODE_ECB: /* can be emulated with CBC + empty IV */
        case CHAIN_MODE_CBC:
            if (key->u.s.secret_len == 16) return GNUTLS_CIPHER_AES_128_CBC;
            if (key->u.s.secret_len == 24) return GNUTLS_CIPHER_AES_192_CBC;
            if (key->u.s.secret_len == 32) return GNUTLS_CIPHER_AES_256_CBC;
            break;
        case CHAIN_MODE_CFB:
            if (key->u.s.secret_len == 16) return GNUTLS_CIPHER_AES_128_CFB8;
            if (key->u.s.secret_len == 24) return GNUTLS_CIPHER_AES_192_CFB8;
            if (key->u.s.secret_len == 32) return GNUTLS_CIPHER_AES_256_CFB8;
            break;
        default:
            break;
        }
        FIXME( "AES mode %u with key length %u not supported\n", key->u.s.mode, key->u.s.secret_len );
        return GNUTLS_CIPHER_UNKNOWN;

    default:
        FIXME( "algorithm %u not supported\n", key->alg_id );
        return GNUTLS_CIPHER_UNKNOWN;
    }
}

static NTSTATUS key_symmetric_vector_reset( void *args )
{
    struct key *key = args;

    if (!key_data(key)->cipher) return STATUS_SUCCESS;
    TRACE( "invalidating cipher handle\n" );
    pgnutls_cipher_deinit( key_data(key)->cipher );
    key_data(key)->cipher = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS init_cipher_handle( struct key *key )
{
    gnutls_cipher_algorithm_t cipher;
    gnutls_datum_t secret, vector;
    int ret;

    if (key_data(key)->cipher) return STATUS_SUCCESS;
    if ((cipher = get_gnutls_cipher( key )) == GNUTLS_CIPHER_UNKNOWN) return STATUS_NOT_SUPPORTED;

    secret.data = key->u.s.secret;
    secret.size = key->u.s.secret_len;

    vector.data = key->u.s.vector;
    vector.size = key->u.s.vector_len;

    if ((ret = pgnutls_cipher_init( &key_data(key)->cipher, cipher, &secret, key->u.s.vector ? &vector : NULL )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_set_auth_data( void *args )
{
    const struct key_symmetric_set_auth_data_params *params = args;
    NTSTATUS status;
    int ret;

    if (!params->auth_data) return STATUS_SUCCESS;
    if ((status = init_cipher_handle( params->key ))) return status;

    if ((ret = pgnutls_cipher_add_auth( key_data(params->key)->cipher, params->auth_data, params->len )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_encrypt( void *args )
{
    const struct key_symmetric_encrypt_params *params = args;
    NTSTATUS status;
    int ret;

    if ((status = init_cipher_handle( params->key ))) return status;

    if ((ret = pgnutls_cipher_encrypt2( key_data(params->key)->cipher, params->input, params->input_len,
                                        params->output, params->output_len )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_decrypt( void *args )
{
    const struct key_symmetric_decrypt_params *params = args;
    NTSTATUS status;
    int ret;

    if ((status = init_cipher_handle( params->key ))) return status;

    if ((ret = pgnutls_cipher_decrypt2( key_data(params->key)->cipher, params->input, params->input_len,
                                        params->output, params->output_len )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_get_tag( void *args )
{
    const struct key_symmetric_get_tag_params *params = args;
    NTSTATUS status;
    int ret;

    if ((status = init_cipher_handle( params->key ))) return status;

    if ((ret = pgnutls_cipher_tag( key_data(params->key)->cipher, params->tag, params->len )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_symmetric_destroy( void *args )
{
    struct key *key = args;

    if (key_data(key)->cipher) pgnutls_cipher_deinit( key_data(key)->cipher );
    return STATUS_SUCCESS;
}

static ULONG export_gnutls_datum( UCHAR *buffer, ULONG buflen, gnutls_datum_t *d, BOOL zero_pad )
{
    ULONG size = d->size;
    UCHAR *src = d->data;
    ULONG offset = 0;

    assert( size <= buflen + 1 );
    if (size == buflen + 1)
    {
        assert( !src[0] );
        src++;
        size--;
    }
    if (zero_pad)
    {
        offset = buflen - size;
        if (buffer) memset( buffer, 0, offset );
        size = buflen;
    }

    if (buffer) memcpy( buffer + offset, src, size - offset );
    return size;
}

#define EXPORT_SIZE(d,l,p) export_gnutls_datum( NULL, l, &d, p )
static NTSTATUS key_export_rsa_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
    gnutls_datum_t m, e;
    ULONG size = key->u.a.bitlen / 8;
    UCHAR *dst;
    int ret;

    if (key_data(key)->a.pubkey)
        ret = pgnutls_pubkey_export_rsa_raw( key_data(key)->a.pubkey, &m, &e );
    else
        return STATUS_INVALID_PARAMETER;

    if (ret)
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    *ret_len = sizeof(*rsa_blob) + EXPORT_SIZE( e, size, 0 ) + EXPORT_SIZE( m, size, 1 );
    if (len >= *ret_len && buf)
    {
        dst = (UCHAR *)(rsa_blob + 1);
        rsa_blob->cbPublicExp = export_gnutls_datum( dst, size, &e, 0 );

        dst += rsa_blob->cbPublicExp;
        rsa_blob->cbModulus = export_gnutls_datum( dst, size, &m, 1 );

        rsa_blob->Magic     = BCRYPT_RSAPUBLIC_MAGIC;
        rsa_blob->BitLength = key->u.a.bitlen;
        rsa_blob->cbPrime1  = 0;
        rsa_blob->cbPrime2  = 0;
    }

    free( e.data ); free( m.data );
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_ecc_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
    gnutls_ecc_curve_t curve;
    gnutls_datum_t x, y;
    DWORD magic, size;
    UCHAR *dst;
    int ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256:
        magic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
        size = 32;
        break;

    case ALG_ID_ECDH_P384:
        magic = BCRYPT_ECDH_PUBLIC_P384_MAGIC;
        size = 48;
        break;

    case ALG_ID_ECDSA_P256:
        magic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        size = 32;
        break;

    case ALG_ID_ECDSA_P384:
        magic = BCRYPT_ECDSA_PUBLIC_P384_MAGIC;
        size = 48;
        break;

    default:
        FIXME( "algorithm %u not supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (key_data(key)->a.pubkey)
        ret = pgnutls_pubkey_export_ecc_raw( key_data(key)->a.pubkey, &curve, &x, &y );
    else
        return STATUS_INVALID_PARAMETER;

    if (ret)
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    if (curve != GNUTLS_ECC_CURVE_SECP256R1 && curve != GNUTLS_ECC_CURVE_SECP384R1)
    {
        FIXME( "curve %u not supported\n", curve );
        free( x.data ); free( y.data );
        return STATUS_NOT_IMPLEMENTED;
    }

    *ret_len = sizeof(*ecc_blob) + EXPORT_SIZE( x, size, 1 ) + EXPORT_SIZE( y, size, 1 );
    if (len >= *ret_len && buf)
    {
        ecc_blob->dwMagic = magic;
        ecc_blob->cbKey   = size;

        dst = (UCHAR *)(ecc_blob + 1);
        dst += export_gnutls_datum( dst, size, &x, 1 );
        export_gnutls_datum( dst, size, &y, 1 );
    }

    free( x.data ); free( y.data );
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dsa_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_DSA_KEY_BLOB *dsa_blob = (BCRYPT_DSA_KEY_BLOB *)buf;
    gnutls_datum_t p, q, g, y;
    ULONG size = key->u.a.bitlen / 8;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR *dst;
    int ret;

    if (key->u.a.bitlen > 1024)
    {
        FIXME( "bitlen > 1024 not supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (key_data(key)->a.pubkey)
        ret = pgnutls_pubkey_export_dsa_raw( key_data(key)->a.pubkey, &p, &q, &g, &y );
    else
        return STATUS_INVALID_PARAMETER;

    if (ret)
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    if (EXPORT_SIZE( q, sizeof(dsa_blob->q), 1 ) > sizeof(dsa_blob->q))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    *ret_len = sizeof(*dsa_blob) + EXPORT_SIZE( p, size, 1 ) + EXPORT_SIZE( g, size, 1 ) + EXPORT_SIZE( y, size, 1 );
    if (len >= *ret_len && buf)
    {
        dst = (UCHAR *)(dsa_blob + 1);
        dst += export_gnutls_datum( dst, size, &p, 1 );
        dst += export_gnutls_datum( dst, size, &g, 1 );
        export_gnutls_datum( dst, size, &y, 1 );

        dst = dsa_blob->q;
        export_gnutls_datum( dst, sizeof(dsa_blob->q), &q, 1 );

        dsa_blob->dwMagic = BCRYPT_DSA_PUBLIC_MAGIC;
        dsa_blob->cbKey   = size;
        memset( dsa_blob->Count, 0, sizeof(dsa_blob->Count) ); /* FIXME */
        memset( dsa_blob->Seed, 0, sizeof(dsa_blob->Seed) ); /* FIXME */
    }

done:
    free( p.data ); free( q.data ); free( g.data ); free( y.data );
    return status;
}

static void reverse_bytes( UCHAR *buf, ULONG len )
{
    unsigned int i;
    UCHAR tmp;

    for (i = 0; i < len / 2; ++i)
    {
        tmp = buf[i];
        buf[i] = buf[len - i - 1];
        buf[len - i - 1] = tmp;
    }
}

#define Q_SIZE 20
static NTSTATUS key_export_dsa_capi_public( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BLOBHEADER *hdr = (BLOBHEADER *)buf;
    DSSPUBKEY *dsskey;
    gnutls_datum_t p, q, g, y;
    ULONG size = key->u.a.bitlen / 8;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR *dst;
    int ret;

    if (key->u.a.bitlen > 1024)
    {
        FIXME( "bitlen > 1024 not supported\n" );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (key_data(key)->a.pubkey)
        ret = pgnutls_pubkey_export_dsa_raw( key_data(key)->a.pubkey, &p, &q, &g, &y );
    else if (key_data(key)->a.privkey)
        ret = pgnutls_privkey_export_dsa_raw( key_data(key)->a.privkey, &p, &q, &g, &y, NULL );
    else
        return STATUS_INVALID_PARAMETER;

    if (ret)
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    if (EXPORT_SIZE( q, Q_SIZE, 1 ) > Q_SIZE)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    *ret_len = sizeof(*hdr) + sizeof(*dsskey) + sizeof(key->u.a.dss_seed) +
               EXPORT_SIZE( p, size, 1 ) + Q_SIZE + EXPORT_SIZE( g, size, 1 ) + EXPORT_SIZE( y, size, 1 );
    if (len >= *ret_len && buf)
    {
        hdr->bType    = PUBLICKEYBLOB;
        hdr->bVersion = 2;
        hdr->reserved = 0;
        hdr->aiKeyAlg = CALG_DSS_SIGN;

        dsskey = (DSSPUBKEY *)(hdr + 1);
        dsskey->magic  = MAGIC_DSS1;
        dsskey->bitlen = key->u.a.bitlen;

        dst = (UCHAR *)(dsskey + 1);
        export_gnutls_datum( dst, size, &p, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_gnutls_datum( dst, Q_SIZE, &q, 1 );
        reverse_bytes( dst, Q_SIZE );
        dst += Q_SIZE;

        export_gnutls_datum( dst, size, &g, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_gnutls_datum( dst, size, &y, 1 );
        reverse_bytes( dst, size );
        dst += size;

        memcpy( dst, &key->u.a.dss_seed, sizeof(key->u.a.dss_seed) );
    }

done:
    free( p.data ); free( q.data ); free( g.data ); free( y.data );
    return status;
}

#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
static NTSTATUS CDECL gen_random(void *buffer, unsigned int length)
{
    unsigned int read_size;
    int dev_random;

    dev_random = open("/dev/urandom", O_RDONLY);
    if (dev_random == -1)
    {
        FIXME("couldn't open /dev/urandom.\n");
        return STATUS_INTERNAL_ERROR;
    }

    read_size = read(dev_random, buffer, length);
    close(dev_random);
    if (read_size != length)
    {
        FIXME("Could not read from /dev/urandom.");
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static void import_mpz(mpz_t value, const void *input, unsigned int length)
{
    pmpz_import(value, length, 1, 1, 0, 0, input);
}

static void export_mpz(void *output, unsigned int length, const mpz_t value)
{
    size_t export_length;
    unsigned int offset;

    export_length = (pmpz_sizeinbase(value, 2) + 7) / 8;
    assert(export_length <= length);
    offset = length - export_length;
    memset(output, 0, offset);
    pmpz_export((BYTE *)output + offset, &export_length, 1, 1, 0, 0, value);
    if (!export_length)
    {
        ERR("Zero export length, value bits %u.\n", (unsigned)pmpz_sizeinbase(value, 2));
        memset((BYTE *)output + offset, 0, length - offset);
    }
    else
    {
        assert(export_length + offset == length);
    }
}

static NTSTATUS CDECL key_dh_generate( struct key *key )
{
    NTSTATUS status = STATUS_SUCCESS;
    mpz_t p, psub1, g, privkey, pubkey;
    ULONG key_length;
    unsigned int i;
    int ret;

    if (!dh_supported)
    {
        ERR("DH is not available.\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    key_length = key->u.a.bitlen / 8;

    if (!(key->u.a.flags & KEY_FLAG_DH_PARAMS_SET))
    {
        gnutls_datum_t prime, generator;
        gnutls_dh_params_t dh_params;

        if ((ret = pgnutls_dh_params_init( &dh_params )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        dh_key_alloc( key );

        if ((ret = pgnutls_dh_params_generate2( dh_params, key->u.a.bitlen )))
        {
            pgnutls_perror( ret );
            pgnutls_dh_params_deinit( dh_params );
            return STATUS_INTERNAL_ERROR;
        }
        if ((ret = pgnutls_dh_params_export_raw( dh_params, &prime, &generator, NULL )))
        {
            pgnutls_perror( ret );
            pgnutls_dh_params_deinit( dh_params );
            return STATUS_INTERNAL_ERROR;
        }
        pgnutls_dh_params_deinit( dh_params );


        export_gnutls_datum( (UCHAR *)((BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1), key_length, &prime, 1 );
        export_gnutls_datum( (UCHAR *)((BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1) + key_length,
                key_length, &generator, 1 );
        free( prime.data );
        free( generator.data );

        key->u.a.flags |= KEY_FLAG_DH_PARAMS_SET;
    }

    pmpz_init(p);
    pmpz_init(psub1);
    pmpz_init(g);
    pmpz_init(pubkey);
    pmpz_init(privkey);

    import_mpz(p, (BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1, key_length);
    if (!mpz_sgn(p))
    {
        ERR("Got zero modulus.\n");
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }
    pmpz_sub_ui(psub1, p, 1);

    import_mpz(g, (UCHAR *)((BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1) + key_length, key_length);
    if (!mpz_sgn(g))
    {
        ERR("Got zero generator.\n");
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }
    for (i = 0; i < 3; ++i)
    {
        if ((status = gen_random(key_data(key)->d.privkey, key_length)))
        {
            goto done;
        }
        import_mpz(privkey, key_data(key)->d.privkey, key_length);

        pmpz_mod(privkey, privkey, p);
        pmpz_powm(pubkey, g, privkey, p);
        if (p_mpz_cmp_ui(pubkey, 1))
            break;
    }
    if (i == 3)
    {
        ERR("Could not generate key after 3 iterations.\n");
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }

    if (pmpz_cmp(pubkey, psub1) >= 0)
    {
        ERR("pubkey > p - 1.\n");
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }

    export_mpz(key_data(key)->d.privkey, key_length, privkey);
    export_mpz((UCHAR *)((BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1) + 2 * key_length, key_length, pubkey);

done:
    pmpz_clear(psub1);
    pmpz_clear(p);
    pmpz_clear(g);
    pmpz_clear(pubkey);
    pmpz_clear(privkey);
    return status;
}
#else
static NTSTATUS CDECL key_dh_generate( struct key *key )
{
    ERR("Compiled without DH support.\n");
    return STATUS_NOT_IMPLEMENTED;
}
#endif

static NTSTATUS key_asymmetric_generate( void *args )
{
    struct key *key = args;
    gnutls_pk_algorithm_t pk_alg;
    gnutls_privkey_t privkey;
    gnutls_pubkey_t pubkey;
    unsigned int bitlen;
    int ret;

    if (!libgnutls_handle) return STATUS_INTERNAL_ERROR;
    if (key->alg_id != ALG_ID_DH && key_data(key)->a.privkey) return STATUS_INVALID_HANDLE;

    switch (key->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        pk_alg = GNUTLS_PK_RSA;
        bitlen = key->u.a.bitlen;
        break;

    case ALG_ID_DSA:
        pk_alg = GNUTLS_PK_DSA;
        bitlen = key->u.a.bitlen;
        break;

    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDSA_P256:
        pk_alg = GNUTLS_PK_ECC; /* compatible with ECDSA and ECDH */
        bitlen = GNUTLS_CURVE_TO_BITS( GNUTLS_ECC_CURVE_SECP256R1 );
        break;

    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P384:
        pk_alg = GNUTLS_PK_ECC; /* compatible with ECDSA and ECDH */
        bitlen = GNUTLS_CURVE_TO_BITS( GNUTLS_ECC_CURVE_SECP384R1 );
        break;

    case ALG_ID_DH:
        return key_dh_generate( key );

    default:
        FIXME( "algorithm %u not supported\n", key->alg_id );
        return STATUS_NOT_SUPPORTED;
    }

    if ((ret = pgnutls_privkey_init( &privkey )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    if ((ret = pgnutls_pubkey_init( &pubkey )))
    {
        ERR("gnutls error bitlen %u.\n", bitlen);
        pgnutls_perror( ret );
        pgnutls_privkey_deinit( privkey );
        return STATUS_INTERNAL_ERROR;
    }

    if ((ret = pgnutls_privkey_generate( privkey, pk_alg, bitlen, 0 )))
    {
        pgnutls_perror( ret );
        pgnutls_privkey_deinit( privkey );
        pgnutls_pubkey_deinit( pubkey );
        return STATUS_INTERNAL_ERROR;
    }
    if ((ret = pgnutls_pubkey_import_privkey( pubkey, privkey, 0, 0 )))
    {
        pgnutls_perror( ret );
        pgnutls_privkey_deinit( privkey );
        pgnutls_pubkey_deinit( pubkey );
        return STATUS_INTERNAL_ERROR;
    }

    key_data(key)->a.privkey = privkey;
    key_data(key)->a.pubkey  = pubkey;
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_ecc( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob;
    gnutls_ecc_curve_t curve;
    gnutls_datum_t x, y, d;
    DWORD magic, size;
    UCHAR *dst;
    int ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256:
        magic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
        size = 32;
        break;

    case ALG_ID_ECDH_P384:
        magic = BCRYPT_ECDH_PRIVATE_P384_MAGIC;
        size = 48;
        break;

    case ALG_ID_ECDSA_P256:
        magic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
        size = 32;
        break;

    case ALG_ID_ECDSA_P384:
        magic = BCRYPT_ECDSA_PRIVATE_P384_MAGIC;
        size = 48;
        break;

    default:
        FIXME( "algorithm %u does not yet support exporting ecc blob\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!key_data(key)->a.privkey) return STATUS_INVALID_PARAMETER;

    if ((ret = pgnutls_privkey_export_ecc_raw( key_data(key)->a.privkey, &curve, &x, &y, &d )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    if (curve != GNUTLS_ECC_CURVE_SECP256R1 && curve != GNUTLS_ECC_CURVE_SECP384R1)
    {
        FIXME( "curve %u not supported\n", curve );
        free( x.data ); free( y.data ); free( d.data );
        return STATUS_NOT_IMPLEMENTED;
    }

    *ret_len = sizeof(*ecc_blob) + EXPORT_SIZE( x, size, 1 ) + EXPORT_SIZE( y, size, 1 ) + EXPORT_SIZE( d, size, 1 );
    if (len >= *ret_len && buf)
    {
        ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
        ecc_blob->dwMagic = magic;
        ecc_blob->cbKey   = size;

        dst = (UCHAR *)(ecc_blob + 1);
        dst += export_gnutls_datum( dst, size, &x, 1 );
        dst += export_gnutls_datum( dst, size, &y, 1 );
        export_gnutls_datum( dst, size, &d, 1 );
    }

    free( x.data ); free( y.data ); free( d.data );
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_ecc( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob;
    gnutls_ecc_curve_t curve;
    gnutls_privkey_t handle;
    gnutls_datum_t x, y, k;
    int ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDSA_P256:
        curve = GNUTLS_ECC_CURVE_SECP256R1;
        break;

    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P384:
        curve = GNUTLS_ECC_CURVE_SECP384R1;
        break;

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((ret = pgnutls_privkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
    x.data = (unsigned char *)(ecc_blob + 1);
    x.size = ecc_blob->cbKey;
    y.data = x.data + ecc_blob->cbKey;
    y.size = ecc_blob->cbKey;
    k.data = y.data + ecc_blob->cbKey;
    k.size = ecc_blob->cbKey;

    if ((ret = pgnutls_privkey_import_ecc_raw( handle, curve, &x, &y, &k )))
    {
        pgnutls_perror( ret );
        pgnutls_privkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    if (key_data(key)->a.privkey) pgnutls_privkey_deinit( key_data(key)->a.privkey );
    key_data(key)->a.privkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_rsa( struct key *key, ULONG flags, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob;
    gnutls_datum_t m, e, d, p, q, u, e1, e2;
    ULONG size = key->u.a.bitlen / 8;
    BOOL full = (flags & KEY_EXPORT_FLAG_RSA_FULL);
    UCHAR *dst;
    int ret;

    if (!key_data(key)->a.privkey) return STATUS_INVALID_PARAMETER;

    if ((ret = pgnutls_privkey_export_rsa_raw( key_data(key)->a.privkey, &m, &e, &d, &p, &q, &u, &e1, &e2 )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    *ret_len = sizeof(*rsa_blob) + EXPORT_SIZE( e, size, 0 ) + EXPORT_SIZE( m, size, 1 ) +
               EXPORT_SIZE( p, size / 2, 1 ) + EXPORT_SIZE( q, size / 2, 1 );

    if (full) *ret_len += EXPORT_SIZE( e1, size / 2, 1 ) + EXPORT_SIZE( e2, size / 2, 1 ) +
                          EXPORT_SIZE( u, size / 2, 1 ) + EXPORT_SIZE( d, size, 1 );

    if (len >= *ret_len && buf)
    {
        rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
        rsa_blob->Magic     = full ? BCRYPT_RSAFULLPRIVATE_MAGIC : BCRYPT_RSAPRIVATE_MAGIC;
        rsa_blob->BitLength = key->u.a.bitlen;

        dst = (UCHAR *)(rsa_blob + 1);
        rsa_blob->cbPublicExp = export_gnutls_datum( dst, size, &e, 0 );

        dst += rsa_blob->cbPublicExp;
        rsa_blob->cbModulus = export_gnutls_datum( dst, size, &m, 1 );

        dst += rsa_blob->cbModulus;
        rsa_blob->cbPrime1 = export_gnutls_datum( dst, size / 2, &p, 1 );

        dst += rsa_blob->cbPrime1;
        rsa_blob->cbPrime2 = export_gnutls_datum( dst, size / 2, &q, 1 );

        if (full)
        {
            dst += rsa_blob->cbPrime2;
            export_gnutls_datum( dst, size / 2, &e1, 1 );

            dst += rsa_blob->cbPrime1;
            export_gnutls_datum( dst, size / 2, &e2, 1 );

            dst += rsa_blob->cbPrime2;
            export_gnutls_datum( dst, size / 2, &u, 1 );

            dst += rsa_blob->cbPrime1;
            export_gnutls_datum( dst, size, &d, 1 );
        }
    }

    free( m.data ); free( e.data ); free( d.data ); free( p.data ); free( q.data ); free( u.data );
    free( e1.data ); free( e2.data );
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_rsa( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
    gnutls_datum_t m, e, p, q;
    gnutls_privkey_t handle;
    int ret;

    if ((ret = pgnutls_privkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    e.data = (unsigned char *)(rsa_blob + 1);
    e.size = rsa_blob->cbPublicExp;
    m.data = e.data + e.size;
    m.size = rsa_blob->cbModulus;
    p.data = m.data + m.size;
    p.size = rsa_blob->cbPrime1;
    q.data = p.data + p.size;
    q.size = rsa_blob->cbPrime2;

    if ((ret = pgnutls_privkey_import_rsa_raw( handle, &m, &e, NULL, &p, &q, NULL, NULL, NULL )))
    {
        pgnutls_perror( ret );
        pgnutls_privkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    if (key_data(key)->a.privkey) pgnutls_privkey_deinit( key_data(key)->a.privkey );
    key_data(key)->a.privkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_export_dsa_capi( struct key *key, UCHAR *buf, ULONG len, ULONG *ret_len )
{
    BLOBHEADER *hdr;
    DSSPUBKEY *pubkey;
    gnutls_datum_t p, q, g, y, x;
    ULONG size = key->u.a.bitlen / 8;
    UCHAR *dst;
    int ret;

    if (!key_data(key)->a.privkey) return STATUS_INVALID_PARAMETER;

    if ((ret = pgnutls_privkey_export_dsa_raw( key_data(key)->a.privkey, &p, &q, &g, &y, &x )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    if (q.size > 21 || x.size > 21)
    {
        ERR( "can't export key in this format\n" );
        free( p.data ); free( q.data ); free( g.data ); free( y.data ); free( x.data );
        return STATUS_NOT_SUPPORTED;
    }

    *ret_len = sizeof(*hdr) + sizeof(*pubkey) + sizeof(key->u.a.dss_seed) +
               EXPORT_SIZE( p, size, 1 ) + 20 + EXPORT_SIZE( g, size, 1 ) + 20;
    if (len >= *ret_len && buf)
    {
        hdr = (BLOBHEADER *)buf;
        hdr->bType    = PRIVATEKEYBLOB;
        hdr->bVersion = 2;
        hdr->reserved = 0;
        hdr->aiKeyAlg = CALG_DSS_SIGN;

        pubkey = (DSSPUBKEY *)(hdr + 1);
        pubkey->magic  = MAGIC_DSS2;
        pubkey->bitlen = key->u.a.bitlen;

        dst = (UCHAR *)(pubkey + 1);
        export_gnutls_datum( dst, size, &p, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_gnutls_datum( dst, 20, &q, 1 );
        reverse_bytes( dst, 20 );
        dst += 20;

        export_gnutls_datum( dst, size, &g, 1 );
        reverse_bytes( dst, size );
        dst += size;

        export_gnutls_datum( dst, 20, &x, 1 );
        reverse_bytes( dst, 20 );
        dst += 20;

        memcpy( dst, &key->u.a.dss_seed, sizeof(key->u.a.dss_seed) );
    }

    free( p.data ); free( q.data ); free( g.data ); free( y.data ); free( x.data );
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dsa_capi( struct key *key, UCHAR *buf, ULONG len )
{
    BLOBHEADER *hdr = (BLOBHEADER *)buf;
    DSSPUBKEY *pubkey;
    gnutls_privkey_t handle;
    gnutls_datum_t p, q, g, x;
    unsigned char *data, p_data[128], q_data[20], g_data[128], x_data[20];
    int i, ret, size;

    if ((ret = pgnutls_privkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    pubkey = (DSSPUBKEY *)(hdr + 1);
    if ((size = pubkey->bitlen / 8) > sizeof(p_data))
    {
        FIXME( "size %u not supported\n", size );
        pgnutls_privkey_deinit( handle );
        return STATUS_NOT_SUPPORTED;
    }
    data = (unsigned char *)(pubkey + 1);

    p.data = p_data;
    p.size = size;
    for (i = 0; i < p.size; i++) p.data[i] = data[p.size - i - 1];
    data += p.size;

    q.data = q_data;
    q.size = sizeof(q_data);
    for (i = 0; i < q.size; i++) q.data[i] = data[q.size - i - 1];
    data += q.size;

    g.data = g_data;
    g.size = size;
    for (i = 0; i < g.size; i++) g.data[i] = data[g.size - i - 1];
    data += g.size;

    x.data = x_data;
    x.size = sizeof(x_data);
    for (i = 0; i < x.size; i++) x.data[i] = data[x.size - i - 1];
    data += x.size;

    if ((ret = pgnutls_privkey_import_dsa_raw( handle, &p, &q, &g, NULL, &x )))
    {
        pgnutls_perror( ret );
        pgnutls_privkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    memcpy( &key->u.a.dss_seed, data, sizeof(key->u.a.dss_seed) );

    if (key_data(key)->a.privkey) pgnutls_privkey_deinit( key_data(key)->a.privkey );
    key_data(key)->a.privkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_ecc_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_ECCKEY_BLOB *ecc_blob;
    gnutls_ecc_curve_t curve;
    gnutls_datum_t x, y;
    gnutls_pubkey_t handle;
    int ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDSA_P256:
        curve = GNUTLS_ECC_CURVE_SECP256R1; break;

    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P384:
        curve = GNUTLS_ECC_CURVE_SECP384R1; break;

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((ret = pgnutls_pubkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    ecc_blob = (BCRYPT_ECCKEY_BLOB *)buf;
    x.data = buf + sizeof(*ecc_blob);
    x.size = ecc_blob->cbKey;
    y.data = buf + sizeof(*ecc_blob) + ecc_blob->cbKey;
    y.size = ecc_blob->cbKey;

    if ((ret = pgnutls_pubkey_import_ecc_raw( handle, curve, &x, &y )))
    {
        pgnutls_perror( ret );
        pgnutls_pubkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    if (key_data(key)->a.pubkey) pgnutls_pubkey_deinit( key_data(key)->a.pubkey );
    key_data(key)->a.pubkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_rsa_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_RSAKEY_BLOB *rsa_blob;
    gnutls_pubkey_t handle;
    gnutls_datum_t m, e;
    int ret;

    if ((ret = pgnutls_pubkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    rsa_blob = (BCRYPT_RSAKEY_BLOB *)buf;
    e.data = buf + sizeof(*rsa_blob);
    e.size = rsa_blob->cbPublicExp;
    m.data = buf + sizeof(*rsa_blob) + rsa_blob->cbPublicExp;
    m.size = rsa_blob->cbModulus;

    if ((ret = pgnutls_pubkey_import_rsa_raw( handle, &m, &e )))
    {
        pgnutls_perror( ret );
        pgnutls_pubkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    if (key_data(key)->a.pubkey) pgnutls_pubkey_deinit( key_data(key)->a.pubkey );
    key_data(key)->a.pubkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dsa_public( struct key *key, UCHAR *buf, ULONG len )
{
    BCRYPT_DSA_KEY_BLOB *dsa_blob;
    gnutls_datum_t p, q, g, y;
    gnutls_pubkey_t handle;
    int ret;

    if ((ret = pgnutls_pubkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    dsa_blob = (BCRYPT_DSA_KEY_BLOB *)buf;
    p.data = buf + sizeof(*dsa_blob);
    p.size = dsa_blob->cbKey;
    q.data = dsa_blob->q;
    q.size = sizeof(dsa_blob->q);
    g.data = buf + sizeof(*dsa_blob) + dsa_blob->cbKey;
    g.size = dsa_blob->cbKey;
    y.data = buf + sizeof(*dsa_blob) + dsa_blob->cbKey * 2;
    y.size = dsa_blob->cbKey;

    if ((ret = pgnutls_pubkey_import_dsa_raw( handle, &p, &q, &g, &y )))
    {
        pgnutls_perror( ret );
        pgnutls_pubkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    if (key_data(key)->a.pubkey) pgnutls_pubkey_deinit( key_data(key)->a.pubkey );
    key_data(key)->a.pubkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_import_dsa_capi_public( struct key *key, UCHAR *buf, ULONG len )
{
    BLOBHEADER *hdr;
    DSSPUBKEY *pubkey;
    gnutls_datum_t p, q, g, y;
    gnutls_pubkey_t handle;
    unsigned char *data, p_data[128], q_data[20], g_data[128], y_data[128];
    int i, ret, size;

    if ((ret = pgnutls_pubkey_init( &handle )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    hdr = (BLOBHEADER *)buf;
    pubkey = (DSSPUBKEY *)(hdr + 1);
    size = pubkey->bitlen / 8;
    data = (unsigned char *)(pubkey + 1);

    p.data = p_data;
    p.size = size;
    for (i = 0; i < p.size; i++) p.data[i] = data[p.size - i - 1];
    data += p.size;

    q.data = q_data;
    q.size = sizeof(q_data);
    for (i = 0; i < q.size; i++) q.data[i] = data[q.size - i - 1];
    data += q.size;

    g.data = g_data;
    g.size = size;
    for (i = 0; i < g.size; i++) g.data[i] = data[g.size - i - 1];
    data += g.size;

    y.data = y_data;
    y.size = sizeof(y_data);
    for (i = 0; i < y.size; i++) y.data[i] = data[y.size - i - 1];

    if ((ret = pgnutls_pubkey_import_dsa_raw( handle, &p, &q, &g, &y )))
    {
        pgnutls_perror( ret );
        pgnutls_pubkey_deinit( handle );
        return STATUS_INTERNAL_ERROR;
    }

    if (key_data(key)->a.pubkey) pgnutls_pubkey_deinit( key_data(key)->a.pubkey );
    key_data(key)->a.pubkey = handle;
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_export( void *args )
{
    const struct key_asymmetric_export_params *params = args;
    struct key *key = params->key;
    unsigned flags = params->flags;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
            return key_export_ecc_public( key, params->buf, params->len, params->ret_len );
        return key_export_ecc( key, params->buf, params->len, params->ret_len );

    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
            return key_export_rsa_public( key, params->buf, params->len, params->ret_len );
        return key_export_rsa( key, flags, params->buf, params->len, params->ret_len );

    case ALG_ID_DSA:
        if (flags & KEY_EXPORT_FLAG_PUBLIC)
        {
            if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
                return key_export_dsa_capi_public( key, params->buf, params->len, params->ret_len );
            return key_export_dsa_public( key, params->buf, params->len, params->ret_len );
        }
        if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
            return key_export_dsa_capi( key, params->buf, params->len, params->ret_len );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_DH:
        if (!(key->u.a.flags & KEY_FLAG_FINALIZED)) return STATUS_INVALID_HANDLE;
        if (flags & KEY_EXPORT_FLAG_DH_PARAMETERS)
        {
            BCRYPT_DH_PARAMETER_HEADER *h;
            unsigned int data_size;

            data_size = sizeof(BCRYPT_DH_PARAMETER_HEADER) + key->u.a.bitlen / 8 * 2;
            if (params->ret_len) *params->ret_len = data_size;
            if (!params->buf) return STATUS_SUCCESS;
            if (params->len < data_size) return STATUS_BUFFER_TOO_SMALL;

            h = (BCRYPT_DH_PARAMETER_HEADER *)params->buf;
            h->cbLength = data_size;
            h->dwMagic = BCRYPT_DH_PARAMETERS_MAGIC;
            h->cbKeyLength = key->u.a.bitlen / 8;
            memcpy( h + 1, (BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1, h->cbKeyLength * 2);
        }
        else
        {
            BCRYPT_DH_KEY_BLOB *h = (BCRYPT_DH_KEY_BLOB *)params->buf;
            BOOL dh_private = flags & KEY_EXPORT_FLAG_DH_FULL;

            *params->ret_len = dh_pubkey_len( key );
            if (dh_private)
                *params->ret_len += key->u.a.bitlen / 8;

            if (params->len < *params->ret_len) return STATUS_SUCCESS;

            memcpy(params->buf, key_data(key)->d.pubkey, dh_pubkey_len( key ));
            if (dh_private)
                memcpy(params->buf + dh_pubkey_len( key ), key_data(key)->d.privkey, key->u.a.bitlen / 8);

            h->dwMagic = dh_private ? BCRYPT_DH_PRIVATE_MAGIC : BCRYPT_DH_PUBLIC_MAGIC;
            h->cbKey = key->u.a.bitlen / 8;
        }
        return STATUS_SUCCESS;

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

static NTSTATUS key_asymmetric_import( void *args )
{
    const struct key_asymmetric_import_params *params = args;
    struct key *key = params->key;
    unsigned flags = params->flags;
    gnutls_pubkey_t pubkey;
    NTSTATUS ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
            return key_import_ecc_public( key, params->buf, params->len );
        ret = key_import_ecc( key, params->buf, params->len );
        break;

    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
            return key_import_rsa_public( key, params->buf, params->len );
        ret = key_import_rsa( key, params->buf, params->len );
        break;

    case ALG_ID_DSA:
        if (flags & KEY_IMPORT_FLAG_PUBLIC)
        {
            if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
                return key_import_dsa_capi_public( key, params->buf, params->len );
            return key_import_dsa_public( key, params->buf, params->len );
        }
        if (key->u.a.flags & KEY_FLAG_LEGACY_DSA_V2)
        {
            ret = key_import_dsa_capi( key, params->buf, params->len );
            break;
        }
        FIXME( "DSA private key not supported\n" );
        return STATUS_NOT_IMPLEMENTED;

    case ALG_ID_DH:
        if (flags & KEY_IMPORT_FLAG_DH_PARAMETERS)
        {
            const BCRYPT_DH_PARAMETER_HEADER *h = (const BCRYPT_DH_PARAMETER_HEADER *)params->buf;
            ULONG param_size = sizeof(BCRYPT_DH_PARAMETER_HEADER) + key->u.a.bitlen / 8 * 2;

            if (key->u.a.flags & KEY_FLAG_FINALIZED) return STATUS_INVALID_HANDLE;
            if (params->len < param_size) return STATUS_BUFFER_TOO_SMALL;
            if (!h || h->cbLength != param_size || h->dwMagic != BCRYPT_DH_PARAMETERS_MAGIC
                    || h->cbKeyLength != key->u.a.bitlen / 8)
                return STATUS_INVALID_PARAMETER;

            dh_key_alloc( key );
            memcpy((BCRYPT_DH_KEY_BLOB *)key_data(key)->d.pubkey + 1, h + 1, h->cbKeyLength * 2);
            key->u.a.flags |= KEY_FLAG_DH_PARAMS_SET;
        }
        else
        {
            BCRYPT_DH_KEY_BLOB *h = (BCRYPT_DH_KEY_BLOB *)params->buf;
            BOOL dh_private = flags & KEY_IMPORT_FLAG_DH_FULL;
            ULONG size;

            if (h->dwMagic != (dh_private ? BCRYPT_DH_PRIVATE_MAGIC : BCRYPT_DH_PUBLIC_MAGIC))
            {
                WARN("unexpected dwMagic %#x.\n", (int)h->dwMagic);
                return STATUS_INVALID_PARAMETER;
            }

            size = sizeof(*h) + h->cbKey * 3;
            if (dh_private)
                size += h->cbKey;
            if (params->len != size) return STATUS_INVALID_PARAMETER;
            if (h->cbKey * 8 < 512) return STATUS_INVALID_PARAMETER;

            dh_key_alloc( key );

            memcpy( key_data(key)->d.pubkey, params->buf, dh_pubkey_len( key ));

            if (dh_private)
                memcpy( key_data(key)->d.privkey, params->buf + sizeof(*h) + h->cbKey * 3, h->cbKey);
        }
        return STATUS_SUCCESS;

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (ret) return ret;

    if ((ret = pgnutls_pubkey_init( &pubkey )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    if (pgnutls_pubkey_import_privkey( pubkey, key_data(params->key)->a.privkey, 0, 0 ))
    {
        /* Imported private key may be legitimately missing public key, so ignore the failure here. */
        pgnutls_pubkey_deinit( pubkey );
    }
    else
    {
        if (key_data(key)->a.pubkey) pgnutls_pubkey_deinit( key_data(key)->a.pubkey );
        key_data(key)->a.pubkey = pubkey;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS prepare_gnutls_signature_dsa( struct key *key, UCHAR *signature, ULONG signature_len,
                                              gnutls_datum_t *gnutls_signature )
{
    struct buffer buffer;
    DWORD r_len = signature_len / 2;
    DWORD s_len = r_len;
    BYTE *r = signature;
    BYTE *s = signature + r_len;

    buffer_init( &buffer );
    buffer_append_asn1_r_s( &buffer, r, r_len, s, s_len );
    if (buffer.error)
    {
        buffer_free( &buffer );
        return STATUS_NO_MEMORY;
    }

    gnutls_signature->data = buffer.buffer;
    gnutls_signature->size = buffer.pos;
    return STATUS_SUCCESS;
}

static NTSTATUS prepare_gnutls_signature_rsa( struct key *key, UCHAR *signature, ULONG signature_len,
                                              gnutls_datum_t *gnutls_signature )
{
    gnutls_signature->data = signature;
    gnutls_signature->size = signature_len;
    return STATUS_SUCCESS;
}

static NTSTATUS prepare_gnutls_signature( struct key *key, UCHAR *signature, ULONG signature_len,
                                          gnutls_datum_t *gnutls_signature )
{
    switch (key->alg_id)
    {
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_DSA:
        return prepare_gnutls_signature_dsa( key, signature, signature_len, gnutls_signature );

    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
        return prepare_gnutls_signature_rsa( key, signature, signature_len, gnutls_signature );

    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }
}

static gnutls_digest_algorithm_t get_digest_from_id( const WCHAR *alg_id )
{
    if (!wcscmp( alg_id, BCRYPT_SHA1_ALGORITHM ))   return GNUTLS_DIG_SHA1;
    if (!wcscmp( alg_id, BCRYPT_SHA256_ALGORITHM )) return GNUTLS_DIG_SHA256;
    if (!wcscmp( alg_id, BCRYPT_SHA384_ALGORITHM )) return GNUTLS_DIG_SHA384;
    if (!wcscmp( alg_id, BCRYPT_SHA512_ALGORITHM )) return GNUTLS_DIG_SHA512;
    if (!wcscmp( alg_id, BCRYPT_MD2_ALGORITHM ))    return GNUTLS_DIG_MD2;
    if (!wcscmp( alg_id, BCRYPT_MD5_ALGORITHM ))    return GNUTLS_DIG_MD5;
    return GNUTLS_DIG_UNKNOWN;
}

static NTSTATUS pubkey_set_rsa_pss_params( gnutls_pubkey_t key, gnutls_digest_algorithm_t dig, unsigned int salt_size )
{
    gnutls_x509_spki_t spki;
    int ret;

    if (((ret = pgnutls_x509_spki_init( &spki ) < 0)))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    pgnutls_x509_spki_set_rsa_pss_params( spki, dig, salt_size );
    ret = pgnutls_pubkey_set_spki( key, spki, 0 );
    pgnutls_x509_spki_deinit( spki );
    if (ret < 0)
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_verify( void *args )
{
#ifdef GNUTLS_VERIFY_ALLOW_BROKEN
    static const unsigned int verify_flags = GNUTLS_VERIFY_ALLOW_BROKEN;
#else
    static const unsigned int verify_flags = 0;
#endif
    const struct key_asymmetric_verify_params *params = args;
    struct key *key = params->key;
    unsigned flags = params->flags;
    gnutls_digest_algorithm_t hash_alg;
    gnutls_sign_algorithm_t sign_alg;
    gnutls_datum_t gnutls_hash, gnutls_signature;
    gnutls_pk_algorithm_t pk_alg;
    NTSTATUS status;
    int ret;

    switch (key->alg_id)
    {
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    {
        if (flags) FIXME( "flags %#x not supported\n", flags );

        /* only the hash size must match, not the actual hash function */
        switch (params->hash_len)
        {
        case 20: hash_alg = GNUTLS_DIG_SHA1; break;
        case 32: hash_alg = GNUTLS_DIG_SHA256; break;
        case 48: hash_alg = GNUTLS_DIG_SHA384; break;

        default:
            FIXME( "hash size %u not yet supported\n", params->hash_len );
            return STATUS_INVALID_SIGNATURE;
        }
        pk_alg = GNUTLS_PK_ECC;
        break;
    }
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    {
        if (flags & BCRYPT_PAD_PKCS1)
        {
            BCRYPT_PKCS1_PADDING_INFO *info = params->padding;

            if (!info) return STATUS_INVALID_PARAMETER;
            if (!info->pszAlgId) return STATUS_INVALID_SIGNATURE;
            if ((hash_alg = get_digest_from_id(info->pszAlgId)) == GNUTLS_DIG_UNKNOWN)
            {
                FIXME( "hash algorithm %s not supported\n", debugstr_w(info->pszAlgId) );
                return STATUS_NOT_SUPPORTED;
            }
            pk_alg = GNUTLS_PK_RSA;
        }
        else if (flags & BCRYPT_PAD_PSS)
        {
            BCRYPT_PSS_PADDING_INFO *info = params->padding;

            if (!info) return STATUS_INVALID_PARAMETER;
            if (!info->pszAlgId) return STATUS_INVALID_SIGNATURE;
            if ((hash_alg = get_digest_from_id(info->pszAlgId)) == GNUTLS_DIG_UNKNOWN)
            {
                FIXME( "hash algorithm %s not supported\n", debugstr_w(info->pszAlgId) );
                return STATUS_NOT_SUPPORTED;
            }
            if ((status = pubkey_set_rsa_pss_params( key_data(key)->a.pubkey, hash_alg, info->cbSalt ))) return status;
            pk_alg = GNUTLS_PK_RSA_PSS;
        }
        else return STATUS_INVALID_PARAMETER;
        break;
    }
    case ALG_ID_DSA:
    {
        if (flags) FIXME( "flags %#x not supported\n", flags );
        if (params->hash_len != 20)
        {
            FIXME( "hash size %u not supported\n", params->hash_len );
            return STATUS_INVALID_PARAMETER;
        }
        hash_alg = GNUTLS_DIG_SHA1;
        pk_alg = GNUTLS_PK_DSA;
        break;
    }
    default:
        FIXME( "algorithm %u not yet supported\n", key->alg_id );
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((sign_alg = pgnutls_pk_to_sign( pk_alg, hash_alg )) == GNUTLS_SIGN_UNKNOWN)
    {
        FIXME("GnuTLS does not support algorithm %u with hash len %u\n", key->alg_id, params->hash_len );
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((status = prepare_gnutls_signature( key, params->signature, params->signature_len, &gnutls_signature )))
        return status;

    gnutls_hash.data = params->hash;
    gnutls_hash.size = params->hash_len;

    ret = pgnutls_pubkey_verify_hash2( key_data(key)->a.pubkey, sign_alg, verify_flags, &gnutls_hash, &gnutls_signature );
    if (gnutls_signature.data != params->signature) free( gnutls_signature.data );
    return (ret < 0) ? STATUS_INVALID_SIGNATURE : STATUS_SUCCESS;
}

static unsigned int get_signature_length( enum alg_id id )
{
    switch (id)
    {
    case ALG_ID_ECDSA_P256: return 64;
    case ALG_ID_ECDSA_P384: return 96;
    case ALG_ID_DSA:        return 40;
    default:
        FIXME( "unhandled algorithm %u\n", id );
        return 0;
    }
}

static NTSTATUS format_gnutls_signature( enum alg_id type, gnutls_datum_t signature,
                                         UCHAR *output, ULONG output_len, ULONG *ret_len )
{
    switch (type)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    {
        *ret_len = signature.size;
        if (output_len < signature.size) return STATUS_BUFFER_TOO_SMALL;
        if (output) memcpy( output, signature.data, signature.size );
        return STATUS_SUCCESS;
    }
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    case ALG_ID_DSA:
    {
        int err;
        unsigned int sig_len = get_signature_length( type );
        gnutls_datum_t r, s; /* format as r||s */

        if ((err = pgnutls_decode_rs_value( &signature, &r, &s )))
        {
            pgnutls_perror( err );
            return STATUS_INTERNAL_ERROR;
        }

        *ret_len = sig_len;
        if (output_len < sig_len) return STATUS_BUFFER_TOO_SMALL;

        if (r.size > sig_len / 2 + 1 || s.size > sig_len / 2 + 1)
        {
            ERR( "we didn't get a correct signature\n" );
            return STATUS_INTERNAL_ERROR;
        }

        if (output)
        {
            export_gnutls_datum( output, sig_len / 2, &r, 1 );
            export_gnutls_datum( output + sig_len / 2, sig_len / 2, &s, 1 );
        }

        free( r.data ); free( s.data );
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INTERNAL_ERROR;
    }
}

static NTSTATUS privkey_set_rsa_pss_params( gnutls_privkey_t key, gnutls_digest_algorithm_t dig, unsigned int salt_size )
{
    gnutls_x509_spki_t spki;
    int ret;

    if (((ret = pgnutls_x509_spki_init( &spki ) < 0)))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    pgnutls_x509_spki_set_rsa_pss_params( spki, dig, salt_size );
    ret = pgnutls_privkey_set_spki( key, spki, 0 );
    pgnutls_x509_spki_deinit( spki );
    if (ret < 0)
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_sign( void *args )
{
    const struct key_asymmetric_sign_params *params = args;
    struct key *key = params->key;
    unsigned int flags = params->flags, gnutls_flags = 0;
    gnutls_datum_t hash, signature;
    gnutls_digest_algorithm_t hash_alg;
    NTSTATUS status;
    int ret;

    if (key->alg_id == ALG_ID_ECDSA_P256 || key->alg_id == ALG_ID_ECDSA_P384)
    {
        /* With ECDSA, we find the digest algorithm from the hash length, and verify it */
        switch (params->input_len)
        {
        case 20: hash_alg = GNUTLS_DIG_SHA1; break;
        case 32: hash_alg = GNUTLS_DIG_SHA256; break;
        case 48: hash_alg = GNUTLS_DIG_SHA384; break;
        case 64: hash_alg = GNUTLS_DIG_SHA512; break;

        default:
            FIXME( "hash size %u not yet supported\n", params->input_len );
            return STATUS_INVALID_PARAMETER;
        }

        if (flags == BCRYPT_PAD_PKCS1)
        {
            BCRYPT_PKCS1_PADDING_INFO *pad = params->padding;
            if (pad && pad->pszAlgId && get_digest_from_id( pad->pszAlgId ) != hash_alg)
            {
                WARN( "incorrect hashing algorithm %s, expected %u\n", debugstr_w(pad->pszAlgId), hash_alg );
                return STATUS_INVALID_PARAMETER;
            }
        }
    }
    else if (key->alg_id == ALG_ID_DSA)
    {
        if (flags) FIXME( "flags %#x not supported\n", flags );
        if (params->input_len != 20)
        {
            FIXME( "hash size %u not supported\n", params->input_len );
            return STATUS_INVALID_PARAMETER;
        }
        hash_alg = GNUTLS_DIG_SHA1;
    }
    else if (flags == BCRYPT_PAD_PKCS1)
    {
        BCRYPT_PKCS1_PADDING_INFO *pad = params->padding;

        if (!pad || !pad->pszAlgId)
        {
            WARN( "padding info not found\n" );
            return STATUS_INVALID_PARAMETER;
        }
        if ((hash_alg = get_digest_from_id( pad->pszAlgId )) == GNUTLS_DIG_UNKNOWN)
        {
            FIXME( "hash algorithm %s not recognized\n", debugstr_w(pad->pszAlgId) );
            return STATUS_NOT_SUPPORTED;
        }
    }
    else if (flags == BCRYPT_PAD_PSS)
    {
        BCRYPT_PSS_PADDING_INFO *pad = params->padding;

        if (!pad || !pad->pszAlgId)
        {
            WARN( "padding info not found\n" );
            return STATUS_INVALID_PARAMETER;
        }
        if (key->alg_id != ALG_ID_RSA && key->alg_id != ALG_ID_RSA_SIGN)
        {
            FIXME( "BCRYPT_PAD_PSS not supported for key algorithm %u\n", key->alg_id );
            return STATUS_NOT_SUPPORTED;
        }
        if ((hash_alg = get_digest_from_id( pad->pszAlgId )) == GNUTLS_DIG_UNKNOWN)
        {
            FIXME( "hash algorithm %s not recognized\n", debugstr_w(pad->pszAlgId) );
            return STATUS_NOT_SUPPORTED;
        }

        if ((status = privkey_set_rsa_pss_params( key_data(key)->a.privkey, hash_alg, pad->cbSalt ))) return status;
        gnutls_flags = GNUTLS_PRIVKEY_SIGN_FLAG_RSA_PSS;
    }
    else if (!flags)
    {
         WARN( "invalid flags %#x\n", flags );
         return STATUS_INVALID_PARAMETER;
    }
    else
    {
        FIXME( "flags %#x not implemented\n", flags );
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!params->output)
    {
        *params->ret_len = key->u.a.bitlen / 8;
        return STATUS_SUCCESS;
    }
    if (!key_data(key)->a.privkey) return STATUS_INVALID_PARAMETER;

    hash.data = params->input;
    hash.size = params->input_len;

    signature.data = NULL;
    signature.size = 0;

    if ((ret = pgnutls_privkey_sign_hash( key_data(key)->a.privkey, hash_alg, gnutls_flags, &hash, &signature )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    status = format_gnutls_signature( key->alg_id, signature, params->output, params->output_len, params->ret_len );
    free( signature.data );
    return status;
}

static NTSTATUS key_asymmetric_destroy( void *args )
{
    struct key *key = args;

    if (key->alg_id == ALG_ID_DH)
    {
        dh_key_free( key );
    }
    else
    {
        if (key_data(key)->a.privkey) pgnutls_privkey_deinit( key_data(key)->a.privkey );
        if (key_data(key)->a.pubkey) pgnutls_pubkey_deinit( key_data(key)->a.pubkey );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS dup_privkey( struct key *key_orig, struct key *key_copy )
{
    gnutls_privkey_t privkey;
    int ret;

    if ((ret = pgnutls_privkey_init( &privkey )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    switch (key_orig->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    {
        gnutls_datum_t m, e, d, p, q, u, e1, e2;
        if ((ret = pgnutls_privkey_export_rsa_raw( key_data(key_orig)->a.privkey, &m, &e, &d, &p, &q, &u, &e1, &e2 )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_privkey_import_rsa_raw( privkey, &m, &e, &d, &p, &q, &u, &e1, &e2 );
        free( m.data ); free( e.data ); free( d.data ); free( p.data ); free( q.data ); free( u.data );
        free( e1.data ); free( e2.data );
        if (ret)
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        break;
    }
    case ALG_ID_DSA:
    {
        gnutls_datum_t p, q, g, y, x;
        if ((ret = pgnutls_privkey_export_dsa_raw( key_data(key_orig)->a.privkey, &p, &q, &g, &y, &x )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_privkey_import_dsa_raw( privkey, &p, &q, &g, &y, &x );
        free( p.data ); free( q.data ); free( g.data ); free( y.data ); free( x.data );
        if (ret)
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        key_copy->u.a.dss_seed = key_orig->u.a.dss_seed;
        break;
    }
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    {
        gnutls_ecc_curve_t curve;
        gnutls_datum_t x, y, k;
        if ((ret = pgnutls_privkey_export_ecc_raw( key_data(key_orig)->a.privkey, &curve, &x, &y, &k )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_privkey_import_ecc_raw( privkey, curve, &x, &y, &k );
        free( x.data ); free( y.data ); free( k.data );
        if (ret)
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        break;
    }

    default:
        ERR( "unhandled algorithm %u\n", key_orig->alg_id );
        return STATUS_INTERNAL_ERROR;
    }

    key_data(key_copy)->a.privkey = privkey;
    return STATUS_SUCCESS;
}

static NTSTATUS dup_pubkey( struct key *key_orig, struct key *key_copy )
{
    gnutls_pubkey_t pubkey;
    int ret;

    if ((ret = pgnutls_pubkey_init( &pubkey )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    switch (key_orig->alg_id)
    {
    case ALG_ID_RSA:
    case ALG_ID_RSA_SIGN:
    {
        gnutls_datum_t m, e;
        if ((ret = pgnutls_pubkey_export_rsa_raw( key_data(key_orig)->a.pubkey, &m, &e )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_pubkey_import_rsa_raw( pubkey, &m, &e );
        free( m.data ); free( e.data );
        if (ret)
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        break;
    }
    case ALG_ID_DSA:
    {
        gnutls_datum_t p, q, g, y;
        if ((ret = pgnutls_pubkey_export_dsa_raw( key_data(key_orig)->a.pubkey, &p, &q, &g, &y )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_pubkey_import_dsa_raw( pubkey, &p, &q, &g, &y );
        free( p.data ); free( q.data ); free( g.data ); free( y.data );
        if (ret)
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        key_copy->u.a.dss_seed = key_orig->u.a.dss_seed;
        break;
    }
    case ALG_ID_ECDH_P256:
    case ALG_ID_ECDH_P384:
    case ALG_ID_ECDSA_P256:
    case ALG_ID_ECDSA_P384:
    {
        gnutls_ecc_curve_t curve;
        gnutls_datum_t x, y;
        if ((ret = pgnutls_pubkey_export_ecc_raw( key_data(key_orig)->a.pubkey, &curve, &x, &y )))
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        ret = pgnutls_pubkey_import_ecc_raw( pubkey, curve, &x, &y );
        free( x.data ); free( y.data );
        if (ret)
        {
            pgnutls_perror( ret );
            return STATUS_INTERNAL_ERROR;
        }
        break;
    }
    default:
        ERR( "unhandled algorithm %u\n", key_orig->alg_id );
        return STATUS_INTERNAL_ERROR;
    }

    key_data(key_copy)->a.pubkey = pubkey;
    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_duplicate( void *args )
{
    const struct key_asymmetric_duplicate_params *params = args;
    NTSTATUS status;

    if (params->key_orig->alg_id == ALG_ID_DH)
    {
        union key_data *s = key_data( params->key_orig );
        union key_data *d = key_data( params->key_copy );
        if (s->d.privkey)
        {
            dh_key_alloc( params->key_copy );
            memcpy( d->d.privkey, s->d.privkey, params->key_orig->u.a.bitlen / 8 );
            memcpy( d->d.pubkey, s->d.pubkey, dh_pubkey_len( params->key_orig ));
        }
        return STATUS_SUCCESS;
    }

    if (key_data(params->key_orig)->a.privkey && (status = dup_privkey( params->key_orig, params->key_copy )))
        return status;

    if (key_data(params->key_orig)->a.pubkey && (status = dup_pubkey( params->key_orig, params->key_copy )))
        return status;

    return STATUS_SUCCESS;
}

static NTSTATUS key_asymmetric_decrypt( void *args )
{
    const struct key_asymmetric_decrypt_params *params = args;
    gnutls_datum_t e, d = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    int ret;

    e.data = params->input;
    e.size = params->input_len;
    if ((ret = pgnutls_privkey_decrypt_data( key_data(params->key)->a.privkey, 0, &e, &d )))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    *params->ret_len = d.size;
    if (params->output_len >= d.size) memcpy( params->output, d.data, *params->ret_len );
    else status = STATUS_BUFFER_TOO_SMALL;

    free( d.data );
    return status;
}

#if defined(HAVE_GCRYPT_H) && defined(SONAME_LIBGCRYPT)
const char * gcrypt_hash_algorithm_name(LPCWSTR alg_id)
{
    if (!wcscmp( alg_id, BCRYPT_SHA1_ALGORITHM ))   return "sha1";
    if (!wcscmp( alg_id, BCRYPT_SHA256_ALGORITHM )) return "sha256";
    if (!wcscmp( alg_id, BCRYPT_SHA384_ALGORITHM )) return "sha384";
    if (!wcscmp( alg_id, BCRYPT_SHA512_ALGORITHM )) return "sha512";
    if (!wcscmp( alg_id, BCRYPT_MD2_ALGORITHM ))    return "md2";
    if (!wcscmp( alg_id, BCRYPT_MD5_ALGORITHM ))    return "md5";
    return NULL;
}

static NTSTATUS key_asymmetric_encrypt_gcrypt( void *args )
{
    const struct key_asymmetric_encrypt_params *params = args;
    struct key *key = params->key;
    UCHAR *input = params->input;
    ULONG input_len = params->input_len;
    UCHAR *output = params->output;
    ULONG *ret_len = params->ret_len;
    void *padding = params->padding;
    ULONG flags = params->flags;
    BCRYPT_OAEP_PADDING_INFO *oaep_info = padding;
    NTSTATUS status;
    gcry_sexp_t sexp_pubkey = NULL;
    gcry_sexp_t sexp_result = NULL;
    gcry_sexp_t sexp_input = NULL;
    BCRYPT_RSAKEY_BLOB *rsa_blob;
    gcry_sexp_t mpi_a = NULL;
    const void *result;
    size_t result_len;
    gcry_error_t err;
    ULONG len;

    if (!gcrypt_available)
    {
        ERR("Asymmetric encryption not available.\n");
        return STATUS_INTERNAL_ERROR;
    }

    if (key->alg_id != ALG_ID_RSA)
    {
        FIXME("Unsupported algorithm id: %u\n", key->alg_id);
        return STATUS_INTERNAL_ERROR;
    }

    if (flags == BCRYPT_PAD_NONE && input_len != key->u.a.bitlen / 8)
    {
        WARN( "Invalid input_len %u for BCRYPT_PAD_NONE.\n", (int)input_len );
        return STATUS_INVALID_PARAMETER;
    }

    /* import RSA key */
    if ((status = key_export_rsa_public( key, NULL, 0, &len )))
    {
        ERR( "Key export failed.\n" );
        return status;
    }
    rsa_blob = malloc( len );
    if ((status = key_export_rsa_public( key, (UCHAR *)rsa_blob, len, &len )))
    {
        ERR( "Key export failed.\n" );
        return status;
    }
    err = pgcry_sexp_build(&sexp_pubkey, NULL,
                        "(public-key(rsa (e %b)(n %b)))",
                        rsa_blob->cbPublicExp,
                        (UCHAR *)(rsa_blob + 1),
                        rsa_blob->cbModulus,
                        (UCHAR *)(rsa_blob + 1) + rsa_blob->cbPublicExp);
    free( rsa_blob );
    if (err)
    {
        ERR("Failed to build gcrypt public key\n");
        goto done;
    }

    /* import input data with necessary padding */
    if (flags == BCRYPT_PAD_PKCS1)
    {
        err = pgcry_sexp_build(&sexp_input, NULL,
                            "(data(flags pkcs1)(value %b))",
                            input_len,
                            input);
    }
    else if (flags == BCRYPT_PAD_OAEP)
    {
        if (oaep_info->pbLabel)
            err = pgcry_sexp_build(&sexp_input, NULL,
                                "(data(flags oaep)(hash-algo %s)(label %b)(value %b))",
                                gcrypt_hash_algorithm_name(oaep_info->pszAlgId),
                                oaep_info->cbLabel,
                                oaep_info->pbLabel,
                                input_len,
                                input);
        else
            err = pgcry_sexp_build(&sexp_input, NULL,
                                "(data(flags oaep)(hash-algo %s)(value %b))",
                                gcrypt_hash_algorithm_name(oaep_info->pszAlgId),
                                input_len,
                                input);
    }
    else if (flags == BCRYPT_PAD_NONE)
    {
        err = pgcry_sexp_build(&sexp_input, NULL,
                            "(data(flags raw)(value %b))",
                            input_len,
                            input);
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }

    if (err)
    {
        ERR("Failed to build gcrypt padded input data\n");
        goto done;
    }

    if ((err = pgcry_pk_encrypt(&sexp_result, sexp_input, sexp_pubkey)))
    {
        ERR("Failed to encrypt data\n");
        goto done;
    }

    mpi_a = pgcry_sexp_find_token(sexp_result, "a", 0);
    result = pgcry_sexp_nth_data(mpi_a, 1, &result_len);

    *ret_len = result_len;

    if (params->output_len >= result_len) memcpy(output, result, result_len);
    else if (params->output_len == 0) status = STATUS_SUCCESS;
    else status = STATUS_BUFFER_TOO_SMALL;

done:
    pgcry_sexp_release(sexp_input);
    pgcry_sexp_release(sexp_pubkey);
    pgcry_sexp_release(sexp_result);
    pgcry_sexp_release(mpi_a);

    if (status)
        return status;

    if (err)
    {
        ERR("Error = %s/%s\n", pgcry_strsource (err), pgcry_strerror (err));
        return STATUS_INTERNAL_ERROR;
    }

    return STATUS_SUCCESS;
}
#else
static NTSTATUS key_asymmetric_encrypt_gcrypt( void *args )
{
    ERR("Asymmetric key encryption not supported without gcrypt.\n");
    return STATUS_NOT_IMPLEMENTED;
}
#endif

static NTSTATUS key_asymmetric_encrypt( void *args )
{
    const struct key_asymmetric_encrypt_params *params = args;
    gnutls_datum_t d, e = { 0 };
    NTSTATUS status = STATUS_SUCCESS;
    int ret;

    if (!key_data(params->key)->a.pubkey) return STATUS_INVALID_HANDLE;

    if (params->flags == BCRYPT_PAD_NONE || params->flags == BCRYPT_PAD_OAEP)
        return key_asymmetric_encrypt_gcrypt( args );

    d.data = params->input;
    d.size = params->input_len;
    if ((ret = pgnutls_pubkey_encrypt_data(key_data(params->key)->a.pubkey, 0, &d, &e)))
    {
        pgnutls_perror( ret );
        return STATUS_INTERNAL_ERROR;
    }

    *params->ret_len = e.size;
    if (params->output_len >= e.size) memcpy( params->output, e.data, *params->ret_len );
    else if (params->output_len == 0) status = STATUS_SUCCESS;
    else status = STATUS_BUFFER_TOO_SMALL;

    free( e.data );
    return status;
}

#if defined(HAVE_GCRYPT_H) && defined(SONAME_LIBGCRYPT)
static NTSTATUS gcrypt_extract_result_into_secret(gcry_sexp_t result, struct secret *secret)
{
    NTSTATUS status = STATUS_SUCCESS;
    gcry_mpi_t fullcoords = NULL;
    gcry_sexp_t fragment = NULL;
    UCHAR *tmp_buffer = NULL;
    gcry_error_t err;
    size_t size;

    fragment = pgcry_sexp_find_token( result, "s", 0 );
    if (!fragment)
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    fullcoords = pgcry_sexp_nth_mpi( fragment, 1, GCRYMPI_FMT_USG );
    if (!fullcoords)
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    if ((err = pgcry_mpi_print( GCRYMPI_FMT_USG, NULL, 0, &size, fullcoords)) )
    {
        ERR("Error = %s/%s.\n", pgcry_strsource( err ), pgcry_strerror( err ));
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }

    tmp_buffer = malloc(size);
    if ((err = pgcry_mpi_print( GCRYMPI_FMT_STD, tmp_buffer, size, NULL, fullcoords)) )
    {
        ERR( "Error = %s/%s.\n", pgcry_strsource(err), pgcry_strerror(err) );
        status = STATUS_INTERNAL_ERROR;
        goto done;
    }

    memcpy( secret->data, tmp_buffer + size % 2, size / 2 );
    secret->data_len = size / 2;

done:
    free( tmp_buffer );

    pgcry_mpi_release( fullcoords );
    pgcry_sexp_release( fragment );

    return status;
}
#endif


static NTSTATUS key_secret_agreement( void *args )
{
    struct key_secret_agreement_params *params = args;
    struct secret *secret;
    struct key *priv_key;
    struct key *peer_key;

    priv_key = params->privkey;
    peer_key = params->pubkey;
    secret = params->secret;

    switch (priv_key->alg_id)
    {
        case ALG_ID_DH:
#if defined(HAVE_GMP_H) && defined(SONAME_LIBGMP)
        {
            mpz_t p, priv, peer, k;
            ULONG key_length;

            if (!dh_supported)
            {
                ERR("DH is not available.\n");
                return STATUS_NOT_IMPLEMENTED;
            }

            key_length = priv_key->u.a.bitlen / 8;

            if (memcmp((BCRYPT_DH_KEY_BLOB *)key_data(priv_key)->d.pubkey + 1,
                    key_data(peer_key)->d.pubkey + sizeof(BCRYPT_DH_KEY_BLOB), key_length * 2))
            {
                ERR("peer DH paramaters do not match.\n");
                return STATUS_INTERNAL_ERROR;
            }

            pmpz_init(p);
            pmpz_init(priv);
            pmpz_init(peer);
            pmpz_init(k);

            import_mpz(p, (BCRYPT_DH_KEY_BLOB *)key_data(priv_key)->d.pubkey + 1, key_length);
            if (pmpz_sizeinbase(p, 2) < 2)
            {
                ERR("Invalid prime.\n");
                pmpz_clear(p);
                pmpz_clear(priv);
                pmpz_clear(peer);
                pmpz_clear(k);
                return STATUS_INTERNAL_ERROR;
            }
            import_mpz(priv, key_data(priv_key)->d.privkey, key_length);
            import_mpz(peer, key_data(peer_key)->d.pubkey + sizeof(BCRYPT_DH_KEY_BLOB) + key_length * 2, key_length);
            pmpz_powm(k, peer, priv, p);
            export_mpz(secret->data, key_length, k);
            secret->data_len = key_length;

            pmpz_clear(p);
            pmpz_clear(priv);
            pmpz_clear(peer);
            pmpz_clear(k);
            break;
        }
#else
            ERR_(winediag)("Compiled without DH support.\n");
            return STATUS_NOT_IMPLEMENTED;
#endif

        case ALG_ID_ECDH_P256:
        case ALG_ID_ECDH_P384:
/* this is necessary since GNUTLS doesn't support ECDH public key encryption, maybe we can replace this when it does:
   https://github.com/gnutls/gnutls/blob/cdc4fc288d87f91f974aa23b6e8595a53970ce00/lib/nettle/pk.c#L495 */
#if defined(HAVE_GCRYPT_H) && defined(SONAME_LIBGCRYPT)
        {
            gcry_sexp_t xchg_result = NULL;
            gcry_sexp_t privkey = NULL;
            gcry_sexp_t pubkey = NULL;
            const char *pubkey_format;
            BCRYPT_ECCKEY_BLOB *h;
            UCHAR *privkey_blob;
            UCHAR *pubkey_raw;
            gcry_error_t err;
            ULONG key_length;
            NTSTATUS status;
            ULONG key_len;

            if (!gcrypt_available)
            {
                ERR("ECDH secret agreement is not available.\n");
                return STATUS_NOT_IMPLEMENTED;
            }

            if (priv_key->alg_id == ALG_ID_ECDH_P256)
            {
                pubkey_format = "NIST P-256";
                key_length = 32;
            }
            else if (priv_key->alg_id == ALG_ID_ECDH_P384)
            {
                pubkey_format = "NIST P-384";
                key_length = 48;
            }
            else return STATUS_NOT_IMPLEMENTED;

            if (key_length != priv_key->u.a.bitlen / 8)
            {
                ERR( "Key length mismatch, key->u.a.bitlen %u, key_length %u.\n", (int)priv_key->u.a.bitlen,
                     (int)key_length );
                return STATUS_INVALID_PARAMETER;
            }

            if ((status = key_export_ecc( priv_key, NULL, 0, &key_len )))
                return status;
            privkey_blob = malloc( key_len );
            if ((status = key_export_ecc( priv_key, privkey_blob, key_len, &key_len )))
            {
                free( privkey_blob );
                return status;
            }

            if ((status = key_export_ecc_public( peer_key, NULL, 0, &key_len )))
                return status;
            h = malloc( key_len );
            if ((status = key_export_ecc_public( peer_key, (UCHAR *)h, key_len, &key_len )))
            {
                free( privkey_blob );
                return status;
            }

            /* copy public key into temporary buffer so we can prepend 0x04 (to indicate it is uncompressed) */
            pubkey_raw = malloc( (key_length * 2) + 1 );
            pubkey_raw[0] = 0x04;
            memcpy( pubkey_raw + 1, h + 1, key_length * 2 );
            free( h );

            err = pgcry_sexp_build( &pubkey, NULL, "(key-data(public-key(ecdh(curve %s)(q %b))))", pubkey_format,
                                   (key_length * 2) + 1, pubkey_raw );
            free( pubkey_raw );
            if (err)
            {
                free( privkey_blob );
                ERR( "Failed to build gcrypt public key. err %s/%s\n", pgcry_strsource( err ), pgcry_strerror( err ));
                return STATUS_INTERNAL_ERROR;
            }

            err = pgcry_sexp_build( &privkey, NULL, "(data(flags raw)(value %b))", key_length,
                                   privkey_blob + sizeof(BCRYPT_ECCKEY_BLOB) + key_length * 2 );
            free( privkey_blob );
            if (err)
            {
                pgcry_sexp_release( pubkey );
                return STATUS_INTERNAL_ERROR;
            }
            err = pgcry_pk_encrypt( &xchg_result, privkey, pubkey );
            pgcry_sexp_release( privkey );
            pgcry_sexp_release( pubkey );
            if (err)
            {
                ERR( "Failed to perform key exchange. err %s/%s\n", pgcry_strsource( err ), pgcry_strerror( err ));
                return STATUS_INTERNAL_ERROR;
            }
            status = gcrypt_extract_result_into_secret( xchg_result, secret );
            pgcry_sexp_release(xchg_result);
            if (status)
            {
                ERR("Failed to extract secret key.\n");
                return status;
            }
            break;
        }
#else
            WARN("Compiled without ECC secret support.\n");
            return STATUS_NOT_IMPLEMENTED;
#endif

        default:
            ERR( "unhandled algorithm %u\n", priv_key->alg_id );
            return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    gnutls_process_attach,
    gnutls_process_detach,
    key_symmetric_vector_reset,
    key_symmetric_set_auth_data,
    key_symmetric_encrypt,
    key_symmetric_decrypt,
    key_symmetric_get_tag,
    key_symmetric_destroy,
    key_asymmetric_generate,
    key_asymmetric_decrypt,
    key_asymmetric_encrypt,
    key_asymmetric_duplicate,
    key_asymmetric_sign,
    key_asymmetric_verify,
    key_asymmetric_destroy,
    key_asymmetric_export,
    key_asymmetric_import,
    key_secret_agreement,
};

#ifdef _WIN64

typedef ULONG PTR32;

struct key_symmetric32
{
    enum chain_mode mode;
    ULONG           block_size;
    PTR32           vector;
    ULONG           vector_len;
    PTR32           secret;
    ULONG           secret_len;
    ULONG           __cs[6];
};

struct key_asymmetric32
{
    ULONG             bitlen;     /* ignored for ECC keys */
    ULONG             flags;
    DSSSEED           dss_seed;
};

struct key32
{
    struct object hdr;
    enum alg_id   alg_id;
    UINT64        private[2];  /* private data for backend */
    union
    {
        struct key_symmetric32 s;
        struct key_asymmetric32 a;
    } u;
};

union padding
{
    BCRYPT_PKCS1_PADDING_INFO pkcs1;
    BCRYPT_PSS_PADDING_INFO pss;
};

union padding32
{
    struct
    {
        PTR32 pszAlgId;
    } pkcs1;
    struct
    {
        PTR32 pszAlgId;
        ULONG cbSalt;
    } pss;
};

static union padding *get_padding( union padding32 *padding32, union padding *padding, ULONG flags)
{
    if (!padding32) return NULL;

    switch (flags)
    {
    case BCRYPT_PAD_PKCS1:
        padding->pkcs1.pszAlgId = ULongToPtr( padding32->pkcs1.pszAlgId );
        return padding;
    case BCRYPT_PAD_PSS:
        padding->pss.pszAlgId = ULongToPtr( padding32->pss.pszAlgId );
        padding->pss.cbSalt = padding32->pss.cbSalt;
        return padding;
    default:
        break;
    }
    return NULL;
}

static struct key *get_symmetric_key( struct key32 *key32, struct key *key )
{
    key->hdr            = key32->hdr;
    key->alg_id         = key32->alg_id;
    key->private[0]     = key32->private[0];
    key->private[1]     = key32->private[1];
    key->u.s.mode       = key32->u.s.mode;
    key->u.s.block_size = key32->u.s.block_size;
    key->u.s.vector     = ULongToPtr(key32->u.s.vector);
    key->u.s.vector_len = key32->u.s.vector_len;
    key->u.s.secret     = ULongToPtr(key32->u.s.secret);
    key->u.s.secret_len = key32->u.s.secret_len;
    return key;
}

static struct key *get_asymmetric_key( struct key32 *key32, struct key *key )
{
    key->hdr            = key32->hdr;
    key->alg_id         = key32->alg_id;
    key->private[0]     = key32->private[0];
    key->private[1]     = key32->private[1];
    key->u.a.bitlen     = key32->u.a.bitlen;
    key->u.a.flags      = key32->u.a.flags;
    key->u.a.dss_seed   = key32->u.a.dss_seed;
    return key;
}

static void put_symmetric_key32( struct key *key, struct key32 *key32 )
{
    key32->private[0]     = key->private[0];
    key32->private[1]     = key->private[1];
}

static void put_asymmetric_key32( struct key *key, struct key32 *key32 )
{
    key32->private[0]     = key->private[0];
    key32->private[1]     = key->private[1];
    key32->u.a.flags      = key->u.a.flags;
    key32->u.a.dss_seed   = key->u.a.dss_seed;
}

static NTSTATUS wow64_key_symmetric_vector_reset( void *args )
{
    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = args;

    ret = key_symmetric_vector_reset( get_symmetric_key( key32, &key ));
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_set_auth_data( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 auth_data;
        ULONG len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_symmetric_set_auth_data_params params =
    {
        get_symmetric_key( key32, &key ),
        ULongToPtr(params32->auth_data),
        params32->len
    };

    ret = key_symmetric_set_auth_data( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_encrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_symmetric_encrypt_params params =
    {
        get_symmetric_key( key32, &key ),
        ULongToPtr(params32->input),
        params32->input_len,
        ULongToPtr(params32->output),
        params32->output_len
    };

    ret = key_symmetric_encrypt( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_decrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_symmetric_decrypt_params params =
    {
        get_symmetric_key( key32, &key ),
        ULongToPtr(params32->input),
        params32->input_len,
        ULongToPtr(params32->output),
        params32->output_len
    };

    ret = key_symmetric_decrypt( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_get_tag( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 tag;
        ULONG len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_symmetric_get_tag_params params =
    {
        get_symmetric_key( key32, &key ),
        ULongToPtr(params32->tag),
        params32->len
    };

    ret = key_symmetric_get_tag( &params );
    put_symmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_symmetric_destroy( void *args )
{
    struct key32 *key32 = args;
    struct key key;

    return key_symmetric_destroy( get_symmetric_key( key32, &key ));
}

static NTSTATUS wow64_key_asymmetric_generate( void *args )
{
    struct key32 *key32 = args;
    struct key key;
    NTSTATUS ret;

    ret = key_asymmetric_generate( get_asymmetric_key( key32, &key ));
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_decrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_asymmetric_decrypt_params params =
    {
        get_asymmetric_key( key32, &key ),
        ULongToPtr(params32->input),
        params32->input_len,
        ULongToPtr(params32->output),
        params32->output_len,
        ULongToPtr(params32->ret_len)
    };

    ret = key_asymmetric_decrypt( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_encrypt( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_asymmetric_encrypt_params params =
    {
        get_asymmetric_key( key32, &key ),
        ULongToPtr(params32->input),
        params32->input_len,
        ULongToPtr(params32->output),
        params32->output_len,
        ULongToPtr(params32->ret_len)
    };

    ret = key_asymmetric_encrypt( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_duplicate( void *args )
{
    struct
    {
        PTR32 key_orig;
        PTR32 key_copy;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key_orig, key_copy;
    struct key32 *key_orig32 = ULongToPtr( params32->key_orig );
    struct key32 *key_copy32 = ULongToPtr( params32->key_copy );
    struct key_asymmetric_duplicate_params params =
    {
        get_asymmetric_key( key_orig32, &key_orig ),
        get_asymmetric_key( key_copy32, &key_copy )
    };

    ret = key_asymmetric_duplicate( &params );
    put_asymmetric_key32( &key_copy, key_copy32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_sign( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 padding;
        PTR32 input;
        ULONG input_len;
        PTR32 output;
        ULONG output_len;
        PTR32 ret_len;
        ULONG flags;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    union padding padding;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_asymmetric_sign_params params =
    {
        get_asymmetric_key( key32, &key ),
        get_padding(ULongToPtr( params32->padding ), &padding, params32->flags),
        ULongToPtr(params32->input),
        params32->input_len,
        ULongToPtr(params32->output),
        params32->output_len,
        ULongToPtr(params32->ret_len),
        params32->flags
    };

    ret = key_asymmetric_sign( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_verify( void *args )
{
    struct
    {
        PTR32 key;
        PTR32 padding;
        PTR32 hash;
        ULONG hash_len;
        PTR32 signature;
        ULONG signature_len;
        ULONG flags;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    union padding padding;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_asymmetric_verify_params params =
    {
        get_asymmetric_key( key32, &key ),
        get_padding(ULongToPtr( params32->padding ), &padding, params32->flags),
        ULongToPtr(params32->hash),
        params32->hash_len,
        ULongToPtr(params32->signature),
        params32->signature_len,
        params32->flags
    };

    ret = key_asymmetric_verify( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_destroy( void *args )
{
    struct key32 *key32 = args;
    struct key key;

    return key_asymmetric_destroy( get_asymmetric_key( key32, &key ));
}

static NTSTATUS wow64_key_asymmetric_export( void *args )
{
    struct
    {
        PTR32 key;
        ULONG flags;
        PTR32 buf;
        ULONG len;
        PTR32 ret_len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_asymmetric_export_params params =
    {
        get_asymmetric_key( key32, &key ),
        params32->flags,
        ULongToPtr(params32->buf),
        params32->len,
        ULongToPtr(params32->ret_len),
    };

    ret = key_asymmetric_export( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

static NTSTATUS wow64_key_asymmetric_import( void *args )
{
    struct
    {
        PTR32 key;
        ULONG flags;
        PTR32 buf;
        ULONG len;
    } const *params32 = args;

    NTSTATUS ret;
    struct key key;
    struct key32 *key32 = ULongToPtr( params32->key );
    struct key_asymmetric_import_params params =
    {
        get_asymmetric_key( key32, &key ),
        params32->flags,
        ULongToPtr(params32->buf),
        params32->len
    };

    ret = key_asymmetric_import( &params );
    put_asymmetric_key32( &key, key32 );
    return ret;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    gnutls_process_attach,
    gnutls_process_detach,
    wow64_key_symmetric_vector_reset,
    wow64_key_symmetric_set_auth_data,
    wow64_key_symmetric_encrypt,
    wow64_key_symmetric_decrypt,
    wow64_key_symmetric_get_tag,
    wow64_key_symmetric_destroy,
    wow64_key_asymmetric_generate,
    wow64_key_asymmetric_decrypt,
    wow64_key_asymmetric_encrypt,
    wow64_key_asymmetric_duplicate,
    wow64_key_asymmetric_sign,
    wow64_key_asymmetric_verify,
    wow64_key_asymmetric_destroy,
    wow64_key_asymmetric_export,
    wow64_key_asymmetric_import
};

#endif  /* _WIN64 */

#endif /* HAVE_GNUTLS_CIPHER_INIT */