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

/* Return a newly-allocated copy of a messages array (bare array, or object with
 * a "messages" key) with `"cache_control":{"type":"ephemeral"}` added to the
 * last content block of the LAST message. If that message's content is a bare
 * string it is promoted to a one-element text block array so the marker has a
 * block to attach to. Returns NULL on any parse failure (caller should then
 * fall back to the original messages unchanged). Caller must free() a non-NULL
 * result.
 */
char* json_add_cache_control_last_message(const char* messages_json);

/* Return a newly-allocated copy of a full request body (or bare system array)
 * with `"cache_control":{"type":"ephemeral"}` added to the LAST block of the
 * "system" array. Returns NULL on any parse failure. Caller must free() a
 * non-NULL result. Used to cache the system prefix in raw request mode, where
 * the client-built templates in build_request_body() don't apply.
 */
char* json_add_cache_control_last_system_block(const char* body);

/* Copy a JSON value by key path without decoding its contents. */
char* json_get_raw(const char* json, const char* key);

/* Replace/add a top-level property, or remove it when value is NULL.
 * Value is already serialized JSON. Unrelated values remain verbatim. */
char* json_set_property(const char* json, const char* key, const char* value);

/* Convert a trailing assistant prefill to a user message for thinking models. */
char* json_fix_assistant_prefill(const char* json);

/* OAuth scopes may be stored as a string or an array of strings. */
char* json_get_scopes(const char* json, const char* key);

#endif /* JSON_H */
