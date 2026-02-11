/*
 * oauth.h - OAuth login flow for Claude C client
 *
 * Implements OAuth 2.0 Authorization Code flow with PKCE
 * for independent authentication (no dependency on Claude Code).
 */

#ifndef OAUTH_H
#define OAUTH_H

#include "auth.h"
#include <stddef.h>

/* SHA256 hash (shared with api.c for billing header)
 * Returns allocated hash bytes, caller must free()
 * out_len receives the hash length (32 bytes)
 */
unsigned char* sha256(const char* input, size_t* out_len);

/* PKCE parameters */
typedef struct {
    char* code_verifier;    /* Random 43-128 char string */
    char* code_challenge;   /* base64url(SHA256(code_verifier)) */
    char* state;            /* Random state for CSRF protection */
} pkce_t;

/* Generate PKCE parameters
 * Returns allocated pkce_t or NULL on failure
 * Caller must call pkce_free()
 */
pkce_t* pkce_generate(void);

/* Free PKCE parameters */
void pkce_free(pkce_t* pkce);

/* Build authorization URL
 * Returns allocated URL string or NULL on failure
 * Caller must free() the result
 */
char* oauth_build_auth_url(const pkce_t* pkce, int port);

/* Start local HTTP server and wait for callback
 * Returns authorization code or NULL on failure/timeout
 * Caller must free() the result
 */
char* oauth_wait_for_callback(int port, const char* expected_state, int timeout_sec);

/* Exchange authorization code for tokens
 * Returns 0 on success, -1 on failure
 * On success, tokens are saved to storage
 */
int oauth_exchange_code(const char* code, const pkce_t* pkce, int port);

/* Run full OAuth login flow
 * Returns 0 on success, -1 on failure
 */
int oauth_login(void);

#endif /* OAUTH_H */
