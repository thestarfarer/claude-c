/*
 * state.h - Persistent state and metadata for Claude C client
 *
 * Mimics the real Claude Code CLI metadata format:
 *   user_{userID}_account_{accountUuid}_session_{sessionId}
 */

#ifndef STATE_H
#define STATE_H

/* Persistent state cached to ~/.claude/claude-c.json */
typedef struct {
    char* user_id;       /* 64-char hex, persisted across sessions */
    char* account_uuid;  /* From OAuth profile, cached */
} state_t;

/* Load or create persistent state
 * Creates ~/.claude/claude-c.json if needed
 * Generates user_id if not present
 * Caller must call state_free()
 */
state_t state_load(void);

/* Save state to disk */
int state_save(const state_t* state);

/* Free state resources */
void state_free(state_t* state);

/* Generate UUID v4 string (37 bytes including null)
 * Caller must provide buffer of at least 37 bytes
 */
void generate_uuid_v4(char* buf);

/* Fetch and cache account info from OAuth profile API
 * Updates state with account_uuid
 * Returns 0 on success, -1 on failure
 */
int state_fetch_profile(state_t* state, const char* access_token);

/* Build the full metadata user_id string
 * Format: user_{userID}_account_{accountUuid}_session_{sessionId}
 * session_id should be a UUID v4 string
 * Returns newly allocated string, caller must free()
 */
char* state_build_metadata(const state_t* state, const char* session_id);

#endif /* STATE_H */
