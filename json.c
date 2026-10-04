/*
 * json.c - Minimal JSON parser for Claude C client
 */

#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Skip whitespace */
static const char* skip_ws(const char* p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Skip a JSON value (string, number, object, array, true, false, null) */
static const char* skip_value(const char* p) {
    p = skip_ws(p);
    if (*p == '"') {
        /* String */
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p += 2;
            else p++;
        }
        if (*p == '"') p++;
    } else if (*p == '{') {
        /* Object */
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
    } else if (*p == '[') {
        /* Array */
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) p += 2;
                    else p++;
                }
            }
            if (*p) p++;
        }
    } else {
        /* Number, true, false, null */
        while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) {
            p++;
        }
    }
    return p;
}

/* Find a key in a JSON object, return pointer to value start */
static const char* find_key(const char* json, const char* key) {
    const char* p = skip_ws(json);
    if (*p != '{') return NULL;
    p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;

        /* Parse key */
        p++;
        const char* key_start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p += 2;
            else p++;
        }
        size_t key_len = p - key_start;
        if (*p == '"') p++;

        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);

        /* Check if this is the key we want */
        if (strlen(key) == key_len && strncmp(key_start, key, key_len) == 0) {
            return p;
        }

        /* Skip value */
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') p++;
    }

    return NULL;
}

/* Parse an escaped JSON string, returning newly allocated unescaped string */
static char* parse_string(const char* p) {
    if (*p != '"') return NULL;
    p++;

    /* First pass: calculate length */
    size_t len = 0;
    const char* s = p;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1]) {
            s += 2;
            len++;
        } else {
            s++;
            len++;
        }
    }

    char* result = malloc(len + 1);
    if (!result) return NULL;

    /* Second pass: copy and unescape */
    char* d = result;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': *d++ = '\n'; break;
                case 'r': *d++ = '\r'; break;
                case 't': *d++ = '\t'; break;
                case '\\': *d++ = '\\'; break;
                case '"': *d++ = '"'; break;
                case '/': *d++ = '/'; break;
                default: *d++ = *p; break;
            }
            p++;
        } else {
            *d++ = *p++;
        }
    }
    *d = '\0';

    return result;
}

char* json_get_string(const char* json, const char* key) {
    if (!json || !key) return NULL;

    /* Handle nested keys like "delta.text" */
    char* key_copy = strdup(key);
    if (!key_copy) return NULL;

    const char* current = json;
    char* token = strtok(key_copy, ".");

    while (token && current) {
        current = find_key(current, token);
        token = strtok(NULL, ".");
    }

    free(key_copy);

    if (!current) return NULL;
    return parse_string(current);
}

int json_string_equals(const char* json, const char* key, const char* value) {
    char* str = json_get_string(json, key);
    if (!str) return 0;
    int result = strcmp(str, value) == 0;
    free(str);
    return result;
}

long long json_get_number(const char* json, const char* key, int* found) {
    if (found) *found = 0;
    if (!json || !key) return 0;

    /* Handle nested keys like "delta.count" */
    char* key_copy = strdup(key);
    if (!key_copy) return 0;

    const char* current = json;
    char* token = strtok(key_copy, ".");

    while (token && current) {
        current = find_key(current, token);
        token = strtok(NULL, ".");
    }

    free(key_copy);

    if (!current) return 0;

    /* Skip whitespace and parse number */
    current = skip_ws(current);
    if (!*current || (!isdigit((unsigned char)*current) && *current != '-')) {
        return 0;
    }

    char* endptr;
    long long result = strtoll(current, &endptr, 10);
    if (endptr != current && found) {
        *found = 1;
    }
    return result;
}

/* Extract text content of first user message from a JSON request body or messages array.
 * Handles: {"messages":[{"role":"user","content":"text"},...]}
 * And:     {"messages":[{"role":"user","content":[{"type":"text","text":"..."},...]},... ]}
 * And:     [{"role":"user","content":"text"},...]  (bare messages array)
 * Returns newly allocated string or NULL. Caller must free().
 */
