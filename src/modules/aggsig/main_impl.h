/**********************************************************************
 * Copyright (c) 2017 Andrew Poelstra, Pieter Wuille                  *
 * Distributed under the MIT software license, see the accompanying   *
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.*
 **********************************************************************/

#ifndef _SECP256K1_MODULE_AGGSIG_MAIN_
#define _SECP256K1_MODULE_AGGSIG_MAIN_

#include "include/secp256k1.h"
#include "include/secp256k1_aggsig.h"
#include "hash.h"

enum nonce_progress {
    /* Nonce has not been generated by us or recevied from another party */
    NONCE_PROGRESS_UNKNOWN = 0,
    /* Public nonce has been recevied from another party */
    NONCE_PROGRESS_OTHER = 1,
    /* Public nonce has been generated by us but not used in signing. */
    NONCE_PROGRESS_OURS = 2,
    /* Public nonce has been generated by us and used in signing. An attempt to
     * use a nonce twice will result in an error. */
    NONCE_PROGRESS_SIGNED = 3
};

struct secp256k1_aggsig_context_struct {
    enum nonce_progress *progress;
    secp256k1_pubkey *pubkeys;
    secp256k1_scalar *secnonce;
    secp256k1_gej pubnonce_sum;
    size_t n_sigs;
    secp256k1_rfc6979_hmac_sha256 rng;
};

/* Compute sighash for a single-signer */
static int secp256k1_compute_sighash_single(const secp256k1_context *ctx, secp256k1_scalar *r, const secp256k1_pubkey *pubkey, const unsigned char *msghash32) {
    unsigned char output[32];
    unsigned char buf[33];
    size_t buflen = sizeof(buf);

    int overflow;
    secp256k1_sha256 hasher;
    secp256k1_sha256_initialize(&hasher);

    /* Encode public nonce */
    CHECK(secp256k1_ec_pubkey_serialize(ctx, buf, &buflen, pubkey, SECP256K1_EC_COMPRESSED));
    secp256k1_sha256_write(&hasher, buf, sizeof(buf));

    /* Encode message */
    secp256k1_sha256_write(&hasher, msghash32, 32);

    /* Finish */
    secp256k1_sha256_finalize(&hasher, output);
    secp256k1_scalar_set_b32(r, output, &overflow);
    return !overflow;
}

/* Compute the hash of all the data that every pubkey needs to sign */
static void secp256k1_compute_prehash(const secp256k1_context *ctx, unsigned char *output, const secp256k1_pubkey *pubkeys, size_t n_pubkeys, const secp256k1_fe *nonce_ge_x, const unsigned char *msghash32) {
    size_t i;
    unsigned char buf[33];
    size_t buflen = sizeof(buf);
    secp256k1_sha256 hasher;
    secp256k1_sha256_initialize(&hasher);
    /* Encode pubkeys */
    for (i = 0; i < n_pubkeys; i++) {
        CHECK(secp256k1_ec_pubkey_serialize(ctx, buf, &buflen, &pubkeys[i], SECP256K1_EC_COMPRESSED));
        secp256k1_sha256_write(&hasher, buf, sizeof(buf));
    }
    /* Encode nonce */
    secp256k1_fe_get_b32(buf, nonce_ge_x);
    secp256k1_sha256_write(&hasher, buf, 32);
    /* Encode message */
    secp256k1_sha256_write(&hasher, msghash32, 32);
    /* Finish */
    secp256k1_sha256_finalize(&hasher, output);
}

/* Add the index to the above hash to customize it for each pubkey */
static int secp256k1_compute_sighash(secp256k1_scalar *r, const unsigned char *prehash, size_t index) {
    unsigned char output[32];
    int overflow;
    secp256k1_sha256 hasher;
    secp256k1_sha256_initialize(&hasher);
    /* Encode index as a UTF8-style bignum */
    while (index > 0) {
        unsigned char ch = index & 0x7f;
        secp256k1_sha256_write(&hasher, &ch, 1);
        index >>= 7;
    }
    secp256k1_sha256_write(&hasher, prehash, 32);
    secp256k1_sha256_finalize(&hasher, output);
    secp256k1_scalar_set_b32(r, output, &overflow);
    return !overflow;
}

