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
