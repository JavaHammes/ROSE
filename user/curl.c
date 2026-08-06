#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rose/network.h"
#include "rose/runtime.h"
#include "rose/syscall.h"
#include "user_abi.h"

enum {
        CURL_HOST_MAX = 253,
        CURL_REQUEST_MAX = 1024,
        CURL_HEADER_MAX = 2048,
        CURL_READ_MAX = 1024,
        HTTP_PORT = 80,
};

struct http_url {
        char host[CURL_HOST_MAX + 1U];
        const char *path;
        uint16_t port;
        bool path_needs_slash;
};

static char request_buffer[CURL_REQUEST_MAX];
static uint8_t header_buffer[CURL_HEADER_MAX];
static uint8_t read_buffer[CURL_READ_MAX];

static bool append_bytes(size_t *length, const char *text, size_t count) {
        if (count > CURL_REQUEST_MAX - *length) return false;
        for (size_t index = 0U; index < count; index++) {
                request_buffer[(*length)++] = text[index];
        }
        return true;
}

static bool append_text(size_t *length, const char *text) {
        return append_bytes(length, text, rose_string_length(text));
}

static bool append_port(size_t *length, uint16_t port) {
        char digits[5];
        size_t count = 0U;
        do {
                digits[count++] = (char)('0' + port % 10U);
                port = (uint16_t)(port / 10U);
        } while (port != 0U);
        while (count != 0U) {
                if (!append_bytes(length, &digits[--count], 1U)) return false;
        }
        return true;
}

static bool parse_http_url(const char *text, struct http_url *url) {
        static const char prefix[] = "http://";
        if (!rose_string_starts_with(text, prefix)) return false;
        text += sizeof(prefix) - 1U;

        size_t authority_length = 0U;
        while (text[authority_length] != '\0' &&
               text[authority_length] != '/' &&
               text[authority_length] != '?') {
                authority_length++;
        }
        if (authority_length == 0U) return false;

        size_t host_length = authority_length;
        uint16_t port = HTTP_PORT;
        for (size_t index = 0U; index < authority_length; index++) {
                if (text[index] != ':') continue;
                if (index == 0U || index + 1U == authority_length) return false;
                host_length = index;
                uint32_t parsed_port = 0U;
                for (size_t digit = index + 1U; digit < authority_length;
                     digit++) {
                        if (text[digit] < '0' || text[digit] > '9') {
                                return false;
                        }
                        parsed_port = parsed_port * 10U +
                                      (uint32_t)(text[digit] - '0');
                        if (parsed_port > UINT16_MAX) return false;
                }
                if (parsed_port == 0U) return false;
                port = (uint16_t)parsed_port;
                break;
        }
        if (host_length == 0U || host_length > CURL_HOST_MAX) return false;
        for (size_t index = 0U; index < host_length; index++) {
                char character = text[index];
                if (character == '@' || character == '[' ||
                    character == ']') {
                        return false;
                }
                url->host[index] = character;
        }
        url->host[host_length] = '\0';
        url->path = &text[authority_length];
        url->path_needs_slash = *url->path != '/';
        url->port = port;
        return true;
}

static long build_request(const struct http_url *url) {
        size_t length = 0U;
        if (!append_text(&length, "GET ") ||
            (*url->path == '\0'
                 ? !append_text(&length, "/")
                 : ((url->path_needs_slash &&
                     !append_text(&length, "/")) ||
                    !append_text(&length, url->path))) ||
            !append_text(&length, " HTTP/1.0\r\nHost: ") ||
            !append_text(&length, url->host) ||
            (url->port != HTTP_PORT &&
             (!append_text(&length, ":") ||
              !append_port(&length, url->port))) ||
            !append_text(&length,
                         "\r\nUser-Agent: rose-curl/1.0\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n")) {
                return -USER_ERROR_NAME_TOO_LONG;
        }
        return (long)length;
}

static size_t header_end(const uint8_t *buffer, size_t length) {
        if (length < 4U) return 0U;
        for (size_t index = 3U; index < length; index++) {
                if (buffer[index - 3U] == '\r' &&
                    buffer[index - 2U] == '\n' &&
                    buffer[index - 1U] == '\r' && buffer[index] == '\n') {
                        return index + 1U;
                }
        }
        return 0U;
}

static bool print_response(int descriptor, bool include_headers) {
        size_t buffered = 0U;
        size_t body_start = 0U;
        while (body_start == 0U) {
                long count = rose_read(descriptor, read_buffer,
                                       sizeof(read_buffer));
                if (count <= 0) return false;
                if ((size_t)count > CURL_HEADER_MAX - buffered) return false;
                for (size_t index = 0U; index < (size_t)count; index++) {
                        header_buffer[buffered++] = read_buffer[index];
                }
                body_start = header_end(header_buffer, buffered);
        }

        size_t output_start = include_headers ? 0U : body_start;
        if (buffered != output_start &&
            !rose_write_all(USER_STDOUT_FILENO, &header_buffer[output_start],
                            buffered - output_start)) {
                return false;
        }
        for (;;) {
                long count = rose_read(descriptor, read_buffer,
                                       sizeof(read_buffer));
                if (count == 0) return true;
                if (count < 0 ||
                    !rose_write_all(USER_STDOUT_FILENO, read_buffer,
                                    (size_t)count)) {
                        return false;
                }
        }
}

int rose_curl_main(int argc, char **argv) {
        bool include_headers = false;
        const char *url_text = NULL;
        if (argc == 2) {
                url_text = argv[1];
        } else if (argc == 3 && rose_strings_equal(argv[1], "-i")) {
                include_headers = true;
                url_text = argv[2];
        } else {
                rose_print_error("usage: curl [-i] http://HOST[:PORT]/PATH\n");
                return 2;
        }
        if (rose_string_starts_with(url_text, "https://")) {
                rose_print_error("curl: HTTPS is not supported (TLS unavailable)\n");
                return 1;
        }

        struct http_url url;
        if (!parse_http_url(url_text, &url)) {
                rose_print_error("curl: invalid or unsupported URL\n");
                return 2;
        }
        uint32_t address;
        if (rose_resolve_ipv4(url.host, &address) < 0) {
                rose_print_error("curl: could not resolve host\n");
                return 1;
        }
        long descriptor = rose_socket(USER_SOCKET_STREAM,
                                      USER_INTERNET_PROTOCOL_TCP);
        if (descriptor < 0) {
                rose_print_error("curl: could not create socket\n");
                return 1;
        }
        struct user_socket_address destination = {
            .address = address,
            .port = url.port,
        };
        if (rose_socket_connect((int)descriptor, &destination) < 0) {
                rose_print_error("curl: connection failed\n");
                (void)rose_close((int)descriptor);
                return 1;
        }
        long request_length = build_request(&url);
        if (request_length < 0 ||
            !rose_write_all((int)descriptor, request_buffer,
                            (size_t)request_length)) {
                rose_print_error("curl: request failed\n");
                (void)rose_close((int)descriptor);
                return 1;
        }
        bool success = print_response((int)descriptor, include_headers);
        (void)rose_close((int)descriptor);
        if (!success) {
                rose_print_error("curl: response failed\n");
                return 1;
        }
        return 0;
}
