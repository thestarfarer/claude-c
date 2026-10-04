/*
 * auth.c - Authentication handling for Claude C client
 *
 * Uses separate credential storage from Claude Code.
 * Migrates credentials on first run, then manages independently.
 */

#define _GNU_SOURCE  /* For timegm() */

#include "auth.h"
#include "api.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <curl/curl.h>
#include <limits.h>

/* Storage paths */
#define CLAUDE_C_STATE_FILE ".claude/claude-c.json"
#define CLAUDE_CODE_CREDS_FILE ".claude/.credentials.json"

/* Get current time in milliseconds */
static long long current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Read entire file into string */
static char* read_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    char* content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(content, 1, size, f);
    fclose(f);

    content[nread] = '\0';
    return content;
}

/* Ensure directory exists */
static int ensure_dir(const char* path) {
    char* dir = strdup(path);
    if (!dir) return -1;

    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        struct stat st;
        if (stat(dir, &st) != 0) {
            char* parent_slash = strrchr(dir, '/');
            if (parent_slash) {
                *parent_slash = '\0';
                mkdir(dir, 0700);
                *parent_slash = '/';
            }
            mkdir(dir, 0700);
        }
    }

    free(dir);
    return 0;
}

/* Get claude-c state file path */
static char* get_state_path(void) {
    const char* home = getenv("HOME");
    if (!home) return NULL;

    size_t len = strlen(home) + strlen(CLAUDE_C_STATE_FILE) + 2;
    char* path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/%s", home, CLAUDE_C_STATE_FILE);
    return path;
}

/* Exclusive advisory lock serializing EVERY reader-modify-writer of the
 * shared state file (~/.claude/claude-c.json) across concurrent claude-c
 * processes: token refresh (single-use refresh tokens race), oauth_save and
 * state_save (both read-modify-write the same file and can resurrect stale
 * fields over a sibling's fresh write). Lock lives on a SEPARATE file — the
 * state file itself is replaced by rename(). The kernel releases flocks on
 * process death, so a crash can never wedge the lock.
 *
 * Re-entrant within a process (claude-c is single-threaded): the refresh path
 * holds the lock while oauth_refresh -> oauth_save runs, which locks again.
 * flock would deadlock on a second fd in the same process, so a depth counter
 * short-circuits nested acquisitions.
 * Returns 0 on success (caller MUST claudec_unlockfile()), -1 if unavailable
 * (caller proceeds unlocked — degrades to the old racy behavior). */
static int g_lock_fd = -1;
static int g_lock_depth = 0;

int claudec_lockfile(void) {
    if (g_lock_depth > 0) {
        g_lock_depth++;
        return 0;
    }
    const char* home = getenv("HOME");
    if (!home) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/.claude/claude-c.lock", home);
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    g_lock_fd = fd;
    g_lock_depth = 1;
    return 0;
}

