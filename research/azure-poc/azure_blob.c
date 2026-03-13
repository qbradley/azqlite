/*
 * azure_blob.c — Azure Blob Storage REST API operations using libcurl
 *
 * Implements page blob, block blob, and lease operations against the
 * Azure Blob Storage REST API (version 2024-08-04).
 *
 * Dependencies: libcurl, OpenSSL (via azure_auth.c)
 */

#include "azure_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* ----------------------------------------------------------------
 * Response buffer management
 * ---------------------------------------------------------------- */

void azure_buffer_init(azure_buffer_t *buf)
{
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

void azure_buffer_free(azure_buffer_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

static int azure_buffer_append(azure_buffer_t *buf,
                               const uint8_t *data, size_t len)
{
    if (buf->size + len > buf->capacity) {
        size_t new_cap = (buf->capacity == 0) ? 4096 : buf->capacity * 2;
        while (new_cap < buf->size + len) new_cap *= 2;
        uint8_t *new_data = realloc(buf->data, new_cap);
        if (!new_data) return -1;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
    return 0;
}

/* libcurl write callback */
static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
{
    size_t real_size = size * nmemb;
    azure_buffer_t *buf = (azure_buffer_t *)userp;
    if (azure_buffer_append(buf, (const uint8_t *)contents, real_size) != 0) {
        return 0; /* Signal error to curl */
    }
    return real_size;
}

/* ----------------------------------------------------------------
 * Header capture callback — captures specific response headers
 * ---------------------------------------------------------------- */

typedef struct {
    char lease_id[64];
    char lease_state[32];
    char lease_status[32];
    char request_id[64];
    char error_code[128];
    int64_t content_length;
    int lease_time;  /* For break lease: remaining time */
} azure_response_headers_t;

static size_t curl_header_cb(char *buffer, size_t size, size_t nitems,
                             void *userp)
{
    size_t len = size * nitems;
    azure_response_headers_t *h = (azure_response_headers_t *)userp;

    /* Parse "Header-Name: value\r\n" */
    char *colon = memchr(buffer, ':', len);
    if (!colon) return len;

    size_t name_len = (size_t)(colon - buffer);
    char *value = colon + 1;
    while (*value == ' ') value++;

    /* Strip trailing \r\n */
    size_t value_len = len - (size_t)(value - buffer);
    while (value_len > 0 && (value[value_len-1] == '\r' || value[value_len-1] == '\n'))
        value_len--;

    /* Match headers we care about (case-insensitive) */
    if (name_len == 12 && strncasecmp(buffer, "x-ms-lease-id", 13) <= 0 &&
        strncasecmp(buffer, "x-ms-lease-id", name_len) == 0) {
        size_t cpy = value_len < sizeof(h->lease_id) - 1 ? value_len : sizeof(h->lease_id) - 1;
        memcpy(h->lease_id, value, cpy);
        h->lease_id[cpy] = '\0';
    }
    else if (strncasecmp(buffer, "x-ms-lease-state", name_len) == 0 && name_len == 16) {
        size_t cpy = value_len < sizeof(h->lease_state) - 1 ? value_len : sizeof(h->lease_state) - 1;
        memcpy(h->lease_state, value, cpy);
        h->lease_state[cpy] = '\0';
    }
    else if (strncasecmp(buffer, "x-ms-lease-status", name_len) == 0 && name_len == 17) {
        size_t cpy = value_len < sizeof(h->lease_status) - 1 ? value_len : sizeof(h->lease_status) - 1;
        memcpy(h->lease_status, value, cpy);
        h->lease_status[cpy] = '\0';
    }
    else if (strncasecmp(buffer, "x-ms-request-id", name_len) == 0 && name_len == 15) {
        size_t cpy = value_len < sizeof(h->request_id) - 1 ? value_len : sizeof(h->request_id) - 1;
        memcpy(h->request_id, value, cpy);
        h->request_id[cpy] = '\0';
    }
    else if (strncasecmp(buffer, "x-ms-error-code", name_len) == 0 && name_len == 15) {
        size_t cpy = value_len < sizeof(h->error_code) - 1 ? value_len : sizeof(h->error_code) - 1;
        memcpy(h->error_code, value, cpy);
        h->error_code[cpy] = '\0';
    }
    else if (strncasecmp(buffer, "Content-Length", name_len) == 0 && name_len == 14) {
        char tmp[32];
        size_t cpy = value_len < sizeof(tmp) - 1 ? value_len : sizeof(tmp) - 1;
        memcpy(tmp, value, cpy);
        tmp[cpy] = '\0';
        h->content_length = strtoll(tmp, NULL, 10);
    }
    else if (strncasecmp(buffer, "x-ms-lease-time", name_len) == 0 && name_len == 15) {
        char tmp[32];
        size_t cpy = value_len < sizeof(tmp) - 1 ? value_len : sizeof(tmp) - 1;
        memcpy(tmp, value, cpy);
        tmp[cpy] = '\0';
        h->lease_time = atoi(tmp);
    }

    return len;
}

/* ----------------------------------------------------------------
 * Client initialization
 * ---------------------------------------------------------------- */

azure_err_t azure_client_init(azure_client_t *client)
{
    memset(client, 0, sizeof(*client));

    /* Read configuration from environment */
    const char *account = getenv("AZURE_STORAGE_ACCOUNT");
    const char *key     = getenv("AZURE_STORAGE_KEY");
    const char *container = getenv("AZURE_STORAGE_CONTAINER");
    const char *sas     = getenv("AZURE_STORAGE_SAS");

    if (!account || !*account) {
        fprintf(stderr, "[azure] AZURE_STORAGE_ACCOUNT not set\n");
        return AZURE_ERR_INVALID_ARG;
    }
    if (!container || !*container) {
        fprintf(stderr, "[azure] AZURE_STORAGE_CONTAINER not set\n");
        return AZURE_ERR_INVALID_ARG;
    }

    strncpy(client->account, account, sizeof(client->account) - 1);
    strncpy(client->container, container, sizeof(client->container) - 1);

    /* SAS token auth (simpler, preferred for MVP) */
    if (sas && *sas) {
        strncpy(client->sas_token, sas, sizeof(client->sas_token) - 1);
        client->use_sas = 1;
        fprintf(stderr, "[azure] Using SAS token authentication\n");
    }
    /* Shared Key auth (full control) */
    else if (key && *key) {
        strncpy(client->key_b64, key, sizeof(client->key_b64) - 1);
        if (azure_base64_decode(key, client->key_raw, sizeof(client->key_raw),
                                &client->key_raw_len) != 0) {
            fprintf(stderr, "[azure] Failed to decode AZURE_STORAGE_KEY\n");
            return AZURE_ERR_AUTH;
        }
        client->use_sas = 0;
        fprintf(stderr, "[azure] Using Shared Key authentication\n");
    }
    else {
        fprintf(stderr, "[azure] Neither AZURE_STORAGE_KEY nor AZURE_STORAGE_SAS set\n");
        return AZURE_ERR_INVALID_ARG;
    }

    /* Initialize libcurl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[azure] curl_easy_init() failed\n");
        return AZURE_ERR_CURL;
    }
    client->curl_handle = curl;

    return AZURE_OK;
}

void azure_client_cleanup(azure_client_t *client)
{
    if (client->curl_handle) {
        curl_easy_cleanup((CURL *)client->curl_handle);
        client->curl_handle = NULL;
    }
    /* Scrub key material from memory */
    memset(client->key_raw, 0, sizeof(client->key_raw));
    memset(client->key_b64, 0, sizeof(client->key_b64));
}

/* ----------------------------------------------------------------
 * URL construction
 *
 * Format: https://<account>.blob.core.windows.net/<container>/<blob>
 * With SAS: append ?<sas_token> (or &<sas_token> if query params exist)
 * ---------------------------------------------------------------- */

void azure_blob_url(const azure_client_t *client, const char *blob_name,
                    char *url_buf, size_t url_buf_size)
{
    snprintf(url_buf, url_buf_size,
             "https://%s.blob.core.windows.net/%s/%s",
             client->account, client->container, blob_name);
}

/* ----------------------------------------------------------------
 * Internal: prepare and execute an HTTP request
 *
 * This is the core function that all operations use. It:
 * 1. Builds the URL (with query params and optional SAS token)
 * 2. Sets up headers (including auth)
 * 3. Executes via libcurl
 * 4. Parses response and errors
 * ---------------------------------------------------------------- */

static azure_err_t azure_execute_request(
    azure_client_t *client,
    const char *method,            /* "GET", "PUT", "DELETE", "HEAD" */
    const char *blob_name,
    const char *query,             /* "comp=page" or NULL */
    const char *const *extra_x_ms, /* Additional x-ms-* headers, NULL-terminated */
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    const char *range_header,      /* "bytes=0-511" or NULL */
    azure_buffer_t *response_body, /* Output: response body (can be NULL) */
    azure_response_headers_t *resp_headers, /* Output: parsed headers */
    azure_error_t *err)
{
    CURL *curl = (CURL *)client->curl_handle;
    CURLcode res;

    /* Build URL */
    char url[2048];
    azure_blob_url(client, blob_name, url, sizeof(url));

    /* Append query parameters */
    if (query && *query) {
        strcat(url, "?");
        strcat(url, query);
    }

    /* Append SAS token */
    if (client->use_sas) {
        strcat(url, (query && *query) ? "&" : "?");
        strcat(url, client->sas_token);
    }

    /* Build x-ms-date and x-ms-version headers */
    char date_buf[64];
    azure_rfc1123_time(date_buf, sizeof(date_buf));

    char date_header[128];
    snprintf(date_header, sizeof(date_header), "x-ms-date:%s", date_buf);

    char version_header[64];
    snprintf(version_header, sizeof(version_header),
             "x-ms-version:%s", AZURE_API_VERSION);

    /* Collect all x-ms-* headers for auth signing */
    const char *all_x_ms[32];
    int x_ms_count = 0;
    all_x_ms[x_ms_count++] = date_header;
    all_x_ms[x_ms_count++] = version_header;
    if (extra_x_ms) {
        for (int i = 0; extra_x_ms[i]; i++) {
            if (x_ms_count < 30) all_x_ms[x_ms_count++] = extra_x_ms[i];
        }
    }
    all_x_ms[x_ms_count] = NULL;

    /* Compute content-length string */
    char content_length_str[32] = "";
    if (body_len > 0) {
        snprintf(content_length_str, sizeof(content_length_str), "%zu", body_len);
    }

    /* Sign the request (unless using SAS) */
    char auth_header[512] = "";
    if (!client->use_sas) {
        char path[1024];
        snprintf(path, sizeof(path), "/%s/%s",
                 client->container, blob_name);

        azure_err_t rc = azure_auth_sign(
            client, method, path, query,
            content_length_str,
            content_type ? content_type : "",
            range_header ? range_header : "",
            (const char *const *)all_x_ms,
            auth_header, sizeof(auth_header));
        if (rc != AZURE_OK) {
            err->code = rc;
            return rc;
        }
    }

    /* Reset curl handle for reuse */
    curl_easy_reset(curl);

    /* Set URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* Set HTTP method */
    if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)body_len);
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (strcmp(method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    /* GET is the default */

    /* Build curl header list */
    struct curl_slist *headers = NULL;

    /* x-ms-date */
    char h_date[256];
    snprintf(h_date, sizeof(h_date), "x-ms-date: %s", date_buf);
    headers = curl_slist_append(headers, h_date);

    /* x-ms-version */
    char h_version[128];
    snprintf(h_version, sizeof(h_version), "x-ms-version: %s", AZURE_API_VERSION);
    headers = curl_slist_append(headers, h_version);

    /* Authorization (Shared Key) */
    if (auth_header[0]) {
        char h_auth[600];
        snprintf(h_auth, sizeof(h_auth), "Authorization: %s", auth_header);
        headers = curl_slist_append(headers, h_auth);
    }

    /* Content-Type */
    if (content_type && *content_type) {
        char h_ct[256];
        snprintf(h_ct, sizeof(h_ct), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, h_ct);
    }

    /* Content-Length for PUT with body */
    if (body_len > 0) {
        char h_cl[64];
        snprintf(h_cl, sizeof(h_cl), "Content-Length: %zu", body_len);
        headers = curl_slist_append(headers, h_cl);
    } else if (strcmp(method, "PUT") == 0) {
        headers = curl_slist_append(headers, "Content-Length: 0");
    }

    /* Range header */
    if (range_header && *range_header) {
        char h_range[256];
        snprintf(h_range, sizeof(h_range), "x-ms-range: %s", range_header);
        headers = curl_slist_append(headers, h_range);
    }

    /* Extra x-ms-* headers (blob type, lease action, etc.) */
    if (extra_x_ms) {
        for (int i = 0; extra_x_ms[i]; i++) {
            /* Convert "x-ms-name:value" to "x-ms-name: value" for curl */
            char h_extra[512];
            const char *colon = strchr(extra_x_ms[i], ':');
            if (colon) {
                size_t name_len = (size_t)(colon - extra_x_ms[i]);
                snprintf(h_extra, sizeof(h_extra), "%.*s: %s",
                         (int)name_len, extra_x_ms[i], colon + 1);
            } else {
                snprintf(h_extra, sizeof(h_extra), "%s", extra_x_ms[i]);
            }
            headers = curl_slist_append(headers, h_extra);
        }
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* Request body for PUT */
    if (body && body_len > 0) {
        /* Use CURLOPT_POSTFIELDS for simplicity with PUT */
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
        /* We'll use CURLOPT_POSTFIELDS trick: set CUSTOMREQUEST to PUT */
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    /* Response body callback */
    azure_buffer_t local_buf;
    azure_buffer_init(&local_buf);
    azure_buffer_t *body_buf = response_body ? response_body : &local_buf;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_buf);

    /* Response header callback */
    azure_response_headers_t local_headers;
    memset(&local_headers, 0, sizeof(local_headers));
    azure_response_headers_t *rh = resp_headers ? resp_headers : &local_headers;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, rh);

    /* Timeouts */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    /* Execute */
    res = curl_easy_perform(curl);

    /* Clean up header list */
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        err->code = AZURE_ERR_CURL;
        snprintf(err->error_message, sizeof(err->error_message),
                 "curl error: %s", curl_easy_strerror(res));
        if (!response_body) azure_buffer_free(&local_buf);
        return AZURE_ERR_CURL;
    }

    /* Get HTTP status */
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    err->http_status = http_status;

    /* Copy request ID for debugging */
    strncpy(err->request_id, rh->request_id, sizeof(err->request_id) - 1);

    /* Check for errors */
    if (http_status >= 400) {
        /* Parse error XML from response body */
        if (body_buf->size > 0) {
            azure_parse_error_xml((const char *)body_buf->data,
                                  body_buf->size, err);
        }
        /* Also check x-ms-error-code header */
        if (rh->error_code[0] && !err->error_code[0]) {
            strncpy(err->error_code, rh->error_code, sizeof(err->error_code) - 1);
        }

        err->code = azure_classify_http_error(http_status, err->error_code);

        if (!response_body) azure_buffer_free(&local_buf);
        return err->code;
    }

    if (!response_body) azure_buffer_free(&local_buf);
    return AZURE_OK;
}

/* ================================================================
 * PAGE BLOB OPERATIONS
 * ================================================================ */

/*
 * Create a page blob.
 *
 * PUT https://<account>.blob.core.windows.net/<container>/<blob>
 * Headers:
 *   x-ms-blob-type: PageBlob
 *   x-ms-blob-content-length: <size>   (must be 512-byte aligned)
 *   Content-Length: 0                    (no body for create)
 */
azure_err_t azure_page_blob_create(
    azure_client_t *client,
    const char *blob_name,
    int64_t blob_size,
    azure_error_t *err)
{
    if (blob_size % AZURE_PAGE_SIZE != 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page blob size must be 512-byte aligned, got %lld", (long long)blob_size);
        return AZURE_ERR_INVALID_ARG;
    }

    char size_header[64];
    snprintf(size_header, sizeof(size_header),
             "x-ms-blob-content-length:%lld", (long long)blob_size);

    const char *extra[] = {
        "x-ms-blob-type:PageBlob",
        size_header,
        NULL
    };

    return azure_execute_request(
        client, "PUT", blob_name, NULL,
        extra, NULL, NULL, 0, NULL,
        NULL, NULL, err);
}

/*
 * Write pages to a page blob.
 *
 * PUT https://<account>.blob.core.windows.net/<container>/<blob>?comp=page
 * Headers:
 *   x-ms-page-write: update
 *   x-ms-range: bytes=<start>-<end>     (512-byte aligned)
 *   Content-Length: <length>
 */
azure_err_t azure_page_blob_write(
    azure_client_t *client,
    const char *blob_name,
    int64_t offset,
    const uint8_t *data,
    size_t length,
    azure_error_t *err)
{
    if (offset % AZURE_PAGE_SIZE != 0 || length % AZURE_PAGE_SIZE != 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page write offset (%lld) and length (%zu) must be 512-byte aligned",
                 (long long)offset, length);
        return AZURE_ERR_INVALID_ARG;
    }
    if (length > AZURE_MAX_PAGE_WRITE) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page write length %zu exceeds 4 MiB maximum", length);
        return AZURE_ERR_INVALID_ARG;
    }

    char range[64];
    snprintf(range, sizeof(range), "bytes=%lld-%lld",
             (long long)offset, (long long)(offset + (int64_t)length - 1));

    const char *extra[] = {
        "x-ms-page-write:update",
        NULL
    };

    return azure_execute_request(
        client, "PUT", blob_name, "comp=page",
        extra, "application/octet-stream",
        data, length, range,
        NULL, NULL, err);
}