char* json_extract_first_user_text(const char* json) {
    if (!json) return NULL;

    const char* p = skip_ws(json);

    /* If it's an object, find "messages" key; if array, use directly */
    const char* arr;
    if (*p == '{') {
        arr = find_key(p, "messages");
        if (!arr) return NULL;
        arr = skip_ws(arr);
    } else if (*p == '[') {
        arr = p;
    } else {
        return NULL;
    }

    if (*arr != '[') return NULL;
    p = arr + 1;

    /* Iterate array elements */
    while (1) {
        p = skip_ws(p);
        if (!*p || *p == ']') break;
        if (*p != '{') { p = skip_value(p); goto next_msg; }

        /* Remember start of this object, find its extent */
        const char* obj_start = p;
        const char* obj_end = skip_value(p);
        size_t obj_len = obj_end - obj_start;

        /* Null-terminate a copy for safe parsing */
        char* obj = malloc(obj_len + 1);
        if (!obj) return NULL;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        /* Check role */
        char* role = json_get_string(obj, "role");
        if (!role || strcmp(role, "user") != 0) {
            free(role);
            free(obj);
            p = obj_end;
            goto next_msg;
        }
        free(role);

        /* Found first user message. Get content. */
        const char* content_ptr = find_key(obj, "content");
        if (!content_ptr) { free(obj); return NULL; }
        content_ptr = skip_ws(content_ptr);

        if (*content_ptr == '"') {
            /* String content */
            char* result = parse_string(content_ptr);
            free(obj);
            return result;
        }

        if (*content_ptr == '[') {
            /* Array content - find first {"type":"text","text":"..."} */
            const char* ap = content_ptr + 1;
            while (1) {
                ap = skip_ws(ap);
                if (!*ap || *ap == ']') break;
                if (*ap != '{') { ap = skip_value(ap); goto next_block; }

                const char* block_start = ap;
                const char* block_end = skip_value(ap);
                size_t block_len = block_end - block_start;

                char* block = malloc(block_len + 1);
                if (!block) { free(obj); return NULL; }
                memcpy(block, block_start, block_len);
                block[block_len] = '\0';

                if (json_string_equals(block, "type", "text")) {
                    char* text = json_get_string(block, "text");
                    free(block);
                    free(obj);
                    return text;
                }
                free(block);
                ap = block_end;

            next_block:
                ap = skip_ws(ap);
                if (*ap == ',') ap++;
            }
            free(obj);
            return NULL;
        }

        free(obj);
        return NULL;

    next_msg:
        p = skip_ws(p);
        if (*p == ',') p++;
    }

    return NULL;
}

char* json_escape_string(const char* str) {
    if (!str) return NULL;

    /* Calculate escaped length */
    size_t len = 0;
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '"':
            case '\\':
            case '\n':
            case '\r':
            case '\t':
                len += 2;
                break;
            default:
                if ((unsigned char)*p < 32) {
                    len += 6; /* \uXXXX */
                } else {
                    len++;
                }
        }
    }

    char* result = malloc(len + 1);
    if (!result) return NULL;

    char* d = result;
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '"': *d++ = '\\'; *d++ = '"'; break;
            case '\\': *d++ = '\\'; *d++ = '\\'; break;
            case '\n': *d++ = '\\'; *d++ = 'n'; break;
            case '\r': *d++ = '\\'; *d++ = 'r'; break;
            case '\t': *d++ = '\\'; *d++ = 't'; break;
            default:
                if ((unsigned char)*p < 32) {
                    d += sprintf(d, "\\u%04x", (unsigned char)*p);
                } else {
                    *d++ = *p;
                }
        }
    }
    *d = '\0';

    return result;
}

/* Add cache_control to the last content block of the last message.
 * Mechanics: locate the messages array, walk to the last element with the
 * depth-aware skip_value(), find its "content", then either splice the field
 * into the last block of an array, or promote a bare string to a text block.
 * The already-escaped message text is reused verbatim. Returns NULL (caller
 * keeps the original) on anything unexpected. See build_request_body() in
 * api.c for why this is placed here and how the caching model works. */
static const char CACHE_CONTROL_FIELD[] = ",\"cache_control\":{\"type\":\"ephemeral\"}";

