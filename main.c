/*
 * main.c - Claude C client entry point
 *
 * Minimal C implementation of Claude Code API client.
 * Supports --print mode for single message queries.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <curl/curl.h>
#include "api.h"
#include "json.h"
#include "image.h"
#include "oauth.h"

#define VERSION "1.2.0"
#define MAX_IMAGES 16

/* Global verbose flag */
int verbose = 0;

static struct option long_options[] = {
    {"print",         no_argument,       0, 'p'},
    {"stream",        no_argument,       0, 'S'},
    {"system-prompt", required_argument, 0, 's'},
    {"messages",      required_argument, 0, 'm'},
    {"model",         required_argument, 0, 'M'},
    {"max-tokens",    required_argument, 0, 't'},
    {"image",         required_argument, 0, 'i'},
    {"request",       required_argument, 0, 'r'},
    {"json-output",   no_argument,       0, 'J'},
    {"login",         no_argument,       0, 'L'},
    {"verbose",       no_argument,       0, 1},
    {"help",          no_argument,       0, 'h'},
    {"version",       no_argument,       0, 'v'},
    {0, 0, 0, 0}
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [TEXT]\n"
        "\n"
        "Minimal Claude API client in C.\n"
        "\n"
        "Options:\n"
        "  -p, --print              Single message mode (non-interactive)\n"
        "  -S, --stream             Stream response (show output as it arrives)\n"
        "  -s, --system-prompt STR  Custom system prompt\n"
        "  -m, --messages JSON      Messages array or @file.json\n"
        "  -i, --image PATH         Attach image file (can use multiple times)\n"
        "  -M, --model ID           Model ID (default: %s)\n"
        "  -t, --max-tokens N       Max output tokens (default: %d)\n"
        "  -r, --request FILE       Full request body JSON file (@file.json)\n"
        "  -J, --json-output        Output raw API response JSON\n"
        "  -L, --login              Authenticate with Claude (OAuth)\n"
        "      --verbose            Verbose output (show debug info)\n"
        "  -h, --help               Show this help\n"
        "  -v, --version            Show version\n"
        "\n"
        "Examples:\n"
        "  # Simple query\n"
        "  %s -p 'Hello, Claude!'\n"
        "\n"
        "  # With image\n"
        "  %s -p -i photo.jpg 'What is in this image?'\n"
        "\n"
        "  # Multiple images\n"
        "  %s -p -i img1.png -i img2.png 'Compare these images'\n"
        "\n"
        "  # With streaming output\n"
        "  %s -p --stream 'Tell me a story'\n"
        "\n"
        "  # From stdin\n"
        "  echo 'Hello' | %s -p -s 'You are helpful'\n"
        "\n"
        "  # Raw API format (for multi-turn)\n"
        "  %s -p -m '[{\"role\":\"user\",\"content\":\"Hello\"}]'\n"
        "\n"
        "  # Multi-turn from file (for large payloads)\n"
        "  %s -p -m @conversation.json\n"
        "\n"
        "Multi-turn JSON format with images:\n"
        "  [{\"role\":\"user\",\"content\":[\n"
        "    {\"type\":\"text\",\"text\":\"What is this?\"},\n"
        "    {\"type\":\"image\",\"source\":{\n"
        "      \"type\":\"base64\",\n"
        "      \"media_type\":\"image/png\",\n"
        "      \"data\":\"<base64-encoded-data>\"\n"
        "    }}\n"
        "  ]}]\n"
        "\n"
        "Supported image formats: PNG, JPEG, GIF, WEBP (max ~3.7 MB)\n"
        "\n"
        "Authentication:\n"
        "  Run '%s --login' to authenticate, or set ANTHROPIC_API_KEY\n"
        "\n",
        prog, DEFAULT_MODEL, DEFAULT_MAX_TOKENS, prog, prog, prog, prog, prog, prog, prog, prog
    );
}