/*
 * Read from a page blob (or any blob) using Range header.
 *
 * GET https://<account>.blob.core.windows.net/<container>/<blob>
 * Headers:
 *   x-ms-range: bytes=<start>-<end>
 *
 * Range reads don't require 512-byte alignment.
 */
azure_err_t azure_page_blob_read(
    azure_client_t *client,
    const char *blob_name,
    int64_t offset,
    size_t length,
    azure_buffer_t *out,
    azure_error_t *err)
{
    char range[64];
    snprintf(range, sizeof(range), "bytes=%lld-%lld",
             (long long)offset, (long long)(offset + (int64_t)length - 1));

    return azure_execute_request(
        client, "GET", blob_name, NULL,
        NULL, NULL, NULL, 0, range,
        out, NULL, err);
}

/*
 * Get blob properties (HEAD request).
 *
 * HEAD https://<account>.blob.core.windows.net/<container>/<blob>
 *
 * Returns Content-Length, x-ms-lease-state, x-ms-lease-status in headers.
 */
azure_err_t azure_blob_get_properties(
    azure_client_t *client,
    const char *blob_name,
    int64_t *content_length,
    char *lease_state,
    char *lease_status,
    azure_error_t *err)
{
    azure_response_headers_t rh;
    memset(&rh, 0, sizeof(rh));

    azure_err_t rc = azure_execute_request(
        client, "HEAD", blob_name, NULL,
        NULL, NULL, NULL, 0, NULL,
        NULL, &rh, err);

    if (rc == AZURE_OK) {
        if (content_length) *content_length = rh.content_length;
        if (lease_state) strncpy(lease_state, rh.lease_state, 31);
        if (lease_status) strncpy(lease_status, rh.lease_status, 31);
    }

    return rc;
}