char* json_add_cache_control_last_message(const char* messages_json) {
    if (!messages_json) return NULL;

    /* Locate the messages array: a bare array, or an object with "messages". */
    const char* p = skip_ws(messages_json);
    const char* arr = NULL;
    if (*p == '[') {
        arr = p;
    } else if (*p == '{') {
        const char* v = find_key(messages_json, "messages");
        if (!v) return NULL;
        v = skip_ws(v);
        if (*v != '[') return NULL;
        arr = v;
    } else {
        return NULL;
    }

    /* Walk to the last array element (the last message object). */
    const char* last_start = NULL;
    const char* last_end = NULL;
    p = arr + 1;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']' || *p == '\0') break;
        const char* es = p;
        const char* ee = skip_value(p);
        if (ee <= es) return NULL;
        last_start = es;
        last_end = ee;
        p = skip_ws(ee);
        if (*p == ',') p++;
    }
    (void)last_end;
    if (!last_start || *last_start != '{') return NULL;

    /* Find the last message's "content" value. */
    const char* content = find_key(last_start, "content");
    if (!content) return NULL;
    content = skip_ws(content);

    size_t in_len = strlen(messages_json);
    char* out = malloc(in_len + 128);
    if (!out) return NULL;
    size_t o = 0;

    if (*content == '[') {
        /* Array content: splice cache_control into the last block object. */
        const char* blk_end = NULL;
        const char* cp = content + 1;
        for (;;) {
            cp = skip_ws(cp);
            if (*cp == ']' || *cp == '\0') break;
            const char* bs = cp;
            const char* be = skip_value(cp);
            if (be <= bs) { free(out); return NULL; }
            blk_end = be;
            cp = skip_ws(be);
            if (*cp == ',') cp++;
        }
        if (!blk_end || *(blk_end - 1) != '}') { free(out); return NULL; }
        size_t head = (size_t)((blk_end - 1) - messages_json);  /* up to last '}' */
        memcpy(out + o, messages_json, head); o += head;
        size_t cclen = sizeof(CACHE_CONTROL_FIELD) - 1;
        memcpy(out + o, CACHE_CONTROL_FIELD, cclen); o += cclen;
        memcpy(out + o, messages_json + head, in_len - head); o += in_len - head;
    } else if (*content == '"') {
        /* String content: promote to a one-element text block carrying cache_control. */
        const char* str_end = skip_value(content);   /* one past closing '"' */
        if (str_end <= content) { free(out); return NULL; }
        static const char OPEN[]  = "[{\"type\":\"text\",\"text\":";
        static const char CLOSE[] = ",\"cache_control\":{\"type\":\"ephemeral\"}}]";
        size_t head = (size_t)(content - messages_json);
        size_t slit = (size_t)(str_end - content);          /* the "..." literal */
        memcpy(out + o, messages_json, head); o += head;
        memcpy(out + o, OPEN, sizeof(OPEN) - 1); o += sizeof(OPEN) - 1;
        memcpy(out + o, content, slit); o += slit;
        memcpy(out + o, CLOSE, sizeof(CLOSE) - 1); o += sizeof(CLOSE) - 1;
        size_t tail_off = (size_t)(str_end - messages_json);
        memcpy(out + o, str_end, in_len - tail_off); o += in_len - tail_off;
    } else {
        free(out);
        return NULL;
    }

    out[o] = '\0';
    return out;
}

/* Add cache_control to the last block of the "system" array of a full request
 * body (or a bare system array). Used by raw request mode. Same splice
 * technique as the array-content branch above; system blocks are always
 * objects. Returns NULL (caller keeps the original) on anything unexpected. */
char* json_add_cache_control_last_system_block(const char* body) {
    if (!body) return NULL;

    const char* p = skip_ws(body);
    const char* arr = NULL;
    if (*p == '[') {
        arr = p;
    } else if (*p == '{') {
        const char* v = find_key(body, "system");
        if (!v) return NULL;
        v = skip_ws(v);
        if (*v != '[') return NULL;
        arr = v;
    } else {
        return NULL;
    }

    /* Walk to the last block object in the system array. */
    const char* blk_end = NULL;
    const char* q = arr + 1;
    for (;;) {
        q = skip_ws(q);
        if (*q == ']' || *q == '\0') break;
        const char* bs = q;
        const char* be = skip_value(q);
        if (be <= bs) return NULL;
        blk_end = be;
        q = skip_ws(be);
        if (*q == ',') q++;
    }
    if (!blk_end || *(blk_end - 1) != '}') return NULL;  /* empty or non-object */

    size_t in_len = strlen(body);
    char* out = malloc(in_len + 64);
    if (!out) return NULL;
    size_t head = (size_t)((blk_end - 1) - body);   /* up to last block's '}' */
    size_t cclen = sizeof(CACHE_CONTROL_FIELD) - 1;
    memcpy(out, body, head);
    memcpy(out + head, CACHE_CONTROL_FIELD, cclen);
    memcpy(out + head + cclen, body + head, in_len - head);
    out[in_len + cclen] = '\0';
    return out;
}