secp256k1_aggsig_context* secp256k1_aggsig_context_create(const secp256k1_context *ctx, const secp256k1_pubkey *pubkeys, size_t n_pubkeys, const unsigned char *seed) {
    secp256k1_aggsig_context* aggctx;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(pubkeys != NULL);
    ARG_CHECK(seed != NULL);

    aggctx = (secp256k1_aggsig_context*)checked_malloc(&ctx->error_callback, sizeof(*aggctx));
    aggctx->progress = (enum nonce_progress*)checked_malloc(&ctx->error_callback, n_pubkeys * sizeof(*aggctx->progress));
    aggctx->pubkeys = (secp256k1_pubkey*)checked_malloc(&ctx->error_callback, n_pubkeys * sizeof(*aggctx->pubkeys));
    aggctx->secnonce = (secp256k1_scalar*)checked_malloc(&ctx->error_callback, n_pubkeys * sizeof(*aggctx->secnonce));
    aggctx->n_sigs = n_pubkeys;
    secp256k1_gej_set_infinity(&aggctx->pubnonce_sum);
    memcpy(aggctx->pubkeys, pubkeys, n_pubkeys * sizeof(*aggctx->pubkeys));
    memset(aggctx->progress, 0, n_pubkeys * sizeof(*aggctx->progress));
    secp256k1_rfc6979_hmac_sha256_initialize(&aggctx->rng, seed, 32);
    return aggctx;
}

int secp256k1_aggsig_generate_nonce_single(const secp256k1_context* ctx, secp256k1_scalar *secnonce, secp256k1_gej* pubnonce, secp256k1_rfc6979_hmac_sha256* rng) {
    int retry;
    unsigned char data[32];

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(secnonce != NULL);
    ARG_CHECK(pubnonce != NULL);
    ARG_CHECK(rng != NULL);

    /* generate nonce from the RNG */
    do {
        secp256k1_rfc6979_hmac_sha256_generate(rng, data, 32);
        secp256k1_scalar_set_b32(secnonce, data, &retry);
        retry |= secp256k1_scalar_is_zero(secnonce);
    } while (retry); /* This branch true is cryptographically unreachable. Requires sha256_hmac output > Fp. */
    secp256k1_ecmult_gen(&ctx->ecmult_gen_ctx, pubnonce, secnonce);
    memset(data, 0, 32);  /* TODO proper clear */
    /* Negate nonce if needed to get y to be a quadratic residue */
    if (!secp256k1_gej_has_quad_y_var(pubnonce)) {
        secp256k1_scalar_negate(secnonce, secnonce);
        secp256k1_gej_neg(pubnonce, pubnonce);
    }
    return 1;
}

int secp256k1_aggsig_export_secnonce_single(const secp256k1_context* ctx, unsigned char* secnonce32, const unsigned char* seed) {
    secp256k1_scalar secnonce;
    secp256k1_gej pubnonce;
    secp256k1_rfc6979_hmac_sha256 rng;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(secnonce32 != NULL);
    secp256k1_rfc6979_hmac_sha256_initialize(&rng, seed, 32);

    if (secp256k1_aggsig_generate_nonce_single(ctx, &secnonce, &pubnonce, &rng) == 0){
       return 0;
    }

    secp256k1_scalar_get_b32(secnonce32, &secnonce);
    return 1;
}

/* TODO extend this to export the nonce if the user wants */
int secp256k1_aggsig_generate_nonce(const secp256k1_context* ctx, secp256k1_aggsig_context* aggctx, size_t index) {
    secp256k1_gej pubnon;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(aggctx != NULL);
    ARG_CHECK(index < aggctx->n_sigs);

    if (aggctx->progress[index] != NONCE_PROGRESS_UNKNOWN) {
        return 0;
    }
    if (secp256k1_aggsig_generate_nonce_single(ctx, &aggctx->secnonce[index], &pubnon, &aggctx->rng) == 0){
        return 0;
    }

    secp256k1_gej_add_var(&aggctx->pubnonce_sum, &aggctx->pubnonce_sum, &pubnon, NULL);
    aggctx->progress[index] = NONCE_PROGRESS_OURS;
    return 1;
}