/*
 * Resize a page blob.
 *
 * PUT https://<account>.blob.core.windows.net/<container>/<blob>?comp=properties
 * Headers:
 *   x-ms-blob-content-length: <new_size>   (512-byte aligned)
 */
azure_err_t azure_page_blob_resize(
    azure_client_t *client,
    const char *blob_name,
    int64_t new_size,
    azure_error_t *err)
{
    if (new_size % AZURE_PAGE_SIZE != 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page blob resize must be 512-byte aligned, got %lld",
                 (long long)new_size);
        return AZURE_ERR_INVALID_ARG;
    }

    char size_header[64];
    snprintf(size_header, sizeof(size_header),
             "x-ms-blob-content-length:%lld", (long long)new_size);

    const char *extra[] = {
        size_header,
        NULL
    };

    return azure_execute_request(
        client, "PUT", blob_name, "comp=properties",
        extra, NULL, NULL, 0, NULL,
        NULL, NULL, err);
}

/* ================================================================
 * BLOCK BLOB OPERATIONS
 * ================================================================ */

/*
 * Upload a block blob (simple Put Blob — entire content at once).
 *
 * PUT https://<account>.blob.core.windows.net/<container>/<blob>
 * Headers:
 *   x-ms-blob-type: BlockBlob
 *   Content-Type: <type>
 *   Content-Length: <length>
 *
 * For blobs up to 5000 MiB (API version 2024-08-04).
 */
