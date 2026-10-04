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
#include <strings.h>
#include <stdio.h>
#include <sys/utsname.h>

/* Billing header constants */
#define BILLING_SALT "59cf53e54c78"
#define BILLING_VERSION CLAUDE_CODE_VERSION

/* X-Stainless-* telemetry headers sent by the official SDK on /v1/messages.
 * Static values mirror the bundled Anthropic SDK (v0.112.1); OS/Arch are
 * derived from uname() the way the SDK maps process.platform/process.arch.
 * NOTE: the SDK reports Runtime "node" even under Bun (Bun isn't detected). */
#define STAINLESS_LANG "js"
#define STAINLESS_PKG_VERSION CLAUDE_CODE_SDK_VERSION
#define STAINLESS_RUNTIME "node"
#define STAINLESS_RUNTIME_VERSION "v24.9.0"
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
static struct curl_slist* append_stainless_headers(struct curl_slist* headers, int retry) {
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
    headers = curl_slist_append(headers, retry ? "X-Stainless-Retry-Count: 1" : "X-Stainless-Retry-Count: 0");
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
    long status;
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
    /* A 401 body must never reach stdout before credential recovery. */
    if (ctx->status == 401) return bytes;

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

/* One header builder for both normal and raw requests. */
static struct curl_slist* message_headers(const auth_t* auth, const state_t* state,
                                          const char* body, const char* request_id, int retry) {
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "anthropic-version: " API_VERSION);
    size_t auth_len = strlen(auth->value) + 32;
    char* credential = malloc(auth_len);
    if (!credential) { curl_slist_free_all(headers); return NULL; }
    snprintf(credential, auth_len, auth->type == AUTH_OAUTH ? "Authorization: Bearer %s" : "x-api-key: %s", auth->value);
    headers = curl_slist_append(headers, credential);
    free(credential);
    char beta[512] = "anthropic-beta: ";
    if (auth->type == AUTH_OAUTH) {
        headers = append_stainless_headers(headers, retry);
        headers = curl_slist_append(headers, "User-Agent: " USER_AGENT);
        headers = curl_slist_append(headers, "x-app: cli");
        headers = curl_slist_append(headers, "anthropic-dangerous-direct-browser-access: true");
        char id[256];
        snprintf(id, sizeof(id), "x-client-request-id: %s", request_id);
        headers = curl_slist_append(headers, id);
        if (state->session_id) {
            snprintf(id, sizeof(id), "X-Claude-Code-Session-Id: %s", state->session_id);
            headers = curl_slist_append(headers, id);
        }
        strcat(beta, OAUTH_BETA);
        char* model = json_get_string(body, "model");
        if (!model || !strstr(model, "haiku")) strcat(beta, "," CLAUDE_CODE_BETA);
        free(model);
    }
    char* thinking = json_get_string(body, "thinking.type");
    char* effort = json_get_string(body, "output_config.effort");
    char* format = json_get_string(body, "output_config.format.type");
    const char* extras[3] = {
        thinking && strcmp(thinking, "disabled") != 0 ? "interleaved-thinking-2025-05-14" : NULL,
        effort ? "effort-2025-11-24" : NULL,
        format && strcmp(format, "json_schema") == 0 ? "structured-outputs-2025-12-15" : NULL
    };
    for (int i = 0; i < 3; i++) if (extras[i]) {
        if (beta[strlen(beta) - 1] != ' ') strcat(beta, ",");
        strcat(beta, extras[i]);
    }
    if (beta[strlen(beta) - 1] != ' ') headers = curl_slist_append(headers, beta);
    free(thinking); free(effort); free(format);
    return headers;
}

/* Rate-limit headers of the final response: anthropic-ratelimit-unified-*
 * (prefix stripped, e.g. "7d_oi-utilization") plus retry-after, in arrival
 * order. Bucket names are passed through uninterpreted. */
#define RL_PREFIX "anthropic-ratelimit-unified-"
#define RL_MAX 32
typedef struct {
    int count;
    char key[RL_MAX][64];
    char value[RL_MAX][96];
} ratelimit_info_t;

