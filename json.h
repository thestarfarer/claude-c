/*
 * json.h - Minimal JSON parser for Claude C client
 * Only handles what we need: objects, arrays, strings, extraction
 */

#ifndef JSON_H
#define JSON_H

#include <stddef.h>

/* Extract a string value from JSON by key path (e.g., "delta.text")
 * Returns newly allocated string or NULL if not found
 * Caller must free() the result
 */
char* json_get_string(const char* json, const char* key);

/* Check if a JSON object has a specific string value for a key */
int json_string_equals(const char* json, const char* key, const char* value);

/* Escape a string for JSON embedding
 * Returns newly allocated string, caller must free()
 */
char* json_escape_string(const char* str);

#endif /* JSON_H */
