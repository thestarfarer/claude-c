/*
 * api.c - API request handling for Claude C client
 */

#include "api.h"
#include "auth.h"
#include "json.h"
#include "stream.h"
#include "state.h"
#include "oauth.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Billing header constants */
#define BILLING_SALT "59cf53e54c78"
#define BILLING_VERSION "2.1.39"

/* Context for streaming curl callbacks */
typedef struct {
    stream_parser_t parser;
    FILE* output;
    int error;
} stream_context_t;

/* Context for non-streaming curl callbacks */
typedef struct {
    char* buffer;
    size_t size;
    size_t capacity;
} response_buffer_t;

/* Callback for text deltas - print to output */
static void text_callback(const char* text, void* userdata) {
    stream_context_t* ctx = (stream_context_t*)userdata;
    if (ctx && ctx->output && text) {
        fputs(text, ctx->output);
        fflush(ctx->output);
    }
}

/* Curl write callback for streaming */
static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    stream_context_t* ctx = (stream_context_t*)userdata;
    size_t bytes = size * nmemb;

    if (!ctx) return 0;

    int result = stream_parser_feed(&ctx->parser, ptr, bytes, text_callback, ctx);
    if (result != 0) {
        ctx->error = 1;
    }

    return bytes;
}

/* Curl write callback for non-streaming */
static size_t buffer_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    response_buffer_t* buf = (response_buffer_t*)userdata;
    size_t bytes = size * nmemb;

    if (!buf) return 0;

    /* Grow buffer if needed */
    if (buf->size + bytes + 1 > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + bytes + 1) {
            new_cap = buf->size + bytes + 1;
        }
        char* new_buf = realloc(buf->buffer, new_cap);
        if (!new_buf) return 0;
        buf->buffer = new_buf;
        buf->capacity = new_cap;
    }

    memcpy(buf->buffer + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->buffer[buf->size] = '\0';

    return bytes;
}

/* Compute billing header string from first user message text.
 * Returns allocated string like:
 *   x-anthropic-billing-header: cc_version=2.1.39.abc; cc_entrypoint=cli; cch=00000;
 * Caller must free(). Returns NULL on failure.
 */
static char* compute_billing_header(const char* first_user_text) {
    /* Extract chars at positions 4, 7, 20 (or '0' for out-of-bounds) */
    size_t len = first_user_text ? strlen(first_user_text) : 0;
    char c4  = (len > 4)  ? first_user_text[4]  : '0';
    char c7  = (len > 7)  ? first_user_text[7]  : '0';
    char c20 = (len > 20) ? first_user_text[20] : '0';

    /* Build hash input: salt + chars + version */
    char input[64];
    snprintf(input, sizeof(input), "%s%c%c%c%s", BILLING_SALT, c4, c7, c20, BILLING_VERSION);

    /* SHA256 */
    size_t hash_len;
    unsigned char* hash = sha256(input, &hash_len);
    if (!hash) return NULL;

    /* Take first 3 hex chars */
    char hash_hex[4];
    snprintf(hash_hex, sizeof(hash_hex), "%02x%01x", hash[0], (hash[1] >> 4) & 0x0f);
    free(hash);

    /* Build billing header string */
    char* result = malloc(128);
    if (!result) return NULL;

    snprintf(result, 128,
        "x-anthropic-billing-header: cc_version=%s.%s; cc_entrypoint=cli; cch=00000;",
        BILLING_VERSION, hash_hex);

    return result;
}