azure_err_t azure_block_blob_upload(
    azure_client_t *client,
    const char *blob_name,
    const uint8_t *data,
    size_t length,
    const char *content_type,
    azure_error_t *err)
{
    const char *extra[] = {
        "x-ms-blob-type:BlockBlob",
        NULL
    };

    return azure_execute_request(
        client, "PUT", blob_name, NULL,
        extra,
        content_type ? content_type : "application/octet-stream",
        data, length, NULL,
        NULL, NULL, err);
}

/*
 * Download a block blob entirely.
 *
 * GET https://<account>.blob.core.windows.net/<container>/<blob>
 */
azure_err_t azure_block_blob_download(
    azure_client_t *client,
    const char *blob_name,
    azure_buffer_t *out,
    azure_error_t *err)
{
    return azure_execute_request(
        client, "GET", blob_name, NULL,
        NULL, NULL, NULL, 0, NULL,
        out, NULL, err);
}

/*
 * Delete a blob (any type).
 *
 * DELETE https://<account>.blob.core.windows.net/<container>/<blob>
 */
azure_err_t azure_blob_delete(
    azure_client_t *client,
    const char *blob_name,
    azure_error_t *err)
{
    return azure_execute_request(
        client, "DELETE", blob_name, NULL,
        NULL, NULL, NULL, 0, NULL,
        NULL, NULL, err);
}

