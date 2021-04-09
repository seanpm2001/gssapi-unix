/*
 * Copyright © 2021 VMware, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <shadow.h>
#include <errno.h>

/* Needed for SLES11, not sure about other UNIX platforms */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <crypt.h>
#include <stdio.h>
#include "srp.h"
#include <dlfcn.h>

#define CRYPT_MD5         "$1$"
#define CRYPT_BLOWFISH_2A "$2a$"
#define CRYPT_BLOWFISH_2B "$2b$"
#define CRYPT_BLOWFISH_2X "$2x$"
#define CRYPT_BLOWFISH_2Y "$2y$"
#define CRYPT_SHA_256     "$5$"
#define CRYPT_SHA_512     "$6$"

static SRP_HashAlgorithm G_alg     = SRP_SHA1;
static SRP_NGType        G_ng_type = SRP_NG_2048;
static const char        *G_n_hex  = 0;
static const char        *G_g_hex  = 0;

/*
 * This function looks up "username" in the shadow password file, determines
 * the hash algorithm type, and returns the salt and the password
 * hash for that user.
 *
 * Given the salt and the user password, then the hash can be created.
 * The generated hash is used as an SRP password (client side), and
 * the generator for the SRP secret (server side).
 *
 * Crypt password file format references:
 * http://php.net/manual/en/function.crypt.php
 * http://en.wikipedia.org/wiki/Crypt_%28C%29#Blowfish-based_scheme
 *
 * Look up from the shadow password file the specified user, and if found,
 * return the salt field parsed out from the hash entry
 *
 * Algorithm ID
 * $1$  MD5
 * 12 characters salt follows
 *
 * $2a$ Blowfish
 * $2b$ Blowfish
 * $2x$ Blowfish
 * $2y$ Blowfish
 * Blowfish salt format:
 * $id$NN$-----22 chars-salt----++++++hash+++++:
 *
 * SHA salt format
 * $5$  SHA-256
 * $6$  SHA-512
 * $ID$salt$hash
 */
int get_sp_salt(const char *username,
                char **ret_salt,
                char **ret_encpwd)
{
    int st = 0;
    int is_locked = 0;
    struct spwd *spval = NULL;
    struct spwd spval_buf = {0};
    char *spbuf_str = NULL;
    int spbuf_str_len = 256;
    int salt_len = 0;
    char *salt = NULL;
    char *encpwd = NULL;
    char *sp = NULL;
    int cur_uid = 0;
    int error = 0;
    
    if (!username || !ret_salt || !ret_encpwd)
    {
        st = -1;
        errno = EINVAL;
        goto error;
    }

    /* Must be root to read shadow password file */
    cur_uid = getuid();
    error = seteuid(0);
    if (error != 0)
    {
        st = -1;
        goto error;
    }

    /* Obtain password file lock, and hold minimum amount of time */
    st = lckpwdf();
    if (st == -1)
    {
        goto error;
    }
    is_locked = 1;

    spbuf_str = calloc(spbuf_str_len, sizeof(char));
    if (!spbuf_str)
    {
        st = -1;
        goto error;
    }
    st = getspnam_r(username,
                    &spval_buf,
                    spbuf_str,
                    spbuf_str_len,
                    &spval);

    if (!spval || st == -1)
    {
        /* Failed due to permissions or entry not found */
        st = -1;
        goto error;
    }
    salt = strdup(spval->sp_pwdp);
    if (!salt)
    {
        /* errno is set */
        st = -1;
        goto error;
    }
    encpwd = strdup(spval->sp_pwdp);
    if (!encpwd)
    {
        /* errno is set */
        st = -1;
        goto error;
    }
    ulckpwdf();
    error = seteuid(cur_uid);
    if (error != 0)
    {
        st = -1;
        goto error;
    }

    is_locked = 0;

    /* CRYPT_DES hash is not supported; how to test? */

    /* Determine the hash algorithn, and therefore the salt length */
    if (!strncmp(salt, CRYPT_MD5, strlen(CRYPT_MD5)))
    {
        /* $1$123456789012 */
        salt_len = 12 + 3;
    }
    else if (!strncmp(salt, CRYPT_BLOWFISH_2A, strlen(CRYPT_BLOWFISH_2A)) ||
             !strncmp(salt, CRYPT_BLOWFISH_2B, strlen(CRYPT_BLOWFISH_2B)) ||
             !strncmp(salt, CRYPT_BLOWFISH_2X, strlen(CRYPT_BLOWFISH_2X)) ||
             !strncmp(salt, CRYPT_BLOWFISH_2Y, strlen(CRYPT_BLOWFISH_2Y)))
    {
        /* $2a$05$1234567890123456789012 */
        salt_len = 22 + 7;
    }
    else if (!strncmp(salt, CRYPT_SHA_256, strlen(CRYPT_SHA_256)) ||
             !strncmp(salt, CRYPT_SHA_512, strlen(CRYPT_SHA_512)))
    {
        sp = strrchr(salt, '$');
        salt_len = sp - salt + 1;
    }
    if(salt_len == 0)//locked user, user with nologin etc
    {
        st = -1;
        errno = EPERM;
        goto error;
    }
    salt[salt_len] = '\0';
    *ret_salt = salt;
    *ret_encpwd = encpwd;
    salt = NULL;

error:
    if (is_locked)
    {
        ulckpwdf();
        error = seteuid(cur_uid);
    }
    if (spbuf_str)
    {
        free(spbuf_str);
    }
    if (st == -1)
    {
        if (salt)
        {
            free(salt);
            salt = NULL;
        }
        if (encpwd)
        {
            free(encpwd);
            encpwd = NULL;
        }
    }
    return st;
}