static void print_version(void) {
    printf("claude-c version %s\n", VERSION);
}

/* Read file into a string */
static char* read_file_contents(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file: %s\n", path);
        return NULL;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fprintf(stderr, "Error: Empty or invalid file: %s\n", path);
        fclose(f);
        return NULL;
    }

    char* buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);

    buf[read] = '\0';
    return buf;
}

/* Read stdin into a string */
static char* read_stdin(void) {
    if (isatty(STDIN_FILENO)) {
        return NULL;  /* Don't block on interactive stdin */
    }

    size_t cap = 4096;
    size_t len = 0;
    char* buf = malloc(cap);
    if (!buf) return NULL;

    int c;
    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char* new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';

    /* Trim trailing whitespace */
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
                       buf[len-1] == ' ' || buf[len-1] == '\t')) {
        buf[--len] = '\0';
    }

    if (len == 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

/* Build messages JSON from text content (simple string) */
static char* build_simple_messages(const char* text) {
    char* escaped = json_escape_string(text);
    if (!escaped) return NULL;

    size_t len = strlen(escaped) + 64;
    char* json = malloc(len);
    if (!json) {
        free(escaped);
        return NULL;
    }

    snprintf(json, len, "[{\"role\":\"user\",\"content\":\"%s\"}]", escaped);
    free(escaped);

    return json;
}

/* Build messages JSON with text and images */
static char* build_messages_with_images(const char* text, image_t** images, int image_count) {
    /* Escape the text */
    char* escaped_text = json_escape_string(text);
    if (!escaped_text) return NULL;

    /* Calculate total size needed */
    size_t total_size = 128 + strlen(escaped_text);  /* Base JSON structure + text */

    /* Add size for each image JSON */
    char** image_jsons = malloc(image_count * sizeof(char*));
    if (!image_jsons) {
        free(escaped_text);
        return NULL;
    }

    for (int i = 0; i < image_count; i++) {
        image_jsons[i] = image_to_json(images[i]);
        if (!image_jsons[i]) {
            for (int j = 0; j < i; j++) free(image_jsons[j]);
            free(image_jsons);
            free(escaped_text);
            return NULL;
        }
        total_size += strlen(image_jsons[i]) + 2;  /* +2 for comma and space */
    }

    /* Build the JSON */
    char* json = malloc(total_size);
    if (!json) {
        for (int i = 0; i < image_count; i++) free(image_jsons[i]);
        free(image_jsons);
        free(escaped_text);
        return NULL;
    }

    /* Start with: [{"role":"user","content":[ */
    char* p = json;
    p += sprintf(p, "[{\"role\":\"user\",\"content\":[");

    /* Add text block first */
    p += sprintf(p, "{\"type\":\"text\",\"text\":\"%s\"}", escaped_text);
    free(escaped_text);

    /* Add image blocks */
    for (int i = 0; i < image_count; i++) {
        p += sprintf(p, ",%s", image_jsons[i]);
        free(image_jsons[i]);
    }
    free(image_jsons);

    /* Close: ]}] */
    sprintf(p, "]}]");

    return json;
}

int main(int argc, char** argv) {
    int print_mode = 0;
    int stream_mode = 0;
    char* system_prompt = NULL;
    char* messages = NULL;
    char* model = NULL;
    int max_tokens = 0;
    int messages_allocated = 0;

    /* Raw request mode */
    char* request_body = NULL;
    int request_body_allocated = 0;
    int json_output = 0;

    /* Image handling */
    char* image_paths[MAX_IMAGES];
    int image_count = 0;
    image_t* images[MAX_IMAGES];

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "pSs:m:M:t:i:r:JLhv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p':
                print_mode = 1;
                break;
            case 'S':
                stream_mode = 1;
                break;
            case 's':
                system_prompt = optarg;
                break;
            case 'm':
                if (optarg[0] == '@') {
                    /* Read from file */
                    messages = read_file_contents(optarg + 1);
                    if (!messages) return 1;
                    messages_allocated = 1;
                } else {
                    messages = optarg;
                }
                break;
            case 'M':
                model = optarg;
                break;
            case 't':
                max_tokens = atoi(optarg);
                break;
            case 'i':
                if (image_count >= MAX_IMAGES) {
                    fprintf(stderr, "Error: Too many images (max %d)\n", MAX_IMAGES);
                    return 1;
                }
                image_paths[image_count++] = optarg;
                break;
            case 'r':
                if (optarg[0] == '@') {
                    /* Read from file */
                    request_body = read_file_contents(optarg + 1);
                    if (!request_body) return 1;
                    request_body_allocated = 1;
                } else {
                    request_body = optarg;
                }
                break;
            case 'J':
                json_output = 1;
                break;
            case 'L':
                curl_global_init(CURL_GLOBAL_DEFAULT);
                {
                    int result = oauth_login();
                    curl_global_cleanup();
                    return result;
                }
            case 1:  /* --verbose (no short option) */
                verbose = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                print_version();
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Require print mode for now */
    if (!print_mode) {
        fprintf(stderr, "Error: Only --print mode is currently supported.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Raw request mode: send full request body directly */
    if (request_body) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        int result = api_send_raw_request(request_body, json_output, stdout);
        curl_global_cleanup();
        if (request_body_allocated) {
            free(request_body);
        }
        return result;
    }

    /* Collect remaining args as text prompt */
    char* text_prompt = NULL;
    if (optind < argc) {
        /* Concatenate remaining arguments */
        size_t total_len = 0;
        for (int i = optind; i < argc; i++) {
            total_len += strlen(argv[i]) + 1;
        }
        text_prompt = malloc(total_len);
        if (text_prompt) {
            char* p = text_prompt;
            for (int i = optind; i < argc; i++) {
                if (i > optind) *p++ = ' ';
                size_t len = strlen(argv[i]);
                memcpy(p, argv[i], len);
                p += len;
            }
            *p = '\0';
        }
    }

    /* If no text prompt from args, try stdin */
    if (!text_prompt && !messages) {
        text_prompt = read_stdin();
    }

    /* Load images if provided */
    if (image_count > 0) {
        for (int i = 0; i < image_count; i++) {
            images[i] = image_read(image_paths[i]);
            if (!images[i]) {
                /* Error already printed by image_read */
                for (int j = 0; j < i; j++) image_free(images[j]);
                free(text_prompt);
                return 1;
            }
        }
    }

    /* Build messages if not provided via -m */
    if (!messages) {
        if (!text_prompt) {
            fprintf(stderr, "Error: No prompt provided.\n");
            fprintf(stderr, "Use -m for raw JSON, provide text as argument, or pipe via stdin.\n\n");
            for (int i = 0; i < image_count; i++) image_free(images[i]);
            print_usage(argv[0]);
            return 1;
        }

        if (image_count > 0) {
            messages = build_messages_with_images(text_prompt, images, image_count);
        } else {
            messages = build_simple_messages(text_prompt);
        }

        if (!messages) {
            fprintf(stderr, "Error: Failed to build messages\n");
            for (int i = 0; i < image_count; i++) image_free(images[i]);
            free(text_prompt);
            return 1;
        }
        messages_allocated = 1;
    } else if (image_count > 0) {
        fprintf(stderr, "Warning: Images provided with -i are ignored when using -m (raw messages).\n");
        fprintf(stderr, "Include images in your JSON for multi-turn with images.\n");
    }

    free(text_prompt);
    for (int i = 0; i < image_count; i++) image_free(images[i]);

    /* Initialize libcurl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Make the API call */
    int result = api_send_message(model, system_prompt, messages, max_tokens, stream_mode, stdout);

    /* Cleanup */
    curl_global_cleanup();

    if (messages_allocated) {
        free(messages);
    }

    return result;
}
