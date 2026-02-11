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

/* Extract a number value from JSON by key path
 * Returns the number or 0 if not found
 * Sets *found to 1 if found, 0 if not (can be NULL if you don't care)
 */
long long json_get_number(const char* json, const char* key, int* found);

/* Check if a JSON object has a specific string value for a key */
int json_string_equals(const char* json, const char* key, const char* value);

/* Extract text content of first user message from request JSON or messages array.
 * Returns newly allocated string or NULL if not found.
 * Caller must free() the result.
 */
char* json_extract_first_user_text(const char* json);

/* Escape a string for JSON embedding
 * Returns newly allocated string, caller must free()
 */
char* json_escape_string(const char* str);

#endif /* JSON_H */