/* Create the temporary SRP secret using username shadow pwd entry */
static int
_srpVerifierInit(
    char *username,
    char *password,
    unsigned char **ret_bytes_s,
    int *ret_len_s,
    unsigned char **ret_bytes_v,
    int *ret_len_v)
{
    int sts = 0;
    const unsigned char *bytes_s = NULL;
    int len_s = 0;
    const unsigned char *bytes_v = NULL;
    int len_v = 0;

    if (!username ||
        !password ||
        !ret_bytes_s ||
        !ret_len_s ||
        !ret_bytes_v ||
        !ret_len_v)
    {
        sts = -1;
        goto error;
    }

    srp_create_salted_verification_key(
        G_alg,
        G_ng_type,
        username,
        (const unsigned char *) password,
        (int) strlen(password),
        &bytes_s,
        &len_s,
        &bytes_v,
        &len_v,
        G_n_hex,
        G_g_hex);

    *ret_bytes_s = (unsigned char *) bytes_s;
    *ret_len_s   = len_s;

    *ret_bytes_v = (unsigned char *) bytes_v;
    *ret_len_v = len_v;

error:
    return sts;
}

int
get_salt_and_v_value(
    int plugin_type,
    const char *username,
    char **ret_salt,
    unsigned char **ret_bytes_s,
    int *ret_len_s,
    unsigned char **ret_bytes_v,
    int *ret_len_v
    )
{
    int sts = 0;
    char *user_salt = NULL;
    char *encpwd = NULL;
    unsigned char *bytes_s = NULL;
    int len_s = 0;
    unsigned char *bytes_v = NULL;
    int len_v = 0;

    if(!username ||
       !ret_salt ||
       !ret_bytes_s ||
       !ret_len_s ||
       !ret_bytes_v ||
       !ret_len_v)
    {
        sts = EINVAL;
        goto error;
    }

    //Check if we have privileges
    if(getuid() != 0)
    {
        sts = EPERM;
        goto error;
    }

    sts = get_sp_salt(username, &user_salt, &encpwd);
    if(sts)
    {
        goto error;
    }

    /*
     * This call creates the temporary server-side SRP secret
     *
     * bytes_s: SRP salt, publically known to client/server
     * bytes_v: SRP secret, privately known only by server
     */
    sts = _srpVerifierInit(
              (char *)username,
              encpwd,
              &bytes_s,
              &len_s,
              &bytes_v,
              &len_v);
    if (sts)
    {
        goto error;
    }

    *ret_salt = user_salt;
    *ret_bytes_s = bytes_s;
    *ret_len_s = len_s;
    *ret_bytes_v = bytes_v;
    *ret_len_v = len_v;
cleanup:
    return sts;

error:
    if(ret_salt)
    {
        *ret_salt = NULL;
    }
    free(user_salt);
    free(encpwd);
    goto cleanup;
}
