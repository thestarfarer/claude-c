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
#include <sys/utsname.h>

/* Billing header constants */
#define BILLING_SALT "59cf53e54c78"
#define BILLING_VERSION "2.1.206"

/* X-Stainless-* telemetry headers sent by the official SDK on /v1/messages.
 * Static values mirror the bundled Anthropic SDK (v0.94.0); OS/Arch are
 * derived from uname() the way the SDK maps process.platform/process.arch.
 * NOTE: the SDK reports Runtime "node" even under Bun (Bun isn't detected). */
#define STAINLESS_LANG "js"
#define STAINLESS_PKG_VERSION "0.94.0"
#define STAINLESS_RUNTIME "node"
#define STAINLESS_RUNTIME_VERSION "v24.9.0"
#define STAINLESS_RETRY_COUNT "0"   /* claude-c does not auto-retry */
#define STAINLESS_TIMEOUT "600"     /* matches the 600s curl timeout below */

/* Map uname sysname -> SDK X-Stainless-OS value */
static const char* stainless_os(const char* sysname) {
    if (!sysname) return "Unknown";
    if (strcmp(sysname, "Linux") == 0) return "Linux";
    if (strcmp(sysname, "Darwin") == 0) return "MacOS";
    if (strncmp(sysname, "CYGWIN", 6) == 0 ||
        strncmp(sysname, "MINGW", 5) == 0 ||
        strncmp(sysname, "MSYS", 4) == 0) return "Windows";
    if (strcmp(sysname, "FreeBSD") == 0) return "FreeBSD";
    if (strcmp(sysname, "OpenBSD") == 0) return "OpenBSD";
    return sysname;
}

/* Map uname machine -> SDK X-Stainless-Arch value */
static const char* stainless_arch(const char* machine) {
    if (!machine) return "unknown";
    if (strcmp(machine, "x86_64") == 0 || strcmp(machine, "amd64") == 0) return "x64";
    if (strcmp(machine, "aarch64") == 0 || strcmp(machine, "arm64") == 0) return "arm64";
    if (strcmp(machine, "i386") == 0 || strcmp(machine, "i686") == 0) return "x32";
    return machine;
}

/* Append the SDK's X-Stainless-* telemetry headers (only for /v1/messages) */
static struct curl_slist* append_stainless_headers(struct curl_slist* headers) {
    struct utsname u;
    const char* os = "Linux";
    const char* arch = "x64";
    if (uname(&u) == 0) {
        os = stainless_os(u.sysname);
        arch = stainless_arch(u.machine);
    }
    char buf[128];
    headers = curl_slist_append(headers, "X-Stainless-Lang: " STAINLESS_LANG);
    headers = curl_slist_append(headers, "X-Stainless-Package-Version: " STAINLESS_PKG_VERSION);
    headers = curl_slist_append(headers, "X-Stainless-Runtime: " STAINLESS_RUNTIME);
    headers = curl_slist_append(headers, "X-Stainless-Runtime-Version: " STAINLESS_RUNTIME_VERSION);
    headers = curl_slist_append(headers, "X-Stainless-Retry-Count: " STAINLESS_RETRY_COUNT);
    headers = curl_slist_append(headers, "X-Stainless-Timeout: " STAINLESS_TIMEOUT);
    snprintf(buf, sizeof(buf), "X-Stainless-OS: %s", os);
    headers = curl_slist_append(headers, buf);
    snprintf(buf, sizeof(buf), "X-Stainless-Arch: %s", arch);
    headers = curl_slist_append(headers, buf);
    return headers;
}

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
 *   x-anthropic-billing-header: cc_version=2.1.206.abc; cc_entrypoint=cli; cch=00000;
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
/* Prompt caching (opt-in via --cache) -- how it works.
 *
 * Anthropic prompt caching is an ABSOLUTE PREFIX cache: a "cache_control"
 * breakpoint caches everything from the start of the request up to that block,
 * and a later request gets a HIT only if that whole prefix is byte-for-byte
 * identical to the cached one. When `cache` is set we place two breakpoints:
 *   1. the LAST block of the system array (below), and
 *   2. the last content block of the last message (json_add_cache_control_last_message).
 *
 * For an APPEND-ONLY conversation -- one transcript that only ever grows, e.g.
 * two agents talking turn by turn -- each turn's prefix (system + full history)
 * is a superset of the previous turn's. So the server reads the old prefix
 * cheaply (usage.cache_read_input_tokens) and writes only the new delta
 * (usage.cache_creation_input_tokens): the cache extends incrementally instead
 * of being rebuilt each turn. The billing block is deliberately kept FIRST and
 * uncached -- it is derived from the first user message, so it is stable within
 * a conversation and never invalidates the prefix. TTL is a 5-minute sliding
 * window, refreshed on every hit; we use {type:"ephemeral"} with no ttl/scope,
 * so no beta header is needed and it works on first-party, Bedrock, and Vertex.
 *
 * SCOPE: this is the append-only (Case A) variant ONLY. If a message already
 * inside the cached prefix is mutated (e.g. a drifting appendage injected at a
 * fixed depth-from-end), the prefix diverges there and caching breaks
 * downstream -- covering that needs a second "stable prefix" breakpoint (what
 * Claude Code's ZRy does) and is intentionally NOT implemented here.
 */
