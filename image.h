/*
 * image.h - Image handling for Claude C client
 *
 * Supports: PNG, JPEG, GIF, WEBP
 * Constraints: max 2000x2000 pixels, ~5MB base64
 */

#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>

/* Limits matching Claude Code */
#define IMAGE_MAX_RAW_BYTES   3879731   /* ~3.7 MB raw, ~5 MB base64 */
#define IMAGE_MAX_WIDTH       2000
#define IMAGE_MAX_HEIGHT      2000
#define IMAGE_TOKEN_FACTOR    0.125     /* 8 base64 chars = 1 token */

/* Image data structure */
typedef struct {
    char* base64;           /* Base64 encoded data (caller must free) */
    char* media_type;       /* e.g., "image/png" (caller must free) */
    size_t original_size;   /* Original file size in bytes */
} image_t;

/* Detect MIME type from magic bytes
 * Returns static string like "image/png" or NULL if not recognized
 */
const char* image_detect_type(const unsigned char* data, size_t len);

/* Check if file extension is supported image type
 * ext should include the dot, e.g., ".png"
 * Returns 1 if supported, 0 otherwise
 */
int image_is_supported_ext(const char* ext);

/* Read image file and encode to base64
 * Returns allocated image_t on success, NULL on error
 * Caller must call image_free() when done
 *
 * Will fail if:
 * - File doesn't exist or can't be read
 * - File is not a recognized image format
 * - File exceeds IMAGE_MAX_RAW_BYTES
 */
image_t* image_read(const char* path);

/* Free image data */
void image_free(image_t* img);

/* Build JSON content block for an image
 * Returns allocated JSON string like:
 * {"type":"image","source":{"type":"base64","media_type":"image/png","data":"..."}}
 * Caller must free the result
 */
char* image_to_json(const image_t* img);

/* Calculate approximate token count for base64 data */
static inline int image_token_count(size_t base64_len) {
    return (int)(base64_len * IMAGE_TOKEN_FACTOR + 0.5);
}

#endif /* IMAGE_H */