/* Build request body JSON */
static char* build_request_body(const char* model, const char* system_prompt,
                                 const char* messages_json, int max_tokens,
                                 int stream, const char* metadata_user_id,
                                 const char* first_user_text) {
    /* Escape system prompt if provided */
    char* escaped_prompt = NULL;
    if (system_prompt) {
        escaped_prompt = json_escape_string(system_prompt);
        if (!escaped_prompt) return NULL;
    }

    /* Compute billing header */
    char* billing = compute_billing_header(first_user_text);

    /* Build billing block for system array */
    char billing_block[256] = "";
    if (billing) {
        snprintf(billing_block, sizeof(billing_block),
            "{\"type\":\"text\",\"text\":\"%s\"},", billing);
        free(billing);
        billing = NULL;
    }

    /* Calculate buffer size (generous) */
    size_t prompt_len = escaped_prompt ? strlen(escaped_prompt) : 0;
    size_t messages_len = messages_json ? strlen(messages_json) : 2;
    size_t metadata_len = metadata_user_id ? strlen(metadata_user_id) : 8;
    size_t billing_len = strlen(billing_block);
    size_t buf_size = 2048 + prompt_len + messages_len + metadata_len + billing_len;

    char* body = malloc(buf_size);
    if (!body) {
        free(escaped_prompt);
        return NULL;
    }

    const char* stream_str = stream ? "true" : "false";
    const char* user_id = metadata_user_id ? metadata_user_id : "claude-c";

    /* Build JSON - billing header first, then identity, then user prompt */
    if (escaped_prompt) {
        snprintf(body, buf_size,
            "{"
            "\"model\":\"%s\","
            "\"max_tokens\":%d,"
            "\"stream\":%s,"
            "\"system\":["
                "%s"
                "{\"type\":\"text\",\"text\":\"%s\"},"
                "{\"type\":\"text\",\"text\":\"%s\"}"
            "],"
            "\"messages\":%s,"
            "\"metadata\":{\"user_id\":\"%s\"}"
            "}",
            model,
            max_tokens,
            stream_str,
            billing_block,
            IDENTITY_AGENT,
            escaped_prompt,
            messages_json ? messages_json : "[]",
            user_id
        );
        free(escaped_prompt);
    } else {
        snprintf(body, buf_size,
            "{"
            "\"model\":\"%s\","
            "\"max_tokens\":%d,"
            "\"stream\":%s,"
            "\"system\":["
                "%s"
                "{\"type\":\"text\",\"text\":\"%s\"}"
            "],"
            "\"messages\":%s,"
            "\"metadata\":{\"user_id\":\"%s\"}"
            "}",
            model,
            max_tokens,
            stream_str,
            billing_block,
            IDENTITY_AGENT,
            messages_json ? messages_json : "[]",
            user_id
        );
    }

    return body;
}

/* Extract text content from non-streaming response */
static char* extract_response_text(const char* json) {
    /* Response format: {"content":[{"type":"text","text":"..."},...], ...} */
    /* We need to find content array and extract text from first text block */

    /* Simple approach: find "content":[ then find {"type":"text","text":" */
    const char* content = strstr(json, "\"content\":");
    if (!content) return NULL;

    content = strchr(content, '[');
    if (!content) return NULL;
    content++;

    /* Look for text blocks and concatenate them */
    size_t result_cap = 4096;
    size_t result_len = 0;
    char* result = malloc(result_cap);
    if (!result) return NULL;
    result[0] = '\0';

    const char* p = content;
    while (*p) {
        /* Find next object */
        while (*p && *p != '{' && *p != ']') p++;
        if (*p == ']' || !*p) break;

        /* Check if it's a text type */
        const char* obj_start = p;

        /* Find end of this object (handle nesting) */
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) p += 2;
                    else p++;
                }
            }
            if (*p) p++;
        }

        /* Now extract from obj_start to p */
        size_t obj_len = p - obj_start;
        char* obj = malloc(obj_len + 1);
        if (!obj) continue;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        /* Check type and extract text */
        if (json_string_equals(obj, "type", "text")) {
            char* text = json_get_string(obj, "text");
            if (text) {
                size_t text_len = strlen(text);
                if (result_len + text_len + 1 > result_cap) {
                    result_cap = result_cap * 2 + text_len;
                    char* new_result = realloc(result, result_cap);
                    if (!new_result) {
                        free(text);
                        free(obj);
                        free(result);
                        return NULL;
                    }
                    result = new_result;
                }
                memcpy(result + result_len, text, text_len);
                result_len += text_len;
                result[result_len] = '\0';
                free(text);
            }
        }
        free(obj);

        /* Skip comma if present */
        while (*p && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }

    if (result_len == 0) {
        free(result);
        return NULL;
    }

    return result;
}