static char* build_request_body(const char* model, const char* system_prompt,
                                 const char* messages_json, int max_tokens,
                                 int stream, int cache, const char* metadata_user_id,
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

    /* Prompt caching: add cache_control to the last message (breakpoint #2) and
       to the last system block below (breakpoint #1). On any messages-parse
       failure, cached_messages is NULL and we fall back to the original. */
    char* cached_messages = NULL;
    if (cache && messages_json) {
        cached_messages = json_add_cache_control_last_message(messages_json);
    }
    const char* messages_out = cached_messages ? cached_messages
                             : (messages_json ? messages_json : "[]");
    const char* cache_cc = cache ? ",\"cache_control\":{\"type\":\"ephemeral\"}" : "";

    /* Calculate buffer size (generous) */
    size_t prompt_len = escaped_prompt ? strlen(escaped_prompt) : 0;
    size_t messages_len = strlen(messages_out);
    size_t metadata_len = metadata_user_id ? strlen(metadata_user_id) : 8;
    size_t billing_len = strlen(billing_block);
    size_t buf_size = 2048 + prompt_len + messages_len + metadata_len + billing_len;

    char* body = malloc(buf_size);
    if (!body) {
        free(escaped_prompt);
        free(cached_messages);
        return NULL;
    }

    const char* stream_str = stream ? "true" : "false";
    const char* user_id = metadata_user_id ? metadata_user_id : "claude-c";

    /* Build JSON - billing header first, then identity, then user prompt.
       The last system block carries cache_cc (empty unless --cache). */
    if (escaped_prompt) {
        snprintf(body, buf_size,
            "{"
            "\"model\":\"%s\","
            "\"max_tokens\":%d,"
            "\"stream\":%s,"
            "\"system\":["
                "%s"
                "{\"type\":\"text\",\"text\":\"%s\"},"
                "{\"type\":\"text\",\"text\":\"%s\"%s}"
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
            cache_cc,
            messages_out,
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
                "{\"type\":\"text\",\"text\":\"%s\"%s}"
            "],"
            "\"messages\":%s,"
            "\"metadata\":{\"user_id\":\"%s\"}"
            "}",
            model,
            max_tokens,
            stream_str,
            billing_block,
            IDENTITY_AGENT,
            cache_cc,
            messages_out,
            user_id
        );
    }

    free(cached_messages);
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
                     int stream, int cache, FILE* output) {
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

    /* Fetch profile if OAuth and no cached account_uuid */
    if (auth.type == AUTH_OAUTH && !state.account_uuid) {
        state_fetch_profile(&state, auth.value);
    }

    /* Build metadata user_id as JSON */
    metadata_user_id = state_build_metadata(&state);

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
    /* Accept: SSE when streaming, JSON otherwise (matches official SDK) */
    headers = curl_slist_append(headers,
        stream ? "Accept: text/event-stream" : "Accept: application/json");
    headers = append_stainless_headers(headers);

    char version_header[64];
    snprintf(version_header, sizeof(version_header), "anthropic-version: %s", API_VERSION);
    headers = curl_slist_append(headers, version_header);

    char ua_header[128];
    snprintf(ua_header, sizeof(ua_header), "User-Agent: %s", USER_AGENT);
    headers = curl_slist_append(headers, ua_header);

    headers = curl_slist_append(headers, "x-app: cli");

    /* Add session ID header */
    if (state.session_id) {
        char session_header[128];
        snprintf(session_header, sizeof(session_header), "X-Claude-Code-Session-Id: %s", state.session_id);
        headers = curl_slist_append(headers, session_header);
    }

    /* Add auth header */
    char auth_header[512];
    if (auth.type == AUTH_API_KEY) {
        snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", auth.value);
        headers = curl_slist_append(headers, auth_header);
    } else if (auth.type == AUTH_OAUTH) {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", auth.value);
        headers = curl_slist_append(headers, auth_header);

        char beta_header[64];
        snprintf(beta_header, sizeof(beta_header), "anthropic-beta: %s", MESSAGES_BETA);
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
        cache,
        metadata_user_id,
        first_user_text
    );
    free(first_user_text);

    if (!body) {
        fprintf(stderr, "Error: Failed to build request body\n");
        goto cleanup;
    }

    DEBUG("Request body: %s\n", body);

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

    /* Fetch profile if OAuth and no cached account_uuid */
    if (auth.type == AUTH_OAUTH && !state.account_uuid) {
        state_fetch_profile(&state, auth.value);
    }

    /* Build metadata user_id as JSON */
    metadata_user_id = state_build_metadata(&state);

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
    /* Raw request mode is always non-streaming */
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = append_stainless_headers(headers);

    char version_header[64];
    snprintf(version_header, sizeof(version_header), "anthropic-version: %s", API_VERSION);
    headers = curl_slist_append(headers, version_header);

    char ua_header[128];
    snprintf(ua_header, sizeof(ua_header), "User-Agent: %s", USER_AGENT);
    headers = curl_slist_append(headers, ua_header);

    headers = curl_slist_append(headers, "x-app: cli");

    /* Add session ID header */
    if (state.session_id) {
        char session_header[128];
        snprintf(session_header, sizeof(session_header), "X-Claude-Code-Session-Id: %s", state.session_id);
        headers = curl_slist_append(headers, session_header);
    }

    /* Add auth header */
    char auth_header[512];
    if (auth.type == AUTH_API_KEY) {
        snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", auth.value);
        headers = curl_slist_append(headers, auth_header);
    } else if (auth.type == AUTH_OAUTH) {
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", auth.value);
        headers = curl_slist_append(headers, auth_header);

        char beta_header[64];
        snprintf(beta_header, sizeof(beta_header), "anthropic-beta: %s", MESSAGES_BETA);
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
