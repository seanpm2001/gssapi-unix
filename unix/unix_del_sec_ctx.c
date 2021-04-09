/*
 * Copyright © 2021 VMware, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Module: srp_del_sec_ctx.c
 * Abstract:
 *     VMware GSSAPI SRP Authentication Plugin
 *     Implements SRP delete security context
 *
 * Author: Adam Bernstein (abernstein@vmware.com)
 */

#include "gssapiP_unix.h"
#include "includes.h"
#include "srp.h"

/*
 * Cleanup SRP client-side memory. SRP server-side binding handle
 * and SRP verifier handle are cleaned up by srp_gss_accept_sec_context()
 * when the authentication exchange is complete, pass or fail.
 * Note: calling these cleanup routines here causes a hang;
 *       the reason is unknown.
 */
OM_uint32
srp_gss_delete_sec_context(
                OM_uint32 *minor_status,
                gss_ctx_id_t *context_handle,
                gss_buffer_t output_token)
{
    srp_gss_ctx_id_t srp_ctx = NULL;
    OM_uint32 tmp_minor = GSS_S_COMPLETE;
    OM_uint32 ret = GSS_S_COMPLETE;

    if (context_handle == NULL || *context_handle == NULL)
    {
        return (GSS_S_FAILURE);
    }

    srp_ctx = (srp_gss_ctx_id_t) *context_handle;

    if (srp_ctx->upn_name)
    {
        free(srp_ctx->upn_name);
    }

    if (srp_ctx->unix_username)
    {
        free(srp_ctx->unix_username);
    }

    if (srp_ctx->srp_session_key)
    {
        free(srp_ctx->srp_session_key);
    }

    if (srp_ctx->srp_usr)
    {
        srp_user_delete(srp_ctx->srp_usr);
        srp_ctx->srp_usr = NULL;
    }

    if (srp_ctx->srp_ver)
    {
        srp_verifier_delete(srp_ctx->srp_ver);
        srp_ctx->srp_ver = NULL;
    }

    if (srp_ctx->mech)
    {
        gss_release_oid(&tmp_minor, &srp_ctx->mech);
    }

    if (srp_ctx->krb5_ctx)
    {
        if (srp_ctx->keyblock)
        {
            krb5_free_keyblock_contents(srp_ctx->krb5_ctx, srp_ctx->keyblock);
            free(srp_ctx->keyblock);
            srp_ctx->keyblock = NULL;
        }

        krb5_free_context(srp_ctx->krb5_ctx);
        srp_ctx->krb5_ctx = NULL;
    }

    HMAC_CTX_cleanup(&srp_ctx->hmac_ctx);
#ifdef SRP_FIPS_ENABLED
    if (srp_ctx->evp_encrypt_ctx)
    {
        EVP_CIPHER_CTX_free(srp_ctx->evp_encrypt_ctx);
    }
    if (srp_ctx->evp_decrypt_ctx)
    {
        EVP_CIPHER_CTX_free(srp_ctx->evp_decrypt_ctx);
    }
#endif

    if (srp_ctx->bytes_v)
    {
        free(srp_ctx->bytes_v);
    }
    if (srp_ctx->bytes_s)
    {
        free(srp_ctx->bytes_s);
    }
    free(*context_handle);
    *context_handle = NULL;
    return (ret);
}
