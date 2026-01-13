/*
 * stream.c - SSE stream parser for Claude C client
 */

#include "stream.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define INITIAL_BUFFER_SIZE 4096

int stream_parser_init(stream_parser_t* parser) {
    if (!parser) return -1;

    parser->buffer = malloc(INITIAL_BUFFER_SIZE);
    if (!parser->buffer) return -1;

    parser->buffer[0] = '\0';
    parser->buffer_len = 0;
    parser->buffer_cap = INITIAL_BUFFER_SIZE;
    parser->current_event = NULL;
    parser->message_started = 0;
    parser->finished = 0;
    parser->error_message = NULL;

    return 0;
}

void stream_parser_free(stream_parser_t* parser) {
    if (!parser) return;

    free(parser->buffer);
    free(parser->current_event);
    free(parser->error_message);

    parser->buffer = NULL;
    parser->current_event = NULL;
    parser->error_message = NULL;
}

/* Grow buffer if needed */
static int ensure_capacity(stream_parser_t* parser, size_t additional) {
    size_t needed = parser->buffer_len + additional + 1;
    if (needed <= parser->buffer_cap) return 0;

    size_t new_cap = parser->buffer_cap * 2;
    while (new_cap < needed) new_cap *= 2;

    char* new_buf = realloc(parser->buffer, new_cap);
    if (!new_buf) return -1;

    parser->buffer = new_buf;
    parser->buffer_cap = new_cap;
    return 0;
}

/* Process a single line */
static int process_line(stream_parser_t* parser, const char* line, size_t len,
                        text_callback_t callback, void* userdata) {
    /* Empty line - end of event */
    if (len == 0) {
        free(parser->current_event);
        parser->current_event = NULL;
        return 0;
    }

    /* Event line */
    if (len > 7 && strncmp(line, "event: ", 7) == 0) {
        free(parser->current_event);
        parser->current_event = strndup(line + 7, len - 7);
        return 0;
    }

    /* Data line */
    if (len > 6 && strncmp(line, "data: ", 6) == 0) {
        const char* json = line + 6;

        /* Check for error */
        char* error_type = json_get_string(json, "error.type");
        if (error_type) {
            char* error_msg = json_get_string(json, "error.message");
            if (error_msg) {
                parser->error_message = error_msg;
            } else {
                parser->error_message = strdup(error_type);
            }
            free(error_type);
            return -1;
        }

        /* Get event type from JSON if not from SSE event line */
        char* type = json_get_string(json, "type");
        if (!type) return 0;

        if (strcmp(type, "message_start") == 0) {
            parser->message_started = 1;
        }
        else if (strcmp(type, "message_stop") == 0) {
            parser->finished = 1;
        }
        else if (strcmp(type, "content_block_delta") == 0) {
            /* Check delta type */
            char* delta_type = json_get_string(json, "delta.type");
            if (delta_type) {
                if (strcmp(delta_type, "text_delta") == 0) {
                    char* text = json_get_string(json, "delta.text");
                    if (text && callback) {
                        callback(text, userdata);
                    }
                    free(text);
                }
                else if (strcmp(delta_type, "thinking_delta") == 0) {
                    /* Could handle thinking output here if needed */
                }
                free(delta_type);
            }
        }
        else if (strcmp(type, "error") == 0) {
            char* error_msg = json_get_string(json, "error.message");
            if (error_msg) {
                parser->error_message = error_msg;
            }
            free(type);
            return -1;
        }

        free(type);
    }

    return 0;
}

int stream_parser_feed(stream_parser_t* parser, const char* data, size_t len,
                       text_callback_t callback, void* userdata) {
    if (!parser || !data) return -1;

    /* Append to buffer */
    if (ensure_capacity(parser, len) != 0) return -1;
    memcpy(parser->buffer + parser->buffer_len, data, len);
    parser->buffer_len += len;
    parser->buffer[parser->buffer_len] = '\0';

    /* Process complete lines */
    char* line_start = parser->buffer;
    char* newline;

    while ((newline = strchr(line_start, '\n')) != NULL) {
        size_t line_len = newline - line_start;

        /* Strip \r if present */
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }

        /* Null-terminate for processing */
        char saved = line_start[line_len];
        line_start[line_len] = '\0';

        int result = process_line(parser, line_start, line_len, callback, userdata);

        line_start[line_len] = saved;

        if (result != 0) return result;

        line_start = newline + 1;
    }

    /* Move remaining data to start of buffer */
    size_t remaining = parser->buffer_len - (line_start - parser->buffer);
    if (remaining > 0 && line_start != parser->buffer) {
        memmove(parser->buffer, line_start, remaining);
    }
    parser->buffer_len = remaining;
    parser->buffer[remaining] = '\0';

    return 0;
}

int stream_parser_finished(stream_parser_t* parser) {
    return parser ? parser->finished : 0;
}

const char* stream_parser_error(stream_parser_t* parser) {
    return parser ? parser->error_message : NULL;
}
