/*
 * azure_error.c — Error handling, XML parsing, retry logic
 *
 * Parses Azure Storage REST API error responses (XML format).
 * Classifies errors as transient vs permanent for retry decisions.
 * Implements exponential backoff with jitter.
 */

#include "azure_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Error code to string
 * ---------------------------------------------------------------- */

const char *azure_err_str(azure_err_t code)
{
    switch (code) {
        case AZURE_OK:              return "OK";
        case AZURE_ERR_HTTP:        return "HTTP error (non-retryable)";
        case AZURE_ERR_TRANSIENT:   return "Transient error (retryable)";
        case AZURE_ERR_AUTH:        return "Authentication failure";
        case AZURE_ERR_NOT_FOUND:   return "Not found (404)";
        case AZURE_ERR_CONFLICT:    return "Conflict (409)";
        case AZURE_ERR_PRECONDITION:return "Precondition failed (412)";
        case AZURE_ERR_CURL:        return "libcurl error";
        case AZURE_ERR_OPENSSL:     return "OpenSSL error";
        case AZURE_ERR_XML_PARSE:   return "XML parse error";
        case AZURE_ERR_INVALID_ARG: return "Invalid argument";
        case AZURE_ERR_ALLOC:       return "Memory allocation failure";
    }
    return "Unknown error";
}

/* ----------------------------------------------------------------
 * Simple XML tag extractor (no dependency on libxml2)
 *
 * Azure error responses are small and well-structured:
 *   <Error>
 *     <Code>ServerBusy</Code>
 *     <Message>The server is busy.</Message>
 *   </Error>
 *
 * We just need <Code> and <Message> — a simple strstr parser suffices.
 * ---------------------------------------------------------------- */

static int extract_xml_tag(const char *xml, size_t xml_len __attribute__((unused)),
                           const char *tag, char *out, size_t out_size)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return -1;
    start += strlen(open_tag);

    const char *end_ptr = strstr(start, close_tag);
    if (!end_ptr) return -1;

    size_t len = (size_t)(end_ptr - start);
    if (len >= out_size) len = out_size - 1;

    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

azure_err_t azure_parse_error_xml(const char *xml, size_t xml_len,
                                  azure_error_t *err)
{
    if (!xml || xml_len == 0 || !err) return AZURE_ERR_INVALID_ARG;

    /* Extract <Code> */
    if (extract_xml_tag(xml, xml_len, "Code",
                        err->error_code, sizeof(err->error_code)) != 0) {
        /* Not all errors have XML bodies (e.g., connection timeouts).
         * That's OK — the HTTP status code still tells us enough. */
        err->error_code[0] = '\0';
    }

    /* Extract <Message> */
    if (extract_xml_tag(xml, xml_len, "Message",
                        err->error_message, sizeof(err->error_message)) != 0) {
        err->error_message[0] = '\0';
    }

    return AZURE_OK;
}

/* ----------------------------------------------------------------
 * Classify HTTP status + error code as transient or permanent
 *
 * Transient errors (retry):
 *   500 InternalError
 *   503 ServerBusy
 *   408 RequestTimeout
 *   429 Too Many Requests (throttling)
 *
 * Azure-specific transient error codes:
 *   "ServerBusy", "InternalError", "OperationTimedOut"
 *
 * Permanent errors (don't retry):
 *   400 Bad Request
 *   401/403 Auth failure
 *   404 Not Found
 *   409 Conflict (lease held, blob exists)
 *   412 Precondition Failed
 *   416 Range Not Satisfiable
 * ---------------------------------------------------------------- */

int azure_is_transient_error(long http_status, const char *error_code)
{
    /* HTTP status-based classification */
    switch (http_status) {
        case 408: /* Request Timeout */
        case 429: /* Too Many Requests */
        case 500: /* Internal Server Error */
        case 502: /* Bad Gateway */
        case 503: /* Service Unavailable */
        case 504: /* Gateway Timeout */
            return 1;
    }

    /* Azure error code-based classification */
    if (error_code && *error_code) {
        if (strcmp(error_code, "ServerBusy") == 0) return 1;
        if (strcmp(error_code, "InternalError") == 0) return 1;
        if (strcmp(error_code, "OperationTimedOut") == 0) return 1;
    }

    return 0;
}

/* Map HTTP status to our error type */
azure_err_t azure_classify_http_error(long http_status, const char *error_code)
{
    if (http_status >= 200 && http_status < 300) return AZURE_OK;

    if (azure_is_transient_error(http_status, error_code))
        return AZURE_ERR_TRANSIENT;

    switch (http_status) {
        case 401:
        case 403: return AZURE_ERR_AUTH;
        case 404: return AZURE_ERR_NOT_FOUND;
        case 409: return AZURE_ERR_CONFLICT;
        case 412: return AZURE_ERR_PRECONDITION;
        default:  return AZURE_ERR_HTTP;
    }
}

/* ----------------------------------------------------------------
 * Exponential backoff with jitter
 *
 * Delay = min(base * 2^attempt + random_jitter, max_delay)
 *
 * This matches the pattern used by Azure SDKs (Go, .NET, Python):
 *   - Base delay: 500ms
 *   - Exponential growth: 500, 1000, 2000, 4000, 8000, ...
 *   - Jitter: random 0-500ms added to prevent thundering herd
 *   - Max delay: 30 seconds
 *   - Max retries: 5
 * ---------------------------------------------------------------- */

void azure_retry_sleep(int attempt)
{
    int delay_ms = AZURE_RETRY_BASE_MS * (1 << attempt);
    if (delay_ms > AZURE_RETRY_MAX_MS) delay_ms = AZURE_RETRY_MAX_MS;

    /* Add jitter: 0 to base_ms random */
    int jitter_ms = rand() % AZURE_RETRY_BASE_MS;
    delay_ms += jitter_ms;

    fprintf(stderr, "[azure] Retry attempt %d, sleeping %d ms\n",
            attempt + 1, delay_ms);

    usleep((unsigned int)(delay_ms * 1000));
}

/* ----------------------------------------------------------------
 * Execute an operation with retry logic
 * ---------------------------------------------------------------- */

azure_err_t azure_retry_execute(azure_operation_fn fn, void *ctx,
                                azure_error_t *err)
{
    azure_err_t rc;

    for (int attempt = 0; attempt <= AZURE_MAX_RETRIES; attempt++) {
        memset(err, 0, sizeof(*err));

        rc = fn(ctx, err);

        if (rc == AZURE_OK) return AZURE_OK;

        if (rc != AZURE_ERR_TRANSIENT) {
            /* Permanent error — don't retry */
            fprintf(stderr, "[azure] Permanent error: %s (HTTP %ld, %s: %s)\n",
                    azure_err_str(rc), err->http_status,
                    err->error_code, err->error_message);
            return rc;
        }

        if (attempt < AZURE_MAX_RETRIES) {
            fprintf(stderr, "[azure] Transient error: HTTP %ld %s — retrying\n",
                    err->http_status, err->error_code);
            azure_retry_sleep(attempt);
        }
    }

    fprintf(stderr, "[azure] All %d retries exhausted\n", AZURE_MAX_RETRIES);
    return rc;
}
