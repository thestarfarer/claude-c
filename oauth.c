/*
 * oauth.c - OAuth login flow for Claude C client
 *
 * Implements OAuth 2.0 Authorization Code flow with PKCE.
 */

#include "oauth.h"
#include "auth.h"
#include "api.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <curl/curl.h>

#define PKCE_VERIFIER_LEN 64
#define STATE_LEN 32
#define DEFAULT_PORT 8765
#define CALLBACK_TIMEOUT 600

/* Base64url alphabet (no padding) */
static const char base64url_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Base64url encode (no padding) */
static char* base64url_encode(const unsigned char* data, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4 + 1;
    char* out = malloc(out_len);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < len; ) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = base64url_table[(triple >> 18) & 0x3F];
        out[j++] = base64url_table[(triple >> 12) & 0x3F];
        out[j++] = base64url_table[(triple >> 6) & 0x3F];
        out[j++] = base64url_table[triple & 0x3F];
    }

    /* Remove padding equivalent */
    if (len % 3 == 1) j -= 2;
    else if (len % 3 == 2) j -= 1;
    out[j] = '\0';

    return out;
}

/* Generate random base64url string */
static char* random_base64url(size_t bytes) {
    unsigned char* buf = malloc(bytes);
    if (!buf) return NULL;

    if (RAND_bytes(buf, bytes) != 1) {
        free(buf);
        return NULL;
    }

    char* result = base64url_encode(buf, bytes);
    free(buf);
    return result;
}

/* SHA256 hash */
static unsigned char* sha256(const char* input, size_t* out_len) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;

    unsigned char* hash = malloc(EVP_MD_size(EVP_sha256()));
    if (!hash) {
        EVP_MD_CTX_free(ctx);
        return NULL;
    }

    unsigned int len;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, input, strlen(input)) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &len) != 1) {
        free(hash);
        EVP_MD_CTX_free(ctx);
        return NULL;
    }

    EVP_MD_CTX_free(ctx);
    *out_len = len;
    return hash;
}

/* Generate PKCE parameters */
pkce_t* pkce_generate(void) {
    pkce_t* pkce = calloc(1, sizeof(pkce_t));
    if (!pkce) return NULL;

    /* Generate code_verifier (43-128 chars, we use 64 bytes -> ~86 chars) */
    pkce->code_verifier = random_base64url(PKCE_VERIFIER_LEN);
    if (!pkce->code_verifier) {
        free(pkce);
        return NULL;
    }

    /* Generate code_challenge = base64url(SHA256(code_verifier)) */
    size_t hash_len;
    unsigned char* hash = sha256(pkce->code_verifier, &hash_len);
    if (!hash) {
        free(pkce->code_verifier);
        free(pkce);
        return NULL;
    }

    pkce->code_challenge = base64url_encode(hash, hash_len);
    free(hash);
    if (!pkce->code_challenge) {
        free(pkce->code_verifier);
        free(pkce);
        return NULL;
    }

    /* Generate state */
    pkce->state = random_base64url(STATE_LEN);
    if (!pkce->state) {
        free(pkce->code_verifier);
        free(pkce->code_challenge);
        free(pkce);
        return NULL;
    }

    return pkce;
}

void pkce_free(pkce_t* pkce) {
    if (!pkce) return;
    free(pkce->code_verifier);
    free(pkce->code_challenge);
    free(pkce->state);
    free(pkce);
}

/* Build authorization URL */
char* oauth_build_auth_url(const pkce_t* pkce, int port) {
    if (!pkce) return NULL;

    /* URL-encode components are safe (base64url has no special chars) */
    size_t url_len = 1024 + strlen(pkce->code_challenge) + strlen(pkce->state);
    char* url = malloc(url_len);
    if (!url) return NULL;

    snprintf(url, url_len,
        "%s?code=true"
        "&client_id=%s"
        "&response_type=code"
        "&redirect_uri=http://localhost:%d/callback"
        "&scope=user:profile%%20user:inference%%20user:sessions:claude_code"
        "&code_challenge=%s"
        "&code_challenge_method=S256"
        "&state=%s",
        OAUTH_AUTHORIZE_URL,
        OAUTH_CLIENT_ID,
        port,
        pkce->code_challenge,
        pkce->state);

    return url;
}

/* Extract query parameter from URL */
static char* get_query_param(const char* url, const char* param) {
    char search[256];
    snprintf(search, sizeof(search), "%s=", param);

    const char* start = strstr(url, search);
    if (!start) return NULL;
    start += strlen(search);

    const char* end = start;
    while (*end && *end != '&' && *end != ' ' && *end != '\r' && *end != '\n') {
        end++;
    }

    size_t len = end - start;
    char* value = malloc(len + 1);
    if (!value) return NULL;

    memcpy(value, start, len);
    value[len] = '\0';
    return value;
}