/* ================================================================
 * LEASE OPERATIONS
 *
 * All lease operations use:
 *   PUT https://<account>.blob.core.windows.net/<container>/<blob>?comp=lease
 *   x-ms-lease-action: <action>
 *
 * Lease durations: 15-60 seconds, or -1 for infinite.
 * Lease IDs are GUIDs returned by Azure.
 *
 * For SQLite locking:
 *   - SHARED lock: We don't need a lease (read without lease is fine)
 *   - RESERVED lock: Acquire lease (prevents other writers)
 *   - EXCLUSIVE lock: Already have lease from RESERVED
 *   - Unlock: Release lease
 * ================================================================ */

azure_err_t azure_lease_acquire(
    azure_client_t *client,
    const char *blob_name,
    int duration_secs,
    char *lease_id_out,
    size_t lease_id_size,
    azure_error_t *err)
{
    if (duration_secs != -1 && (duration_secs < 15 || duration_secs > 60)) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Lease duration must be 15-60 or -1 (infinite), got %d",
                 duration_secs);
        return AZURE_ERR_INVALID_ARG;
    }

    char duration_header[64];
    snprintf(duration_header, sizeof(duration_header),
             "x-ms-lease-duration:%d", duration_secs);

    const char *extra[] = {
        "x-ms-lease-action:acquire",
        duration_header,
        NULL
    };

    azure_response_headers_t rh;
    memset(&rh, 0, sizeof(rh));

    azure_err_t rc = azure_execute_request(
        client, "PUT", blob_name, "comp=lease",
        extra, NULL, NULL, 0, NULL,
        NULL, &rh, err);

    if (rc == AZURE_OK && lease_id_out) {
        strncpy(lease_id_out, rh.lease_id, lease_id_size - 1);
        lease_id_out[lease_id_size - 1] = '\0';
    }

    return rc;
}