void claudec_unlockfile(void) {
    if (g_lock_depth > 1) {
        g_lock_depth--;
        return;
    }
    if (g_lock_depth == 1 && g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
    g_lock_depth = 0;
}

/* Get Claude Code credentials file path */
static char* get_claude_code_creds_path(void) {
    const char* config_dir = getenv("CLAUDE_CONFIG_DIR");
    const char* home = getenv("HOME");
    const char* base = config_dir ? config_dir : home;

    if (!base) return NULL;

    size_t len = strlen(base) + strlen(CLAUDE_CODE_CREDS_FILE) + 2;
    char* path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/%s", base, CLAUDE_CODE_CREDS_FILE);
    return path;
}

/* Parse ISO 8601 date string to milliseconds timestamp */
static long long parse_iso_date(const char* iso_date) {
    if (!iso_date) return 0;

    /* Try parsing as milliseconds first (number) */
    char* endptr;
    long long ts = strtoll(iso_date, &endptr, 10);
    if (*endptr == '\0' && ts > 1000000000000LL) {
        return ts;  /* Already milliseconds */
    }

    /* Parse ISO 8601 format: 2024-01-15T10:30:00.000Z */
    struct tm tm = {0};
    int ms = 0;

    /* Try with milliseconds */
    if (sscanf(iso_date, "%d-%d-%dT%d:%d:%d.%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) {
            return (long long)t * 1000 + ms;
        }
    }

    /* Try without milliseconds */
    if (sscanf(iso_date, "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t t = timegm(&tm);
        if (t != -1) {
            return (long long)t * 1000;
        }
    }

    return 0;
}

/* Check if OAuth token needs refresh */
int oauth_needs_refresh(long long expires_at) {
    if (expires_at == 0) return 1;  /* No expiry info, assume expired */
    long long now = current_time_ms();
    return now + OAUTH_REFRESH_BUFFER_MS >= expires_at;
}

/* Free OAuth credentials */
void oauth_creds_free(oauth_creds_t* creds) {
    if (!creds) return;
    if (creds->access_token) {
        memset(creds->access_token, 0, strlen(creds->access_token));
        free(creds->access_token);
    }
    if (creds->refresh_token) {
        memset(creds->refresh_token, 0, strlen(creds->refresh_token));
        free(creds->refresh_token);
    }
    free(creds->scopes);
    free(creds->client_id);
    free(creds);
}

/* Load OAuth credentials from claude-c storage */
oauth_creds_t* oauth_load(void) {
    char* path = get_state_path();
    if (!path) return NULL;

    char* content = read_file(path);
    free(path);
    if (!content) return NULL;

    /* Check for oauth credentials in state */
    char* access_token = json_get_string(content, "oauth.accessToken");
    if (!access_token) {
        free(content);
        return NULL;
    }

    oauth_creds_t* creds = calloc(1, sizeof(oauth_creds_t));
    if (!creds) {
        free(access_token);
        free(content);
        return NULL;
    }

    creds->access_token = access_token;
    creds->refresh_token = json_get_string(content, "oauth.refreshToken");
    creds->scopes = json_get_scopes(content, "oauth.scopes");
    creds->client_id = json_get_string(content, "oauth.clientId");
    creds->refresh_expires_at = json_get_number(content, "oauth.refreshTokenExpiresAt", NULL);

    int found_expires = 0;
    creds->expires_at = json_get_number(content, "oauth.expiresAt", &found_expires);

    free(content);
    return creds;
}

/* Save OAuth credentials to claude-c storage.
 * Serialized under the shared writer lock; preserves ALL non-oauth state
 * fields (userId, accountUuid, sessionId — dropping sessionId here is how
 * credentials got mangled once); writes atomically via tmp+rename so a
 * concurrent reader can never see a torn/partial file. */
int oauth_save(const oauth_creds_t* creds) {
    if (!creds || !creds->access_token) return -1;

    char* path = get_state_path();
    if (!path) return -1;

    ensure_dir(path);

    int locked = claudec_lockfile();

    /* Read existing state (under lock, so it cannot be mid-rewrite) */
    char* existing = read_file(path);
    char* user_id = NULL;
    char* account_uuid = NULL;
    char* session_id = NULL;

    if (existing) {
        user_id = json_get_string(existing, "userId");
        account_uuid = json_get_string(existing, "accountUuid");
        session_id = json_get_string(existing, "sessionId");
        free(existing);
    }

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        if (locked == 0) claudec_unlockfile();
        free(path);
        free(user_id);
        free(account_uuid);
        free(session_id);
        return -1;
    }

    fprintf(f, "{\n");

    if (user_id) {
        fprintf(f, "  \"userId\": \"%s\",\n", user_id);
        free(user_id);
    }

    if (account_uuid) {
        fprintf(f, "  \"accountUuid\": \"%s\",\n", account_uuid);
        free(account_uuid);
    }

    if (session_id) {
        fprintf(f, "  \"sessionId\": \"%s\",\n", session_id);
        free(session_id);
    }

    fprintf(f, "  \"oauth\": {\n");
    fprintf(f, "    \"accessToken\": \"%s\"", creds->access_token);

    if (creds->refresh_token) {
        fprintf(f, ",\n    \"refreshToken\": \"%s\"", creds->refresh_token);
    }

    if (creds->expires_at > 0) {
        fprintf(f, ",\n    \"expiresAt\": %lld", creds->expires_at);
    }

    if (creds->scopes) {
        fprintf(f, ",\n    \"scopes\": \"%s\"", creds->scopes);
    }

    if (creds->client_id) {
        fprintf(f, ",\n    \"clientId\": \"%s\"", creds->client_id);
    }
    if (creds->refresh_expires_at > 0) {
        fprintf(f, ",\n    \"refreshTokenExpiresAt\": %lld", creds->refresh_expires_at);
    }
    fprintf(f, "\n  }\n");
    fprintf(f, "}\n");

    fclose(f);
    int rc = rename(tmp_path, path);
    if (locked == 0) claudec_unlockfile();
    free(path);
    if (rc != 0) return -1;
    return 0;
}

/* Curl write callback */
typedef struct {
    char* data;
    size_t size;
} response_t;

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    response_t* resp = (response_t*)userdata;
    size_t bytes = size * nmemb;

    char* new_data = realloc(resp->data, resp->size + bytes + 1);
    if (!new_data) return 0;

    resp->data = new_data;
    memcpy(resp->data + resp->size, ptr, bytes);
    resp->size += bytes;
    resp->data[resp->size] = '\0';

    return bytes;
}

