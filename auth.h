/*
 * auth.h - Authentication handling for Claude C client
 *
 * Manages OAuth tokens with automatic refresh, using separate storage
 * from the main Claude Code CLI. On first run, migrates credentials
 * from Claude Code's storage.
 */

#ifndef AUTH_H
#define AUTH_H

#include <time.h>

/* OAuth configuration - must match Claude Code */
#define OAUTH_TOKEN_URL "https://console.anthropic.com/v1/oauth/token"
#define OAUTH_CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define OAUTH_SCOPES "user:profile user:inference user:sessions:claude_code"
#define OAUTH_REFRESH_BUFFER_MS 300000  /* 5 minutes before expiry */

/* Authentication type */
typedef enum {
    AUTH_NONE,
    AUTH_API_KEY,
    AUTH_OAUTH
} auth_type_t;

/* OAuth credentials - stored in claude-c's own storage */
typedef struct {
    char* access_token;
    char* refresh_token;
    long long expires_at;      /* Unix timestamp in milliseconds */
    char* scopes;              /* Space-separated scopes */
} oauth_creds_t;

/* Authentication result */
typedef struct {
    auth_type_t type;
    char* value;               /* API key or access token */
} auth_t;

/* Load authentication
 * Priority:
 *   1. ANTHROPIC_API_KEY environment variable
 *   2. OAuth token from claude-c storage (with auto-refresh)
 *   3. Migrate from Claude Code storage on first run
 *
 * Returns auth with type AUTH_NONE if no authentication available.
 * Caller must call auth_free().
 */
auth_t auth_load(void);

/* Free authentication resources */
void auth_free(auth_t* auth);

/* Check if OAuth token needs refresh (expires within buffer time) */
int oauth_needs_refresh(long long expires_at);

/* Refresh OAuth token using refresh_token
 * Returns 0 on success, -1 on failure
 * On success, updates the credentials in place
 */
int oauth_refresh(oauth_creds_t* creds);

/* Load OAuth credentials from claude-c storage
 * Returns credentials or NULL if not found
 * Caller must call oauth_creds_free()
 */
oauth_creds_t* oauth_load(void);

/* Save OAuth credentials to claude-c storage
 * Returns 0 on success, -1 on failure
 */
int oauth_save(const oauth_creds_t* creds);

/* Free OAuth credentials */
void oauth_creds_free(oauth_creds_t* creds);

/* Migrate OAuth credentials from Claude Code storage
 * Only called on first run when claude-c has no credentials
 * Returns 0 on success, -1 on failure
 */
int oauth_migrate_from_claude_code(void);

#endif /* AUTH_H */