char* json_get_raw(const char* json, const char* key) {
    if (!json || !key) return NULL;
    char* keys = strdup(key);
    if (!keys) return NULL;
    const char* p = json;
    char* save = NULL;
    for (char* part = strtok_r(keys, ".", &save); part && p;
         part = strtok_r(NULL, ".", &save)) p = find_key(p, part);
    free(keys);
    if (!p) return NULL;
    return strndup(p, (size_t)(skip_value(p) - p));
}

char* json_set_property(const char* json, const char* key, const char* value) {
    if (!json || !key) return NULL;
    const char* p = skip_ws(json);
    if (*p++ != '{') return NULL;
    char* escaped = json_escape_string(key);
    if (!escaped) return NULL;
    size_t size = strlen(json) + strlen(escaped) + (value ? strlen(value) : 0) + 8;
    char* result = malloc(size);
    if (!result) { free(escaped); return NULL; }
    char* out = result;
    *out++ = '{';
    int count = 0;
    while (*(p = skip_ws(p)) && *p != '}') {
        if (*p != '"') goto fail;
        const char* start = p;
        char* name = parse_string(p);
        if (!name) goto fail;
        p = skip_ws(skip_value(p));
        if (*p++ != ':') { free(name); goto fail; }
        const char* end = skip_value(p);
        if (end == p) { free(name); goto fail; }
        if (strcmp(name, key) != 0) {
            if (count++) *out++ = ',';
            memcpy(out, start, (size_t)(end - start));
            out += end - start;
        }
        free(name);
        p = skip_ws(end);
        if (*p == ',') p++;
        else if (*p != '}') goto fail;
    }
    if (*p != '}' || *skip_ws(p + 1)) goto fail;
    if (value) {
        if (count) *out++ = ',';
        out += sprintf(out, "\"%s\":%s", escaped, value);
    }
    *out++ = '}'; *out = 0;
    free(escaped);
    return result;
fail:
    free(escaped); free(result); return NULL;
}

char* json_get_scopes(const char* json, const char* key) {
    char* raw = json_get_raw(json, key);
    if (!raw) return NULL;
    if (*raw == '"') { char* value = parse_string(raw); free(raw); return value; }
    if (*raw != '[') { free(raw); return NULL; }
    char* result = calloc(strlen(raw) + 1, 1);
    if (!result) { free(raw); return NULL; }
    const char* p = raw + 1;
    while (*(p = skip_ws(p)) && *p != ']') {
        char* scope = parse_string(p);
        if (!scope) { free(result); free(raw); return NULL; }
        if (*result) strcat(result, " ");
        strcat(result, scope); free(scope);
        p = skip_ws(skip_value(p));
        if (*p == ',') p++;
        else if (*p != ']') { free(result); free(raw); return NULL; }
    }
    free(raw);
    return result;
}

char* json_fix_assistant_prefill(const char* json) {
    const char* array = find_key(json, "messages");
    if (!array || *array != '[') return strdup(json);
    const char* p = array + 1;
    const char* last = NULL;
    const char* end = NULL;
    while (*(p = skip_ws(p)) && *p != ']') {
        last = p; end = skip_value(p);
        if (end == p) return NULL;
        p = skip_ws(end);
        if (*p == ',') p++;
        else if (*p != ']') return NULL;
    }
    if (!last || !json_string_equals(last, "role", "assistant")) return strdup(json);
    const char* role = find_key(last, "role");
    const char* after = skip_value(role);
    char* result = malloc(strlen(json) + 1);
    if (!result) return NULL;
    size_t prefix = (size_t)(role - json);
    memcpy(result, json, prefix);
    strcpy(result + prefix, "\"user\"");
    strcpy(result + prefix + 6, after);
    return result;
}