int secp256k1_aggsig_sign_single(const secp256k1_context* ctx,
    unsigned char *sig64,
    const unsigned char *msg32,
    const unsigned char *seckey32,
    const unsigned char* secnonce32,
    const secp256k1_pubkey* pubnonce,
    const unsigned char* seed){

    secp256k1_scalar sighash;
    secp256k1_rfc6979_hmac_sha256 rng;
    secp256k1_scalar sec;
    secp256k1_ge tmp_ge;
    secp256k1_gej pubnonce_j;
    secp256k1_pubkey pub_tmp;

    secp256k1_scalar secnonce;
    secp256k1_ge final;
    int overflow;
    int retry;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(sig64 != NULL);
    ARG_CHECK(msg32 != NULL);
    ARG_CHECK(seckey32 != NULL);

    /* generate nonce if needed */
    if (secnonce32==NULL){
        secp256k1_rfc6979_hmac_sha256_initialize(&rng, seed, 32);
        if (secp256k1_aggsig_generate_nonce_single(ctx, &secnonce, &pubnonce_j, &rng) == 0){
            return 0;
        }
        secp256k1_rfc6979_hmac_sha256_finalize(&rng);
    } else {
        /* Use existing nonce */
        secp256k1_scalar_set_b32(&secnonce, secnonce32, &retry);
        secp256k1_ecmult_gen(&ctx->ecmult_gen_ctx, &pubnonce_j, &secnonce);
        /* TODO: Check if this is needed here */
        /* Negate nonce if needed to get y to be a quadratic residue */
        if (!secp256k1_gej_has_quad_y_var(&pubnonce_j)) {
            secp256k1_scalar_negate(&secnonce, &secnonce);
            secp256k1_gej_neg(&pubnonce_j, &pubnonce_j);
        }
    }

    /* compute signature hash (in the simple case just message+pubnonce) */
    secp256k1_ge_set_gej(&tmp_ge, &pubnonce_j);
    if (!secp256k1_gej_has_quad_y_var(&pubnonce_j)) {
        secp256k1_scalar_negate(&secnonce, &secnonce);
        secp256k1_ge_neg(&tmp_ge, &tmp_ge);
    }
    secp256k1_fe_normalize(&tmp_ge.x);

    if (pubnonce != NULL) {
        secp256k1_compute_sighash_single(ctx, &sighash, pubnonce, msg32);
    } else {
        secp256k1_pubkey_save(&pub_tmp, &tmp_ge);
        secp256k1_compute_sighash_single(ctx, &sighash, &pub_tmp, msg32);
    }
    /* calculate signature */
    secp256k1_scalar_set_b32(&sec, seckey32, &overflow);
    if (overflow) {
        secp256k1_scalar_clear(&sec);
        return 0;
    }

    secp256k1_scalar_mul(&sec, &sec, &sighash);
    secp256k1_scalar_add(&sec, &sec, &secnonce);

    /* finalize */
    secp256k1_scalar_get_b32(sig64, &sec);
    secp256k1_ge_set_gej(&final, &pubnonce_j);
    secp256k1_fe_normalize_var(&final.x);
    secp256k1_fe_get_b32(sig64 + 32, &final.x);

    secp256k1_scalar_clear(&sec);

    return 1;
}

int secp256k1_aggsig_partial_sign(const secp256k1_context* ctx, secp256k1_aggsig_context* aggctx, secp256k1_aggsig_partial_signature *partial, const unsigned char *msghash32, const unsigned char *seckey32, size_t index) {
    size_t i;
    secp256k1_scalar sighash;
    secp256k1_scalar sec;
    secp256k1_ge tmp_ge;
    int overflow;
    unsigned char prehash[32];

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_gen_context_is_built(&ctx->ecmult_gen_ctx));
    ARG_CHECK(aggctx != NULL);
    ARG_CHECK(partial != NULL);
    ARG_CHECK(msghash32 != NULL);
    ARG_CHECK(seckey32 != NULL);
    ARG_CHECK(index < aggctx->n_sigs);

    /* check state machine */
    for (i = 0; i < aggctx->n_sigs; i++) {
        if (aggctx->progress[i] == NONCE_PROGRESS_UNKNOWN) {
            return 0;
        }
    }
    if (aggctx->progress[index] != NONCE_PROGRESS_OURS) {
        return 0;
    }

    /* sign */
    /* If the total public nonce has wrong sign, negate our
     * secret nonce. Everyone will negate the public one
     * at combine time. */
    secp256k1_ge_set_gej(&tmp_ge, &aggctx->pubnonce_sum);  /* TODO cache this */
    if (!secp256k1_gej_has_quad_y_var(&aggctx->pubnonce_sum)) {
        secp256k1_scalar_negate(&aggctx->secnonce[index], &aggctx->secnonce[index]);
        secp256k1_ge_neg(&tmp_ge, &tmp_ge);
    }
    secp256k1_fe_normalize(&tmp_ge.x);
    secp256k1_compute_prehash(ctx, prehash, aggctx->pubkeys, aggctx->n_sigs, &tmp_ge.x, msghash32);
    if (secp256k1_compute_sighash(&sighash, prehash, index) == 0) {
        return 0;
    }
    secp256k1_scalar_set_b32(&sec, seckey32, &overflow);
    if (overflow) {
        secp256k1_scalar_clear(&sec);
        return 0;
    }
    secp256k1_scalar_mul(&sec, &sec, &sighash);
    secp256k1_scalar_add(&sec, &sec, &aggctx->secnonce[index]);

    /* finalize */
    secp256k1_scalar_get_b32(partial->data, &sec);
    secp256k1_scalar_clear(&sec);
    aggctx->progress[index] = NONCE_PROGRESS_SIGNED;
    return 1;
}