int api_send_message(const char* model, const char* system_prompt,
                     const char* messages_json, int max_tokens,
                     int stream, FILE* output) {
    int result = 1;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    char* body = NULL;
    char* metadata_user_id = NULL;
    stream_context_t stream_ctx = {0};
    response_buffer_t response_buf = {0};
    state_t state = {0};

    /* Load authentication */
    auth_t auth = auth_load();
    if (auth.type == AUTH_NONE) {
        fprintf(stderr, "Error: No authentication found.\n");
        fprintf(stderr, "Set ANTHROPIC_API_KEY or run 'claude' to authenticate.\n");
        return 1;
    }

    /* Load persistent state and build metadata */
    state = state_load();
    char session_id[37];
    generate_uuid_v4(session_id);

    /* Fetch profile if OAuth and no cached account_uuid */
    if (auth.type == AUTH_OAUTH && !state.account_uuid) {
        state_fetch_profile(&state, auth.value);
    }

    /* Build metadata user_id: user_{id}_account_{uuid}_session_{uuid} */
    metadata_user_id = state_build_metadata(&state, session_id);

    /* Initialize curl */
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: Failed to initialize curl\n");
        auth_free(&auth);
        state_free(&state);
        free(metadata_user_id);
        return 1;
    }

    /* Initialize context based on mode */
    if (stream) {
        if (stream_parser_init(&stream_ctx.parser) != 0) {
            fprintf(stderr, "Error: Failed to initialize parser\n");
            curl_easy_cleanup(curl);
            auth_free(&auth);
            return 1;
        }
        stream_ctx.output = output;
        stream_ctx.error = 0;
    } else {
        response_buf.capacity = 4096;
        response_buf.buffer = malloc(response_buf.capacity);
        if (!response_buf.buffer) {
            fprintf(stderr, "Error: Failed to allocate response buffer\n");
            curl_easy_cleanup(curl);
            auth_free(&auth);
            return 1;
        }
        response_buf.buffer[0] = '\0';
        response_buf.size = 0;
    }

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", API_BASE_URL, API_MESSAGES_PATH);

    /* Build headers */
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char version_header[64];
    snprintf(version_header, sizeof(version_header), "anthropic-version: %s", API_VERSION);
    headers = curl_slist_append(headers, version_header);

    char ua_header[128];
    snprintf(ua_header, sizeof(ua_header), "User-Agent: %s", USER_AGENT);
    headers = curl_slist_append(headers, ua_header);

    headers = curl_slist_append(headers, "x-app: cli");

    /* Add auth header */
    char auth_header[512];
    if (auth.type == AUTH_API_KEY) {
        snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", auth.value);
        headers = curl_slist_append(headers, auth_header);
    } else if (auth.type == AUTH_OAUTH) {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", auth.value);
        headers = curl_slist_append(headers, auth_header);

        char beta_header[64];
        snprintf(beta_header, sizeof(beta_header), "anthropic-beta: %s", OAUTH_BETA);
        headers = curl_slist_append(headers, beta_header);
    }

    /* Extract first user message text for billing header */
    char* first_user_text = json_extract_first_user_text(messages_json);

    /* Build request body */
    body = build_request_body(
        model ? model : DEFAULT_MODEL,
        system_prompt,
        messages_json,
        max_tokens > 0 ? max_tokens : DEFAULT_MAX_TOKENS,
        stream,
        metadata_user_id,
        first_user_text
    );
    free(first_user_text);

    if (!body) {
        fprintf(stderr, "Error: Failed to build request body\n");
        goto cleanup;
    }

    /* Configure curl */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);  /* 10 minute timeout */

    if (stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_ctx);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    }

    /* Perform request */
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "Error: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }

    /* Check HTTP status */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        if (stream) {
            const char* err = stream_parser_error(&stream_ctx.parser);
            if (err) {
                fprintf(stderr, "API Error (%ld): %s\n", http_code, err);
            } else {
                fprintf(stderr, "API Error: HTTP %ld\n", http_code);
            }
        } else {
            /* Try to extract error message from response */
            char* err_msg = json_get_string(response_buf.buffer, "error.message");
            if (err_msg) {
                fprintf(stderr, "API Error (%ld): %s\n", http_code, err_msg);
                free(err_msg);
            } else {
                fprintf(stderr, "API Error: HTTP %ld\n", http_code);
            }
        }
        goto cleanup;
    }

    /* Process response */
    if (stream) {
        /* Check for stream errors */
        if (stream_ctx.error || stream_parser_error(&stream_ctx.parser)) {
            const char* err = stream_parser_error(&stream_ctx.parser);
            fprintf(stderr, "Stream Error: %s\n", err ? err : "Unknown error");
            goto cleanup;
        }
    } else {
        /* Extract and print text from response */
        char* text = extract_response_text(response_buf.buffer);
        if (text) {
            fputs(text, output);
            free(text);
        } else {
            fprintf(stderr, "Error: Failed to parse response\n");
            goto cleanup;
        }
    }

    /* Ensure output ends with newline */
    fprintf(output, "\n");

    result = 0;