typedef struct {
    stream_context_t* stream;   /* NULL in non-streaming mode */
    ratelimit_info_t* rl;
} header_context_t;

/* Copy at most cap-1 chars of src[0..len), dropping CR/LF and turning spaces
 * into '_' so the RATELIMIT line stays one space-separated record. */
static void rl_copy(char* dst, size_t cap, const char* src, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        char c = src[i];
        if (c == '\r' || c == '\n') continue;
        dst[n++] = (c == ' ' || c == '\t') ? '_' : c;
    }
    dst[n] = 0;
}

static void rl_collect(ratelimit_info_t* rl, const char* line, size_t len) {
    const char* key;
    size_t prefix;
    if (len > strlen(RL_PREFIX) && strncasecmp(line, RL_PREFIX, strlen(RL_PREFIX)) == 0) {
        prefix = strlen(RL_PREFIX);
    } else if (len > 12 && strncasecmp(line, "retry-after:", 12) == 0) {
        prefix = 0;
    } else {
        return;
    }
    if (rl->count >= RL_MAX) return;
    const char* colon = memchr(line, ':', len);
    if (!colon) return;
    key = line + prefix;
    const char* val = colon + 1;
    while (val < line + len && (*val == ' ' || *val == '\t')) val++;
    rl_copy(rl->key[rl->count], sizeof(rl->key[0]), key, (size_t)(colon - key));
    rl_copy(rl->value[rl->count], sizeof(rl->value[0]), val, (size_t)(line + len - val));
    rl->count++;
}

static size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    header_context_t* ctx = userdata;
    size_t bytes = size * nmemb;
    char line[128];
    size_t len = bytes < sizeof(line) - 1 ? bytes : sizeof(line) - 1;
    memcpy(line, ptr, len); line[len] = 0;
    long status;
    if (sscanf(line, "HTTP/%*s %ld", &status) == 1) {
        if (ctx->stream) ctx->stream->status = status;
        /* New response (e.g. after a 401 retry): report only the final one. */
        if (ctx->rl) ctx->rl->count = 0;
    } else if (ctx->rl) {
        rl_collect(ctx->rl, ptr, bytes);
    }
    return bytes;
}

/* One line on stderr: RATELIMIT http=429 [error_type=...] key=value ... */
static void rl_report(const ratelimit_info_t* rl, long http_code, const char* error_type) {
    fprintf(stderr, "RATELIMIT http=%ld", http_code);
    if (error_type) fprintf(stderr, " error_type=%s", error_type);
    for (int i = 0; i < rl->count; i++)
        fprintf(stderr, " %s=%s", rl->key[i], rl->value[i]);
    fputc('\n', stderr);
}

/* Report the response's rate-limit state: always on errors that carry
 * headers or are a 429, on success only with --ratelimit/--verbose.
 * Returns the exit status for a non-200 response. */
static int rl_finish(const ratelimit_info_t* rl, long http_code, const char* error_type) {
    if (http_code == 200) {
        if ((report_ratelimit || verbose) && rl->count > 0) rl_report(rl, http_code, NULL);
        return 0;
    }
    if (rl->count > 0 || http_code == 429) rl_report(rl, http_code, error_type);
    return http_code == 429 ? EXIT_RATE_LIMITED : 1;
}

/* Retry only a definitive OAuth 401, never a partial successful response. */
static CURLcode perform_message(CURL* curl, struct curl_slist** headers, auth_t* auth,
                                 const state_t* state, const char* body,
                                 stream_context_t* stream, response_buffer_t* buffer,
                                 ratelimit_info_t* rl) {
    char request_id[37];
    generate_uuid_v4(request_id);
    header_context_t hctx = { stream, rl };
    curl_easy_setopt(curl, CURLOPT_URL, auth->type == AUTH_OAUTH
        ? API_BASE_URL API_MESSAGES_PATH "?beta=true" : API_BASE_URL API_MESSAGES_PATH);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);
    for (int retry = 0; retry < 2; retry++) {
        curl_slist_free_all(*headers);
        *headers = message_headers(auth, state, body, request_id, retry);
        if (!*headers) return CURLE_OUT_OF_MEMORY;
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
        CURLcode result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (result != CURLE_OK || status != 401 || retry || auth_recover_unauthorized(auth) != 0) return result;
        if (buffer) { buffer->size = 0; buffer->buffer[0] = 0; }
    }
    return CURLE_OK;
}

