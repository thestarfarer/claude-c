/*
 * api.h - API request handling for Claude C client
 */

#ifndef API_H
#define API_H

#include <stdio.h>

/* Verbose output flag - set via -v/--verbose */
extern int verbose;
#define DEBUG(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

/* API configuration */
#define API_BASE_URL "https://api.anthropic.com"
#define API_MESSAGES_PATH "/v1/messages"
#define API_VERSION "2023-06-01"
#define OAUTH_BETA "oauth-2025-04-20"
#define USER_AGENT "claude-cli/1.0.0-c (external, cli)"

/* Identity strings - must match server whitelist exactly */
#define IDENTITY_AGENT "You are a Claude agent, built on Anthropic's Claude Agent SDK."
#define IDENTITY_CLI "You are Claude Code, Anthropic's official CLI for Claude."

/* Default settings */
#define DEFAULT_MODEL "claude-sonnet-4-5-20250929"
#define DEFAULT_MAX_TOKENS 16384

/* Send a message to the Claude API
 * - model: Model ID (e.g., "claude-sonnet-4-5-20250929")
 * - system_prompt: Custom system prompt (can be NULL)
 * - messages_json: Messages array as JSON string
 * - max_tokens: Maximum output tokens
 * - stream: If non-zero, use streaming mode
 * - output: Stream text output to this file (usually stdout)
 *
 * Returns 0 on success, non-zero on error
 */
int api_send_message(
    const char* model,
    const char* system_prompt,
    const char* messages_json,
    int max_tokens,
    int stream,
    FILE* output
);

/* Send a raw request body to the Claude API
 * - request_body: Full request JSON (model, messages, tools, etc.)
 *                 Identity string and metadata will be injected automatically
 * - json_output: If non-zero, output raw API response JSON
 *                If zero, extract and output text content only
 * - output: Output to this file (usually stdout)
 *
 * Returns 0 on success, non-zero on error
 */
int api_send_raw_request(
    const char* request_body,
    int json_output,
    FILE* output
);

#endif /* API_H */