cleanup:
    if (stream) {
        stream_parser_free(&stream_ctx.parser);
    } else {
        free(response_buf.buffer);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    free(metadata_user_id);
    state_free(&state);
    auth_free(&auth);

    return result;
}

/* Inject identity string and metadata into request body JSON */
static char* inject_identity_and_metadata(const char* request_body, const char* metadata_user_id) {
    /*
     * We need to:
     * 1. Find "system" array and prepend identity block, OR add "system" array if missing
     * 2. Add "metadata" field before the closing }
     *
     * Input:  {"model":"...", "system":[...], "messages":[...], ...}
     * Output: {"model":"...", "system":[{identity}, ...], "messages":[...], ..., "metadata":{...}}
     */

    size_t request_len = strlen(request_body);
    const char* identity_block = "{\"type\":\"text\",\"text\":\"" IDENTITY_AGENT "\"}";
    size_t identity_len = strlen(identity_block);

    /* Compute billing header from first user message */
    char* first_user_text = json_extract_first_user_text(request_body);
    char* billing = compute_billing_header(first_user_text);
    free(first_user_text);

    char billing_block[256] = "";
    if (billing) {
        snprintf(billing_block, sizeof(billing_block),
            "{\"type\":\"text\",\"text\":\"%s\"},", billing);
        free(billing);
    }
    size_t billing_len = strlen(billing_block);

    /* Calculate size for metadata */
    char metadata_json[512];
    snprintf(metadata_json, sizeof(metadata_json),
        "\"metadata\":{\"user_id\":\"%s\"}",
        metadata_user_id ? metadata_user_id : "claude-c");
    size_t metadata_len = strlen(metadata_json);

    /* Allocate generous buffer */
    size_t buf_size = request_len + billing_len + identity_len + metadata_len + 256;
    char* result = malloc(buf_size);
    if (!result) return NULL;

    /* Find "system": in the request */
    const char* system_pos = strstr(request_body, "\"system\"");
    const char* system_array_start = NULL;

    if (system_pos) {
        /* Find the [ after "system": */
        const char* p = system_pos + 8; /* skip "system" */
        while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n')) p++;
        if (*p == '[') {
            system_array_start = p;
        }
    }

    char* out = result;

    if (system_array_start) {
        /* Copy up to and including [ */
        size_t prefix_len = system_array_start - request_body + 1;
        memcpy(out, request_body, prefix_len);
        out += prefix_len;

        /* Insert billing block (before identity) */
        if (billing_len > 0) {
            memcpy(out, billing_block, billing_len);
            out += billing_len;
        }

        /* Insert identity block */
        memcpy(out, identity_block, identity_len);
        out += identity_len;

        /* Check if system array is empty or has content */
        const char* after_bracket = system_array_start + 1;
        while (*after_bracket && (*after_bracket == ' ' || *after_bracket == '\t' || *after_bracket == '\n')) {
            after_bracket++;
        }
        if (*after_bracket && *after_bracket != ']') {
            /* Non-empty array, add comma */
            *out++ = ',';
        }

        /* Copy the rest until the final } */
        const char* rest = system_array_start + 1;
        /* Find the final } of the JSON object */
        const char* final_brace = request_body + request_len - 1;
        while (final_brace > rest && *final_brace != '}') final_brace--;

        if (final_brace > rest) {
            size_t rest_len = final_brace - rest;
            memcpy(out, rest, rest_len);
            out += rest_len;

            /* Add metadata */
            *out++ = ',';
            memcpy(out, metadata_json, metadata_len);
            out += metadata_len;

            /* Close with } */
            *out++ = '}';
            *out = '\0';
        } else {
            /* Malformed JSON, just copy rest */
            strcpy(out, rest);
        }
    } else {
        /* No "system" field found, need to add it after the opening { */
        const char* open_brace = strchr(request_body, '{');
        if (!open_brace) {
            free(result);
            return NULL;
        }

        /* Copy { */
        *out++ = '{';

        /* Add system array with billing + identity */
        out += sprintf(out, "\"system\":[%s%s],", billing_block, identity_block);

        /* Copy rest of original content (skip the {) */
        const char* content_start = open_brace + 1;
        while (*content_start && (*content_start == ' ' || *content_start == '\t' || *content_start == '\n')) {
            content_start++;
        }

        /* Find final } */
        const char* final_brace = request_body + request_len - 1;
        while (final_brace > content_start && *final_brace != '}') final_brace--;

        if (final_brace > content_start) {
            size_t content_len = final_brace - content_start;
            memcpy(out, content_start, content_len);
            out += content_len;

            /* Add metadata */
            *out++ = ',';
            memcpy(out, metadata_json, metadata_len);
            out += metadata_len;

            /* Close with } */
            *out++ = '}';
            *out = '\0';
        } else {
            free(result);
            return NULL;
        }
    }

    return result;
}