/* Apply the model-specific parts of Tavern's 2.1.281 request update. Raw
 * requests retain tool schemas, output format, signatures and explicit effort. */
static int model_family(const char* model, const char* family) {
    size_t n = strlen(family);
    return model && strncmp(model, family, n) == 0 && (model[n] == 0 || model[n] == '-');
}

static int replace_property(char** body, const char* key, const char* value) {
    char* next = json_set_property(*body, key, value);
    if (!next) return -1;
    free(*body); *body = next; return 0;
}

static char* prepare_model_request(const char* input) {
    char* body = strdup(input);
    char* model = json_get_string(input, "model");
    char* thinking = json_get_string(input, "thinking.type");
    int opus5 = model_family(model, "claude-opus-5");
    int sonnet5 = model_family(model, "claude-sonnet-5");
    int fable = model_family(model, "claude-fable-5");
    int mythos = model_family(model, "claude-mythos-5");
    int opus47 = model_family(model, "claude-opus-4-7");
    int opus48 = model_family(model, "claude-opus-4-8");
    int adaptive = opus5 || sonnet5 || fable || mythos || opus47 || opus48
        || model_family(model, "claude-opus-4-6") || model_family(model, "claude-sonnet-4-6");
    int required = model_family(model, "claude-opus-5-5") || fable || model_family(model, "claude-mythos-5-1");
    int xhigh = opus5 || sonnet5 || fable || opus47 || opus48 || model_family(model, "claude-mythos-5-1");
    if (!body) goto done;
    if (adaptive && (!thinking || (required && strcmp(thinking, "adaptive") != 0))) {
        if (replace_property(&body, "thinking", "{\"type\":\"adaptive\",\"display\":\"omitted\"}") != 0) goto fail;
    }
    int active = json_string_equals(body, "thinking.type", "adaptive") || json_string_equals(body, "thinking.type", "enabled");
    if (active || opus5 || sonnet5 || fable || mythos || opus47 || opus48) {
        if (replace_property(&body, "temperature", NULL) || replace_property(&body, "top_p", NULL)
            || replace_property(&body, "top_k", NULL)) goto fail;
    }
    if (active && (json_string_equals(body, "tool_choice.type", "any") || json_string_equals(body, "tool_choice.type", "tool"))) {
        if (replace_property(&body, "tool_choice", "{\"type\":\"auto\"}")) goto fail;
    }
    if (active || model_family(model, "claude-opus-4-6")) {
        char* fixed = json_fix_assistant_prefill(body);
        if (!fixed) goto fail;
        free(body); body = fixed;
    }
    if (!xhigh && json_string_equals(body, "output_config.effort", "xhigh")) {
        char* config = json_get_raw(body, "output_config");
        char* adjusted = config ? json_set_property(config, "effort", "\"high\"") : NULL;
        free(config);
        if (!adjusted) goto fail;
        int result = replace_property(&body, "output_config", adjusted);
        free(adjusted);
        if (result) goto fail;
    }
    goto done;
fail:
    free(body); body = NULL;
done:
    free(model); free(thinking); return body;
}

