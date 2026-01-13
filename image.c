/*
 * image.c - Image handling for Claude C client
 */

#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <sys/stat.h>

/* Base64 encoding table */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 encode data
 * Returns allocated string, caller must free
 */
static char* base64_encode(const unsigned char* data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < len; ) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }

    /* Padding */
    size_t mod = len % 3;
    if (mod == 1) {
        out[out_len - 2] = '=';
        out[out_len - 1] = '=';
    } else if (mod == 2) {
        out[out_len - 1] = '=';
    }

    out[out_len] = '\0';
    return out;
}

/* Detect MIME type from magic bytes */
const char* image_detect_type(const unsigned char* data, size_t len) {
    if (len < 4) return NULL;

    /* PNG: 0x89 P N G */
    if (data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47) {
        return "image/png";
    }

    /* JPEG: 0xFF 0xD8 0xFF */
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "image/jpeg";
    }

    /* GIF: G I F 8 */
    if (data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46) {
        return "image/gif";
    }

    /* WEBP: RIFF....WEBP */
    if (len >= 12 &&
        data[0] == 0x52 && data[1] == 0x49 &&
        data[2] == 0x46 && data[3] == 0x46 &&
        data[8] == 0x57 && data[9] == 0x45 &&
        data[10] == 0x42 && data[11] == 0x50) {
        return "image/webp";
    }

    return NULL;
}

/* Check if extension is supported */
int image_is_supported_ext(const char* ext) {
    if (!ext) return 0;

    /* Skip leading dot if present */
    if (ext[0] == '.') ext++;

    return (strcasecmp(ext, "png") == 0 ||
            strcasecmp(ext, "jpg") == 0 ||
            strcasecmp(ext, "jpeg") == 0 ||
            strcasecmp(ext, "gif") == 0 ||
            strcasecmp(ext, "webp") == 0);
}

/* Get file extension from path */
static const char* get_extension(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot || dot == path) return NULL;
    return dot;
}

/* Read entire file into buffer */
static unsigned char* read_file(const char* path, size_t* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    /* Get file size */
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return NULL;
    }
    size_t size = st.st_size;

    /* Allocate buffer */
    unsigned char* data = malloc(size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    /* Read file */
    if (fread(data, 1, size, f) != size) {
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size_out = size;
    return data;
}

/* Read and encode image file */
image_t* image_read(const char* path) {
    /* Check extension first */
    const char* ext = get_extension(path);
    if (!ext || !image_is_supported_ext(ext)) {
        fprintf(stderr, "Error: Unsupported image format: %s\n",
                ext ? ext : "(no extension)");
        fprintf(stderr, "Supported formats: PNG, JPEG, GIF, WEBP\n");
        return NULL;
    }

    /* Read file */
    size_t size;
    unsigned char* data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "Error: Failed to read file: %s\n", path);
        return NULL;
    }

    /* Check size limit */
    if (size > IMAGE_MAX_RAW_BYTES) {
        fprintf(stderr, "Error: Image too large: %zu bytes (max %d)\n",
                size, IMAGE_MAX_RAW_BYTES);
        fprintf(stderr, "Please resize the image to under ~3.7 MB.\n");
        free(data);
        return NULL;
    }

    /* Detect actual type from magic bytes */
    const char* mime = image_detect_type(data, size);
    if (!mime) {
        fprintf(stderr, "Error: File does not appear to be a valid image: %s\n", path);
        free(data);
        return NULL;
    }

    /* Encode to base64 */
    char* b64 = base64_encode(data, size);
    free(data);
    if (!b64) {
        fprintf(stderr, "Error: Failed to encode image\n");
        return NULL;
    }

    /* Allocate result */
    image_t* img = malloc(sizeof(image_t));
    if (!img) {
        free(b64);
        return NULL;
    }

    img->base64 = b64;
    img->media_type = strdup(mime);
    img->original_size = size;

    if (!img->media_type) {
        free(b64);
        free(img);
        return NULL;
    }

    return img;
}

/* Free image data */
void image_free(image_t* img) {
    if (!img) return;
    free(img->base64);
    free(img->media_type);
    free(img);
}

/* Build JSON content block for image */
char* image_to_json(const image_t* img) {
    if (!img || !img->base64 || !img->media_type) return NULL;

    /* Format: {"type":"image","source":{"type":"base64","media_type":"...","data":"..."}} */
    size_t b64_len = strlen(img->base64);
    size_t media_len = strlen(img->media_type);

    /* Calculate size: template (68 chars) + media_type + base64 + null */
    size_t json_len = 72 + media_len + b64_len;

    char* json = malloc(json_len);
    if (!json) return NULL;

    snprintf(json, json_len,
        "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
        "\"media_type\":\"%s\",\"data\":\"%s\"}}",
        img->media_type, img->base64);

    return json;
}