int api_send_raw_request(const char* request_body, int json_output, FILE* output) {
    int result = 1;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    char* body = NULL;
    char* metadata_user_id = NULL;
    response_buffer_t response_buf = {0};
    state_t state = {0};

    /* Load authentication */
    auth_t auth = auth_load();
    if (auth.type == AUTH_NONE) {
        fprintf(stderr, "Error: No authentication found.\n");
        fprintf(stderr, "Set ANTHROPIC_API_KEY or run 'claude' to authenticate.\n");
        return 1;
    }

    /* Load persistent state and build metadata */
    state = state_load();
    char session_id[37];
    generate_uuid_v4(session_id);

    /* Fetch profile if OAuth and no cached account_uuid */
    if (auth.type == AUTH_OAUTH && !state.account_uuid) {
        state_fetch_profile(&state, auth.value);
    }

    /* Build metadata user_id */
    metadata_user_id = state_build_metadata(&state, session_id);

    /* Inject identity string, billing header, and metadata into request body */
    body = inject_identity_and_metadata(request_body, metadata_user_id);
    if (!body) {
        fprintf(stderr, "Error: Failed to process request body\n");
        auth_free(&auth);
        state_free(&state);
        free(metadata_user_id);
        return 1;
    }

    /* Initialize curl */
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: Failed to initialize curl\n");
        auth_free(&auth);
        state_free(&state);
        free(metadata_user_id);
        free(body);
        return 1;
    }

    /* Initialize response buffer (always non-streaming for raw request mode) */
    response_buf.capacity = 4096;
    response_buf.buffer = malloc(response_buf.capacity);
    if (!response_buf.buffer) {
        fprintf(stderr, "Error: Failed to allocate response buffer\n");
        curl_easy_cleanup(curl);
        auth_free(&auth);
        state_free(&state);
        free(metadata_user_id);
        free(body);
        return 1;
    }
    response_buf.buffer[0] = '\0';
    response_buf.size = 0;

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "%s%s", API_BASE_URL, API_MESSAGES_PATH);

    /* Build headers */
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char version_header[64];
    snprintf(version_header, sizeof(version_header), "anthropic-version: %s", API_VERSION);
    headers = curl_slist_append(headers, version_header);

    char ua_header[128];
    snprintf(ua_header, sizeof(ua_header), "User-Agent: %s", USER_AGENT);
    headers = curl_slist_append(headers, ua_header);

    headers = curl_slist_append(headers, "x-app: cli");

    /* Add auth header */
    char auth_header[512];
    if (auth.type == AUTH_API_KEY) {
        snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", auth.value);
        headers = curl_slist_append(headers, auth_header);
    } else if (auth.type == AUTH_OAUTH) {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", auth.value);
        headers = curl_slist_append(headers, auth_header);

        char beta_header[64];
        snprintf(beta_header, sizeof(beta_header), "anthropic-beta: %s", OAUTH_BETA);
        headers = curl_slist_append(headers, beta_header);
    }

    /* Configure curl */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);  /* 10 minute timeout */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

    /* Perform request */
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "Error: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }

    /* Check HTTP status */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        /* Try to extract error message from response */
        char* err_msg = json_get_string(response_buf.buffer, "error.message");
        if (err_msg) {
            fprintf(stderr, "API Error (%ld): %s\n", http_code, err_msg);
            free(err_msg);
        } else {
            fprintf(stderr, "API Error: HTTP %ld\n", http_code);
        }
        goto cleanup;
    }

    /* Process response */
    if (json_output) {
        /* Output raw JSON response */
        fputs(response_buf.buffer, output);
    } else {
        /* Extract and print text from response */
        char* text = extract_response_text(response_buf.buffer);
        if (text) {
            fputs(text, output);
            free(text);
        } else {
            fprintf(stderr, "Error: Failed to parse response\n");
            goto cleanup;
        }
        /* Ensure output ends with newline */
        fprintf(output, "\n");
    }

    result = 0;

cleanup:
    free(response_buf.buffer);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    free(metadata_user_id);
    state_free(&state);
    auth_free(&auth);

    return result;
}
