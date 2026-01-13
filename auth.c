/*
 * auth.c - Authentication handling for Claude C client
 *
 * Uses separate credential storage from Claude Code.
 * Migrates credentials on first run, then manages independently.
 */

#define _GNU_SOURCE  /* For timegm() */

#include "auth.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <curl/curl.h>

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
    if (expires_at == 0) return 0;  /* No expiry info, assume valid */
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
    creds->scopes = json_get_string(content, "oauth.scopes");

    char* expires_str = json_get_string(content, "oauth.expiresAt");
    if (expires_str) {
        creds->expires_at = parse_iso_date(expires_str);
        free(expires_str);
    }

    free(content);
    return creds;
}

/* Save OAuth credentials to claude-c storage */
int oauth_save(const oauth_creds_t* creds) {
    if (!creds || !creds->access_token) return -1;

    char* path = get_state_path();
    if (!path) return -1;

    ensure_dir(path);

    /* Read existing state */
    char* existing = read_file(path);
    char* user_id = NULL;
    char* account_uuid = NULL;

    if (existing) {
        user_id = json_get_string(existing, "userId");
        account_uuid = json_get_string(existing, "accountUuid");
        free(existing);
    }

    FILE* f = fopen(path, "w");
    free(path);
    if (!f) {
        free(user_id);
        free(account_uuid);
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

    fprintf(f, "\n  }\n");
    fprintf(f, "}\n");

    fclose(f);
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

/* Refresh OAuth token */
int oauth_refresh(oauth_creds_t* creds) {
    if (!creds || !creds->refresh_token) return -1;

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    response_t resp = {NULL, 0};
    struct curl_slist* headers = NULL;

    /* Build request body */
    char* escaped_refresh = curl_easy_escape(curl, creds->refresh_token, 0);
    char* escaped_client = curl_easy_escape(curl, OAUTH_CLIENT_ID, 0);
    char* escaped_scopes = curl_easy_escape(curl, OAUTH_SCOPES, 0);

    size_t body_len = 256 + strlen(escaped_refresh) + strlen(escaped_client) + strlen(escaped_scopes);
    char* body = malloc(body_len);
    if (!body) {
        curl_free(escaped_refresh);
        curl_free(escaped_client);
        curl_free(escaped_scopes);
        curl_easy_cleanup(curl);
        return -1;
    }

    snprintf(body, body_len,
        "grant_type=refresh_token&refresh_token=%s&client_id=%s&scope=%s",
        escaped_refresh, escaped_client, escaped_scopes);

    curl_free(escaped_refresh);
    curl_free(escaped_client);
    curl_free(escaped_scopes);

    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL, OAUTH_TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    int result = -1;

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200 && resp.data) {
            char* new_access = json_get_string(resp.data, "access_token");
            char* new_refresh = json_get_string(resp.data, "refresh_token");
            char* expires_in_str = json_get_string(resp.data, "expires_in");

            if (new_access) {
                /* Update credentials */
                free(creds->access_token);
                creds->access_token = new_access;

                if (new_refresh) {
                    free(creds->refresh_token);
                    creds->refresh_token = new_refresh;
                }

                if (expires_in_str) {
                    long expires_in = atol(expires_in_str);
                    creds->expires_at = current_time_ms() + (expires_in * 1000);
                    free(expires_in_str);
                }

                /* Save updated credentials */
                oauth_save(creds);
                result = 0;

                fprintf(stderr, "OAuth token refreshed successfully\n");
            } else {
                free(new_refresh);
            }
        } else {
            fprintf(stderr, "OAuth refresh failed: HTTP %ld\n", http_code);
            if (resp.data) {
                char* error = json_get_string(resp.data, "error");
                char* desc = json_get_string(resp.data, "error_description");
                if (error) fprintf(stderr, "  Error: %s\n", error);
                if (desc) fprintf(stderr, "  Description: %s\n", desc);
                free(error);
                free(desc);
            }
        }
    } else {
        fprintf(stderr, "OAuth refresh request failed: %s\n", curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    free(resp.data);

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
    creds.scopes = json_get_string(content, "claudeAiOauth.scopes");

    char* expires_str = json_get_string(content, "claudeAiOauth.expiresAt");
    if (expires_str) {
        creds.expires_at = parse_iso_date(expires_str);
        free(expires_str);
    }

    free(content);

    /* Save to claude-c storage */
    int result = oauth_save(&creds);

    if (result == 0) {
        fprintf(stderr, "Migrated OAuth credentials from Claude Code\n");
    }

    /* Free strings (oauth_save made copies) */
    free(creds.access_token);
    free(creds.refresh_token);
    free(creds.scopes);

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

    /* Priority 3: Migrate from Claude Code on first run */
    if (!creds) {
        if (oauth_migrate_from_claude_code() == 0) {
            creds = oauth_load();
        }
    }

    if (!creds) {
        fprintf(stderr, "No authentication found.\n");
        fprintf(stderr, "Run 'claude' first to set up OAuth authentication,\n");
        fprintf(stderr, "or set ANTHROPIC_API_KEY environment variable.\n");
        return auth;
    }

    /* Check if token needs refresh */
    if (creds->refresh_token && oauth_needs_refresh(creds->expires_at)) {
        fprintf(stderr, "OAuth token expired or expiring soon, refreshing...\n");
        if (oauth_refresh(creds) != 0) {
            fprintf(stderr, "Token refresh failed. Try running 'claude' to re-authenticate.\n");
            oauth_creds_free(creds);
            return auth;
        }
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