int secp256k1_aggsig_combine_signatures(const secp256k1_context* ctx, secp256k1_aggsig_context* aggctx, unsigned char *sig64, const secp256k1_aggsig_partial_signature *partial, size_t n_sigs) {
    size_t i;
    secp256k1_scalar s;
    secp256k1_ge final;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(aggctx != NULL);
    ARG_CHECK(sig64 != NULL);
    ARG_CHECK(partial != NULL);
    (void) ctx;

    if (n_sigs != aggctx->n_sigs) {
        return 0;
    }

    secp256k1_scalar_set_int(&s, 0);
    for (i = 0; i < n_sigs; i++) {
        secp256k1_scalar tmp;
        int overflow;
        secp256k1_scalar_set_b32(&tmp, partial[i].data, &overflow);
        if (overflow) {
            return 0;
        }
        secp256k1_scalar_add(&s, &s, &tmp);
    }

    /* If we need to negate the public nonce, everyone will
     * have negated their secret nonces in the previous step. */
    if (!secp256k1_gej_has_quad_y_var(&aggctx->pubnonce_sum)) {
        secp256k1_gej_neg(&aggctx->pubnonce_sum, &aggctx->pubnonce_sum);
    }

    secp256k1_scalar_get_b32(sig64, &s);
    secp256k1_ge_set_gej(&final, &aggctx->pubnonce_sum);
    secp256k1_fe_normalize_var(&final.x);
    secp256k1_fe_get_b32(sig64 + 32, &final.x);
    return 1;
}


typedef struct {
    const secp256k1_context *ctx;
    unsigned char prehash[32];
    secp256k1_scalar single_hash;
    const secp256k1_pubkey *pubkeys;
} secp256k1_verify_callback_data;

static int secp256k1_aggsig_verify_callback(secp256k1_scalar *sc, secp256k1_ge *pt, size_t idx, void *data) {
    secp256k1_verify_callback_data *cbdata = (secp256k1_verify_callback_data*) data;

    if (secp256k1_compute_sighash(sc, cbdata->prehash, idx) == 0) {
        return 0;
    }
    secp256k1_scalar_negate(sc, sc);
    secp256k1_pubkey_load(cbdata->ctx, pt, &cbdata->pubkeys[idx]);
    return 1;
}

int secp256k1_aggsig_verify(const secp256k1_context* ctx, secp256k1_scratch_space *scratch, const unsigned char *sig64, const unsigned char *msg32, const secp256k1_pubkey *pubkeys, size_t n_pubkeys) {
    secp256k1_scalar g_sc;
    secp256k1_gej pk_sum;
    secp256k1_ge pk_sum_ge;
    secp256k1_fe r_x;
    int overflow;
    secp256k1_verify_callback_data cbdata;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_context_is_built(&ctx->ecmult_ctx));
    ARG_CHECK(scratch != NULL);
    ARG_CHECK(sig64 != NULL);
    ARG_CHECK(msg32 != NULL);
    ARG_CHECK(pubkeys != NULL);
    (void) ctx;

    if (n_pubkeys == 0) {
        return 0;
    }

    /* extract s */
    secp256k1_scalar_set_b32(&g_sc, sig64, &overflow);
    if (overflow) {
        return 0;
    }

    /* extract R */
    if (!secp256k1_fe_set_b32(&r_x, sig64 + 32)) {
        return 0;
    }

    /* Populate callback data */
    cbdata.ctx = ctx;
    cbdata.pubkeys = pubkeys;
    secp256k1_compute_prehash(ctx, cbdata.prehash, pubkeys, n_pubkeys, &r_x, msg32);

    /* Compute sum sG - e_i*P_i, which should be R */
    if (!secp256k1_ecmult_multi_var(&ctx->ecmult_ctx, scratch, &ctx->error_callback, &pk_sum, &g_sc, secp256k1_aggsig_verify_callback, &cbdata, n_pubkeys)) {
        return 0;
    }

    /* Check sum */
    secp256k1_ge_set_gej(&pk_sum_ge, &pk_sum);
    return secp256k1_fe_equal_var(&r_x, &pk_sum_ge.x) &&
           secp256k1_gej_has_quad_y_var(&pk_sum);
}