/* Wait for OAuth callback */
char* oauth_wait_for_callback(int port, const char* expected_state, int timeout_sec) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port)
    };

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        return NULL;
    }

    /* Set timeout */
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    fprintf(stderr, "Waiting for authorization (timeout: %ds)...\n", timeout_sec);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "Timeout waiting for authorization\n");
        } else {
            perror("accept");
        }
        close(server_fd);
        return NULL;
    }

    /* Read request */
    char buffer[4096];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        close(server_fd);
        return NULL;
    }
    buffer[n] = '\0';

    /* Extract code and state from GET /callback?code=...&state=... */
    char* code = get_query_param(buffer, "code");
    char* state = get_query_param(buffer, "state");

    /* Send response */
    const char* response_ok =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><h1>Authentication successful!</h1>"
        "<p>You can close this window and return to the terminal.</p></body></html>";
    const char* response_err =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><h1>Authentication failed</h1>"
        "<p>Invalid or missing parameters.</p></body></html>";

    int valid = code && state && strcmp(state, expected_state) == 0;
    send(client_fd, valid ? response_ok : response_err,
         strlen(valid ? response_ok : response_err), 0);

    close(client_fd);
    close(server_fd);

    free(state);
    if (!valid) {
        free(code);
        return NULL;
    }

    return code;
}

/* Curl write callback */
typedef struct {
    char* data;
    size_t size;
} response_t;

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    response_t* resp = (response_t*)userdata;
    size_t bytes = size * nmemb;

    char* new_data = realloc(resp->data, resp->size + bytes + 1);
    if (!new_data) return 0;

    resp->data = new_data;
    memcpy(resp->data + resp->size, ptr, bytes);
    resp->size += bytes;
    resp->data[resp->size] = '\0';

    return bytes;
}

/* Exchange authorization code for tokens */
int oauth_exchange_code(const char* code, const pkce_t* pkce, int port) {
    if (!code || !pkce) return -1;

    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    response_t resp = {NULL, 0};
    struct curl_slist* headers = NULL;

    /* Build JSON request body */
    char redirect_uri[128];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://localhost:%d/callback", port);

    size_t body_len = 1024 + strlen(code) + strlen(pkce->code_verifier) + strlen(pkce->state);
    char* body = malloc(body_len);
    if (!body) {
        curl_easy_cleanup(curl);
        return -1;
    }

    snprintf(body, body_len,
        "{\"grant_type\":\"authorization_code\","
        "\"code\":\"%s\","
        "\"redirect_uri\":\"%s\","
        "\"client_id\":\"%s\","
        "\"code_verifier\":\"%s\","
        "\"state\":\"%s\"}",
        code, redirect_uri, OAUTH_CLIENT_ID, pkce->code_verifier, pkce->state);

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, OAUTH_TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    int result = -1;

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200 && resp.data) {
            char* access_token = json_get_string(resp.data, "access_token");
            char* refresh_token = json_get_string(resp.data, "refresh_token");
            int found_expires = 0;
            long long expires_in = json_get_number(resp.data, "expires_in", &found_expires);
            char* scope = json_get_string(resp.data, "scope");

            if (access_token && refresh_token) {
                oauth_creds_t creds = {0};
                creds.access_token = access_token;
                creds.refresh_token = refresh_token;
                creds.scopes = scope;

                if (found_expires && expires_in > 0) {
                    /* Get current time in milliseconds */
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    creds.expires_at = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000 + (expires_in * 1000);
                }

                result = oauth_save(&creds);
                if (result == 0) {
                    fprintf(stderr, "Authentication successful!\n");
                }

                /* Don't free - oauth_save doesn't copy */
                access_token = NULL;
                refresh_token = NULL;
                scope = NULL;
            } else {
                fprintf(stderr, "Token response missing required fields\n");
            }

            free(access_token);
            free(refresh_token);
            free(scope);
        } else {
            fprintf(stderr, "Token exchange failed: HTTP %ld\n", http_code);
            if (resp.data) {
                char* error = json_get_string(resp.data, "error");
                char* desc = json_get_string(resp.data, "error_description");
                if (error) fprintf(stderr, "  Error: %s\n", error);
                if (desc) fprintf(stderr, "  Description: %s\n", desc);
                free(error);
                free(desc);
            }
        }
    } else {
        fprintf(stderr, "Token exchange request failed: %s\n", curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    free(resp.data);

    return result;
}

/* Run full OAuth login flow */
int oauth_login(void) {
    int port = DEFAULT_PORT;

    fprintf(stderr, "Starting OAuth login flow...\n");

    /* Generate PKCE parameters */
    pkce_t* pkce = pkce_generate();
    if (!pkce) {
        fprintf(stderr, "Failed to generate PKCE parameters\n");
        return -1;
    }

    /* Build authorization URL */
    char* auth_url = oauth_build_auth_url(pkce, port);
    if (!auth_url) {
        fprintf(stderr, "Failed to build authorization URL\n");
        pkce_free(pkce);
        return -1;
    }

    /* Display URL for user */
    fprintf(stderr, "\nOpen this URL in your browser to authenticate:\n\n");
    fprintf(stderr, "  %s\n\n", auth_url);
    free(auth_url);

    /* Wait for callback */
    char* code = oauth_wait_for_callback(port, pkce->state, CALLBACK_TIMEOUT);
    if (!code) {
        fprintf(stderr, "Failed to receive authorization code\n");
        pkce_free(pkce);
        return -1;
    }

    /* Exchange code for tokens */
    int result = oauth_exchange_code(code, pkce, port);

    free(code);
    pkce_free(pkce);

    return result;
}