/* Match whole scope names, not substrings. */
static int has_scope(const char* scopes, const char* scope) {
    if (!scopes) return 0;
    size_t n = strlen(scope);
    const char* p = scopes;
    while ((p = strstr(p, scope))) {
        if ((p == scopes || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
        p += n;
    }
    return 0;
}

static char* refresh_scopes(const oauth_creds_t* creds) {
    if ((creds->client_id && strcmp(creds->client_id, OAUTH_CLIENT_ID) != 0)
        || (creds->scopes && *creds->scopes && !has_scope(creds->scopes, "user:inference")))
        return strdup(creds->scopes ? creds->scopes : "");
    char* scopes = malloc(sizeof(OAUTH_REFRESH_SCOPES) + 64);
    if (!scopes) return NULL;
    strcpy(scopes, OAUTH_REFRESH_SCOPES);
    if (has_scope(creds->scopes, "user:projects:read")) strcat(scopes, " user:projects:read");
    if (has_scope(creds->scopes, "user:projects:write")) strcat(scopes, " user:projects:write");
    return scopes;
}

/* Refresh under the caller's shared writer lock, preserving the original grant
 * if an older account rejects the expanded 2.1.281 scope list. */
int oauth_refresh(oauth_creds_t* creds) {
    if (!creds || !creds->refresh_token) return -1;
    if (creds->refresh_expires_at > 0 && current_time_ms() >= creds->refresh_expires_at) {
        fprintf(stderr, "Claude refresh token expired. Run './claude-c --login'.\n");
        return -1;
    }
    char* scopes = refresh_scopes(creds);
    char* token = json_escape_string(creds->refresh_token);
    char* client = json_escape_string(creds->client_id ? creds->client_id : OAUTH_CLIENT_ID);
    CURL* curl = curl_easy_init();
    if (!scopes || !token || !client || !curl) {
        free(scopes); free(token); free(client);
        if (curl) curl_easy_cleanup(curl);
        return -1;
    }
    struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, OAUTH_TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    int result = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        const char* requested = attempt ? creds->scopes : scopes;
        char* escaped = json_escape_string(requested);
        if (!escaped) break;
        size_t len = strlen(token) + strlen(client) + strlen(escaped) + 128;
        char* body = malloc(len);
        if (!body) { free(escaped); break; }
        snprintf(body, len, "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\",\"client_id\":\"%s\",\"scope\":\"%s\"}", token, client, escaped);
        free(escaped);
        response_t resp = {0};
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        CURLcode res = curl_easy_perform(curl);
        free(body);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (res != CURLE_OK) {
            fprintf(stderr, "OAuth refresh request failed: %s\n", curl_easy_strerror(res));
            free(resp.data); break;
        }
        if (status != 200) {
            int retry = attempt == 0 && creds->scopes && *creds->scopes
                && strcmp(requested, creds->scopes) != 0
                && (json_string_equals(resp.data, "error", "invalid_scope")
                    || json_string_equals(resp.data, "error.type", "invalid_scope"));
            free(resp.data);
            if (retry) continue;
            /* Do not print OAuth error bodies; they can contain credentials. */
            fprintf(stderr, "OAuth refresh failed: HTTP %ld\n", status);
            break;
        }
        char* access = json_get_string(resp.data, "access_token");
        char* refresh = json_get_string(resp.data, "refresh_token");
        char* granted = json_get_string(resp.data, "scope");
        char* refresh_field = json_get_raw(resp.data, "refresh_token");
        long long expires = json_get_number(resp.data, "expires_in", NULL);
        long long refresh_expires = json_get_number(resp.data, "refresh_token_expires_in", NULL);
        long long now = current_time_ms();
        int valid = access && *access && !strpbrk(access, "\r\n\"\\")
            && expires > 0 && expires <= (LLONG_MAX - now) / 1000
            && (!refresh_field || (refresh && *refresh && !strpbrk(refresh, "\r\n\"\\")));
        free(refresh_field); free(resp.data);
        if (!valid) {
            fprintf(stderr, "Claude OAuth returned invalid refresh credentials.\n");
            free(access); free(refresh); free(granted); break;
        }
        if (!granted) granted = strdup(requested);
        if (!granted) { free(access); free(refresh); break; }
        free(creds->access_token); creds->access_token = access;
        if (refresh) { free(creds->refresh_token); creds->refresh_token = refresh; }
        free(creds->scopes); creds->scopes = granted;
        creds->expires_at = now + expires * 1000;
        if (refresh_expires > 0 && refresh_expires <= (LLONG_MAX - now) / 1000)
            creds->refresh_expires_at = now + refresh_expires * 1000;
        result = oauth_save(creds);
        if (result == 0) DEBUG("OAuth token refreshed successfully\n");
        else fprintf(stderr, "Failed to save refreshed Claude credentials.\n");
        break;
    }
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    free(scopes); free(token); free(client);
    return result;
}

int auth_recover_unauthorized(auth_t* auth) {
    if (!auth || auth->type != AUTH_OAUTH) return -1;
    /* Never rotate an OAuth token without the cross-process lock. */
    if (claudec_lockfile() != 0) return -1;
    oauth_creds_t* creds = oauth_load();
    int result = -1;
    if (creds) {
        int needs_refresh = strcmp(creds->access_token, auth->value) == 0 || oauth_needs_refresh(creds->expires_at);
        if (!needs_refresh || oauth_refresh(creds) == 0) {
            char* replacement = strdup(creds->access_token);
            if (replacement) { free(auth->value); auth->value = replacement; result = 0; }
        }
    }
    oauth_creds_free(creds);
    claudec_unlockfile();
    return result;
}

/* Migrate OAuth credentials from Claude Code storage */
int oauth_migrate_from_claude_code(void) {
    char* path = get_claude_code_creds_path();
    if (!path) return -1;

    char* content = read_file(path);
    free(path);
    if (!content) return -1;

    /* Try to extract Claude AI OAuth credentials */
    char* access_token = json_get_string(content, "claudeAiOauth.accessToken");
    if (!access_token) {
        free(content);
        return -1;
    }

    oauth_creds_t creds = {0};
    creds.access_token = access_token;
    creds.refresh_token = json_get_string(content, "claudeAiOauth.refreshToken");
    creds.scopes = json_get_scopes(content, "claudeAiOauth.scopes");
    creds.client_id = json_get_string(content, "claudeAiOauth.clientId");
    creds.refresh_expires_at = json_get_number(content, "claudeAiOauth.refreshTokenExpiresAt", NULL);
    creds.expires_at = json_get_number(content, "claudeAiOauth.expiresAt", NULL);

    char* expires_str = json_get_string(content, "claudeAiOauth.expiresAt");
    if (expires_str) {
        creds.expires_at = parse_iso_date(expires_str);
        free(expires_str);
    }

    free(content);

    /* Save to claude-c storage */
    int result = oauth_save(&creds);

    if (result == 0) {
        DEBUG("Migrated OAuth credentials from Claude Code\n");
    }

    /* Free strings (oauth_save made copies) */
    free(creds.access_token);
    free(creds.refresh_token);
    free(creds.scopes);
    free(creds.client_id);

    return result;
}

/* Main auth load function */
auth_t auth_load(void) {
    auth_t auth = { AUTH_NONE, NULL };

    /* Priority 1: ANTHROPIC_API_KEY environment variable */
    const char* api_key = getenv("ANTHROPIC_API_KEY");
    if (api_key && *api_key) {
        auth.type = AUTH_API_KEY;
        auth.value = strdup(api_key);
        return auth;
    }

    /* Priority 2: OAuth from claude-c storage */
    oauth_creds_t* creds = oauth_load();

    /* Priority 3: Migrate from Claude Code — ONLY on genuine first run (state
     * file absent). Migrating whenever oauth_load returns NULL is dangerous:
     * a transiently unreadable state file would resurrect Claude Code's stale
     * tokens over live ones (this happened; the stale refresh token then
     * fails invalid_grant forever). */
    if (!creds) {
        char* sp = get_state_path();
        int missing = (!sp || access(sp, F_OK) != 0);
        free(sp);
        if (missing && oauth_migrate_from_claude_code() == 0) {
            creds = oauth_load();
        }
    }

    if (!creds) {
        fprintf(stderr, "No authentication found.\n");
        fprintf(stderr, "Run './claude-c --login' to authenticate,\n");
        fprintf(stderr, "or set ANTHROPIC_API_KEY environment variable.\n");
        return auth;
    }

    /* Check if token needs refresh. Serialized under an advisory lock with a
     * double-check re-read: while we waited for the lock, a sibling process
     * may already have refreshed and rotated the (single-use) refresh token —
     * in that case the re-read hands us the fresh credentials and we skip. */
    if (creds->refresh_token && oauth_needs_refresh(creds->expires_at)) {
        int locked = claudec_lockfile();
        if (locked != 0) {
            fprintf(stderr, "Cannot lock Claude credentials for refresh.\n");
            oauth_creds_free(creds); return auth;
        }

        oauth_creds_free(creds);
        creds = oauth_load();
        if (!creds) {
            if (locked == 0) claudec_unlockfile();
            fprintf(stderr, "Credentials vanished during refresh lock.\n");
            return auth;
        }

        if (creds->refresh_token && oauth_needs_refresh(creds->expires_at)) {
            DEBUG("OAuth token expired or expiring soon, refreshing...\n");
            if (oauth_refresh(creds) != 0) {
                if (locked == 0) claudec_unlockfile();
                fprintf(stderr, "Token refresh failed. Try running 'claude' to re-authenticate.\n");
                oauth_creds_free(creds);
                return auth;
            }
        }
        if (locked == 0) claudec_unlockfile();
    }

    auth.type = AUTH_OAUTH;
    auth.value = strdup(creds->access_token);

    oauth_creds_free(creds);
    return auth;
}

void auth_free(auth_t* auth) {
    if (auth && auth->value) {
        memset(auth->value, 0, strlen(auth->value));
        free(auth->value);
        auth->value = NULL;
    }
    if (auth) {
        auth->type = AUTH_NONE;
    }
}