/* Compute billing header string from first user message text.
 * Returns allocated string like:
 *   x-anthropic-billing-header: cc_version=2.1.281.abc; cc_entrypoint=cli; cch=00000;
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
    ratelimit_info_t rl = {0};
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

    char* prepared = prepare_model_request(body);
    if (!prepared) { fprintf(stderr, "Error: Invalid Claude request body\n"); goto cleanup; }
    free(body); body = prepared;
    DEBUG("Request body: %s\n", body);

    /* Configure curl */
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
    CURLcode res = perform_message(curl, &headers, &auth, &state, body, stream ? &stream_ctx : NULL, stream ? NULL : &response_buf, &rl);

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
        /* error.type is only recoverable from the buffered (non-stream) body. */
        char* err_type = stream ? NULL : json_get_string(response_buf.buffer, "error.type");
        result = rl_finish(&rl, http_code, err_type);
        free(err_type);
        goto cleanup;
    }
    rl_finish(&rl, http_code, NULL);

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
    char* text = json_extract_first_user_text(request_body);
    char* billing = compute_billing_header(text);
    char* system = json_get_raw(request_body, "system");
    char* system_text = json_get_string(request_body, "system");
    char* escaped = system_text ? json_escape_string(system_text) : NULL;
    char* metadata = json_get_raw(request_body, "metadata");
    char* body = NULL;
    free(text); free(system_text);
    if (!billing) goto cleanup;
    size_t len = strlen(billing) + (system ? strlen(system) : 0)
        + (escaped ? strlen(escaped) : 0) + 256;
    char* blocks = malloc(len);
    if (!blocks) goto cleanup;
    int n = snprintf(blocks, len, "[{\"type\":\"text\",\"text\":\"%s\"},{\"type\":\"text\",\"text\":\"%s\"}", billing, IDENTITY_AGENT);
    if (escaped) {
        snprintf(blocks + n, len - n, ",{\"type\":\"text\",\"text\":\"%s\"}]", escaped);
    } else if (system && *system == '[') {
        const char* start = system + 1;
        while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t') start++;
        snprintf(blocks + n, len - n, "%s%s", *start == ']' ? "" : ",", start);
    } else if (!system) {
        strcpy(blocks + n, "]");
    } else { free(blocks); goto cleanup; }
    body = json_set_property(request_body, "system", blocks);
    free(blocks);
    if (body) {
        /* state_build_metadata returns a JSON-escaped string, ready to quote. */
        size_t size = (metadata_user_id ? strlen(metadata_user_id) : 8) + 3;
        char* user = malloc(size);
        if (!user) { free(body); body = NULL; goto cleanup; }
        snprintf(user, size, "\"%s\"", metadata_user_id ? metadata_user_id : "claude-c");
        char* merged = json_set_property(metadata ? metadata : "{}", "user_id", user);
        free(user);
        if (!merged || replace_property(&body, "metadata", merged)) { free(body); body = NULL; }
        free(merged);
    }
cleanup:
    free(billing); free(system); free(escaped); free(metadata);
    return body;
}

int api_send_raw_request(const char* request_body, int json_output, int cache, FILE* output) {
    int result = 1;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    char* body = NULL;
    char* metadata_user_id = NULL;
    response_buffer_t response_buf = {0};
    ratelimit_info_t rl = {0};
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

    if (replace_property(&body, "stream", "false")) goto cleanup;

    /* Optional prompt caching: mark the last message and last system block of
       the injected body (see build_request_body for the caching model). Each
       helper returns a new copy or NULL (keep previous) on parse failure. */
    if (cache) {
        char* cm = json_add_cache_control_last_message(body);
        if (cm) { free(body); body = cm; }
        char* cs = json_add_cache_control_last_system_block(body);
        if (cs) { free(body); body = cs; }
    }

    char* prepared = prepare_model_request(body);
    if (!prepared) { fprintf(stderr, "Error: Invalid Claude request body\n"); goto cleanup; }
    free(body); body = prepared;
    DEBUG("Request body: %s\n", body);

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

    /* Configure curl */
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);  /* 10 minute timeout */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

    /* Perform request */
    CURLcode res = perform_message(curl, &headers, &auth, &state, body, NULL, &response_buf, &rl);

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
        char* err_type = json_get_string(response_buf.buffer, "error.type");
        result = rl_finish(&rl, http_code, err_type);
        free(err_type);
        goto cleanup;
    }
    rl_finish(&rl, http_code, NULL);

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