azure_err_t azure_lease_renew(
    azure_client_t *client,
    const char *blob_name,
    const char *lease_id,
    azure_error_t *err)
{
    char lease_header[128];
    snprintf(lease_header, sizeof(lease_header),
             "x-ms-lease-id:%s", lease_id);

    const char *extra[] = {
        "x-ms-lease-action:renew",
        lease_header,
        NULL
    };

    return azure_execute_request(
        client, "PUT", blob_name, "comp=lease",
        extra, NULL, NULL, 0, NULL,
        NULL, NULL, err);
}

azure_err_t azure_lease_release(
    azure_client_t *client,
    const char *blob_name,
    const char *lease_id,
    azure_error_t *err)
{
    char lease_header[128];
    snprintf(lease_header, sizeof(lease_header),
             "x-ms-lease-id:%s", lease_id);

    const char *extra[] = {
        "x-ms-lease-action:release",
        lease_header,
        NULL
    };

    return azure_execute_request(
        client, "PUT", blob_name, "comp=lease",
        extra, NULL, NULL, 0, NULL,
        NULL, NULL, err);
}

azure_err_t azure_lease_break(
    azure_client_t *client,
    const char *blob_name,
    int break_period_secs,
    int *remaining_secs,
    azure_error_t *err)
{
    const char *extra[4];
    int idx = 0;
    extra[idx++] = "x-ms-lease-action:break";

    char period_header[64];
    if (break_period_secs >= 0) {
        snprintf(period_header, sizeof(period_header),
                 "x-ms-lease-break-period:%d", break_period_secs);
        extra[idx++] = period_header;
    }
    extra[idx] = NULL;

    azure_response_headers_t rh;
    memset(&rh, 0, sizeof(rh));

    azure_err_t rc = azure_execute_request(
        client, "PUT", blob_name, "comp=lease",
        extra, NULL, NULL, 0, NULL,
        NULL, &rh, err);

    if (rc == AZURE_OK && remaining_secs) {
        *remaining_secs = rh.lease_time;
    }

    return rc;
}