int secp256k1_aggsig_build_scratch_and_verify(const secp256k1_context* ctx, 
                                              const unsigned char *sig64,
                                              const unsigned char *msg32,
                                              const secp256k1_pubkey *pubkeys, 
                                              size_t n_pubkeys) {
    /* just going to inefficiently allocate every time */
    secp256k1_scratch_space *scratch = secp256k1_scratch_space_create(ctx, 1024, 4096);
    int returnval=secp256k1_aggsig_verify(ctx, scratch, sig64, msg32, pubkeys, n_pubkeys);
    secp256k1_scratch_space_destroy(scratch);
    return returnval;
}

static int secp256k1_aggsig_verify_callback_single(secp256k1_scalar *sc, secp256k1_ge *pt, size_t idx, void *data) {
    secp256k1_verify_callback_data *cbdata = (secp256k1_verify_callback_data*) data;
    secp256k1_scalar_negate(sc, &cbdata->single_hash);
    secp256k1_pubkey_load(cbdata->ctx, pt, &cbdata->pubkeys[idx]);
    return 1;
}

int secp256k1_aggsig_verify_single(
    const secp256k1_context* ctx,
    const unsigned char *sig64,
    const unsigned char *msg32,
    const secp256k1_pubkey *pubnonce,
    const secp256k1_pubkey *pubkey){

    secp256k1_scalar g_sc;
    secp256k1_fe r_x;
    secp256k1_gej pk_sum;
    secp256k1_ge pk_sum_ge;
    secp256k1_scalar sighash;
    secp256k1_scratch_space *scratch;
    secp256k1_verify_callback_data cbdata;
    secp256k1_ge tmp_pubnonce_ge;
    secp256k1_pubkey tmp_pk;

    int overflow;

    VERIFY_CHECK(ctx != NULL);
    ARG_CHECK(secp256k1_ecmult_context_is_built(&ctx->ecmult_ctx));
    ARG_CHECK(sig64 != NULL);
    ARG_CHECK(msg32 != NULL);
    ARG_CHECK(pubkey != NULL);

    /* extract s */
    secp256k1_scalar_set_b32(&g_sc, sig64, &overflow);
    if (overflow) {
        return 0;
    }

    /* extract R */
    if (!secp256k1_fe_set_b32(&r_x, sig64 + 32)) {
        return 0;
    }

    /* compute e = sighash */
    if (pubnonce != NULL) {
        secp256k1_compute_sighash_single(ctx, &sighash, pubnonce, msg32);
    } else {
        secp256k1_ge_set_xquad(&tmp_pubnonce_ge, &r_x);
        secp256k1_pubkey_save(&tmp_pk, &tmp_pubnonce_ge);
        secp256k1_compute_sighash_single(ctx, &sighash, &tmp_pk, msg32);
    }

    /* Populate callback data */
    cbdata.ctx = ctx;
    cbdata.pubkeys = pubkey;
    cbdata.single_hash = sighash;

    scratch = secp256k1_scratch_space_create(ctx, 1024, 4096);
    /* Compute sG - eP, which should be R */
    if (!secp256k1_ecmult_multi_var(&ctx->ecmult_ctx, scratch, &ctx->error_callback, &pk_sum, &g_sc, secp256k1_aggsig_verify_callback_single, &cbdata, 1)) {
        return 0;
    }

    secp256k1_scratch_space_destroy(scratch);

    secp256k1_ge_set_gej(&pk_sum_ge, &pk_sum);
    return secp256k1_fe_equal_var(&r_x, &pk_sum_ge.x) &&
           secp256k1_gej_has_quad_y_var(&pk_sum);

}

void secp256k1_aggsig_context_destroy(secp256k1_aggsig_context *aggctx) {
    if (aggctx == NULL) {
        return;
    }
    memset(aggctx->pubkeys, 0, aggctx->n_sigs * sizeof(*aggctx->pubkeys));
    memset(aggctx->secnonce, 0, aggctx->n_sigs * sizeof(*aggctx->secnonce));
    memset(aggctx->progress, 0, aggctx->n_sigs * sizeof(*aggctx->progress));
    free(aggctx->pubkeys);
    free(aggctx->secnonce);
    free(aggctx->progress);
    secp256k1_rfc6979_hmac_sha256_finalize(&aggctx->rng);
    free(aggctx);
}

#endif
