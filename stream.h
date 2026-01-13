/*
 * stream.h - SSE stream parser for Claude C client
 */

#ifndef STREAM_H
#define STREAM_H

#include <stddef.h>

/* Stream parser state */
typedef struct {
    char* buffer;           /* Accumulated data buffer */
    size_t buffer_len;      /* Current data length */
    size_t buffer_cap;      /* Buffer capacity */
    char* current_event;    /* Current event type */
    int message_started;    /* Have we seen message_start? */
    int finished;           /* Have we seen message_stop? */
    char* error_message;    /* Error message if any */
} stream_parser_t;

/* Callback for text deltas */
typedef void (*text_callback_t)(const char* text, void* userdata);

/* Initialize stream parser */
int stream_parser_init(stream_parser_t* parser);

/* Free stream parser resources */
void stream_parser_free(stream_parser_t* parser);

/* Feed data to parser, calls callback for each text delta
 * Returns 0 on success, -1 on error
 */
int stream_parser_feed(stream_parser_t* parser, const char* data, size_t len,
                       text_callback_t callback, void* userdata);

/* Check if stream is finished */
int stream_parser_finished(stream_parser_t* parser);

/* Get error message (NULL if no error) */
const char* stream_parser_error(stream_parser_t* parser);

#endif /* STREAM_H */
