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
