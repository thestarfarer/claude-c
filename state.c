/*
 * state.c - Persistent state and metadata for Claude C client
 */

#include "state.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <curl/curl.h>

#define STATE_FILE ".claude/claude-c.json"
#define PROFILE_URL "https://api.anthropic.com/api/oauth/profile"
#define OAUTH_BETA "oauth-2025-04-20"

/* Get state file path */
static char* get_state_path(void) {
    const char* home = getenv("HOME");
    if (!home) return NULL;

    size_t len = strlen(home) + strlen(STATE_FILE) + 2;
    char* path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/%s", home, STATE_FILE);
    return path;
}

/* Generate 64-char random hex string */
static char* generate_user_id(void) {
    char* id = malloc(65);
    if (!id) return NULL;

    /* Seed with time + pid for uniqueness */
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    srand(seed);

    /* Try /dev/urandom first for better randomness */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned char bytes[32];
        if (read(fd, bytes, 32) == 32) {
            for (int i = 0; i < 32; i++) {
                sprintf(id + i*2, "%02x", bytes[i]);
            }
            close(fd);
            return id;
        }
        close(fd);
    }

    /* Fallback to rand() */
    for (int i = 0; i < 64; i++) {
        int nibble = rand() % 16;
        id[i] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
    }
    id[64] = '\0';

    return id;
}

/* Generate UUID v4 */
void generate_uuid_v4(char* buf) {
    unsigned char bytes[16];

    /* Try /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, bytes, 16) != 16) {
            /* Fallback */
            unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
            srand(seed);
            for (int i = 0; i < 16; i++) bytes[i] = rand() % 256;
        }
        close(fd);
    } else {
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
        srand(seed);
        for (int i = 0; i < 16; i++) bytes[i] = rand() % 256;
    }

    /* Set version (4) and variant (RFC 4122) */
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    sprintf(buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* Read file contents */
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
            /* Try to create parent first */
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

state_t state_load(void) {
    state_t state = {NULL, NULL};

    char* path = get_state_path();
    if (!path) {
        state.user_id = generate_user_id();
        return state;
    }

    char* content = read_file(path);
    if (content) {
        state.user_id = json_get_string(content, "userId");
        state.account_uuid = json_get_string(content, "accountUuid");
        free(content);
    }

    /* Generate user_id if missing */
    if (!state.user_id) {
        state.user_id = generate_user_id();
        state_save(&state);
    }

    free(path);
    return state;
}

int state_save(const state_t* state) {
    char* path = get_state_path();
    if (!path) return -1;

    ensure_dir(path);

    /* Read existing file to preserve oauth credentials */
    char* existing = read_file(path);
    char* oauth_access = NULL;
    char* oauth_refresh = NULL;
    char* oauth_scopes = NULL;
    char* oauth_expires = NULL;

    if (existing) {
        oauth_access = json_get_string(existing, "oauth.accessToken");
        oauth_refresh = json_get_string(existing, "oauth.refreshToken");
        oauth_scopes = json_get_string(existing, "oauth.scopes");
        oauth_expires = json_get_string(existing, "oauth.expiresAt");
        free(existing);
    }

    FILE* f = fopen(path, "w");
    free(path);

    if (!f) {
        free(oauth_access);
        free(oauth_refresh);
        free(oauth_scopes);
        free(oauth_expires);
        return -1;
    }

    fprintf(f, "{\n");

    int has_prev = 0;

    if (state->user_id) {
        fprintf(f, "  \"userId\": \"%s\"", state->user_id);
        has_prev = 1;
    }

    if (state->account_uuid) {
        if (has_prev) fprintf(f, ",\n");
        fprintf(f, "  \"accountUuid\": \"%s\"", state->account_uuid);
        has_prev = 1;
    }

    /* Preserve oauth credentials if they exist */
    if (oauth_access) {
        if (has_prev) fprintf(f, ",\n");
        fprintf(f, "  \"oauth\": {\n");
        fprintf(f, "    \"accessToken\": \"%s\"", oauth_access);

        if (oauth_refresh) {
            fprintf(f, ",\n    \"refreshToken\": \"%s\"", oauth_refresh);
        }
        if (oauth_expires) {
            fprintf(f, ",\n    \"expiresAt\": %s", oauth_expires);
        }
        if (oauth_scopes) {
            fprintf(f, ",\n    \"scopes\": \"%s\"", oauth_scopes);
        }
        fprintf(f, "\n  }");
        has_prev = 1;
    }

    fprintf(f, "\n}\n");

    fclose(f);

    free(oauth_access);
    free(oauth_refresh);
    free(oauth_scopes);
    free(oauth_expires);

    return 0;
}

void state_free(state_t* state) {
    if (!state) return;
    free(state->user_id);
    free(state->account_uuid);
    state->user_id = NULL;
    state->account_uuid = NULL;
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

int state_fetch_profile(state_t* state, const char* access_token) {
    if (!access_token) return -1;

    /* Skip if already have account_uuid */
    if (state->account_uuid) return 0;

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    response_t resp = {NULL, 0};
    struct curl_slist* headers = NULL;

    /* Build auth header */
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
    headers = curl_slist_append(headers, auth_header);

    char beta_header[64];
    snprintf(beta_header, sizeof(beta_header), "anthropic-beta: %s", OAUTH_BETA);
    headers = curl_slist_append(headers, beta_header);

    curl_easy_setopt(curl, CURLOPT_URL, PROFILE_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);

    int result = -1;
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200 && resp.data) {
            /* Extract account.uuid from response */
            char* uuid = json_get_string(resp.data, "account.uuid");
            if (uuid) {
                free(state->account_uuid);
                state->account_uuid = uuid;
                state_save(state);
                result = 0;
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(resp.data);

    return result;
}

char* state_build_metadata(const state_t* state, const char* session_id) {
    const char* user_id = state->user_id ? state->user_id : "";
    const char* account = state->account_uuid ? state->account_uuid : "";
    const char* session = session_id ? session_id : "";

    /* Format: user_{userId}_account_{accountUuid}_session_{sessionId} */
    size_t len = 6 + strlen(user_id) + 9 + strlen(account) + 9 + strlen(session) + 1;
    char* result = malloc(len);
    if (!result) return NULL;

    snprintf(result, len, "user_%s_account_%s_session_%s", user_id, account, session);
    return result;
}
