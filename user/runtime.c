#include "rose/runtime.h"

#include "rose/syscall.h"
#include "user_abi.h"

size_t rose_string_length(const char *text) {
        size_t length = 0U;
        while (text[length] != '\0') {
                length++;
        }
        return length;
}

bool rose_strings_equal(const char *left, const char *right) {
        while (*left != '\0' && *left == *right) {
                left++;
                right++;
        }
        return *left == *right;
}

bool rose_string_starts_with(const char *text, const char *prefix) {
        while (*prefix != '\0') {
                if (*text++ != *prefix++) {
                        return false;
                }
        }
        return true;
}

void rose_string_copy(char *destination, const char *source) {
        do {
                *destination++ = *source;
        } while (*source++ != '\0');
}

bool rose_parse_u64(const char *text, uint64_t *value) {
        if (*text < '0' || *text > '9') {
                return false;
        }

        uint64_t result = 0U;
        do {
                uint64_t digit = (uint64_t)(*text++ - '0');
                if (result > (UINT64_MAX - digit) / 10U) {
                        return false;
                }
                result = result * 10U + digit;
        } while (*text >= '0' && *text <= '9');

        if (*text != '\0') {
                return false;
        }
        *value = result;
        return true;
}

bool rose_write_all(int descriptor, const void *buffer, size_t length) {
        const char *bytes = buffer;
        size_t written = 0U;
        while (written < length) {
                long count =
                    rose_write(descriptor, &bytes[written], length - written);
                if (count <= 0) {
                        return false;
                }
                written += (size_t)count;
        }
        return true;
}

void rose_print(const char *text) {
        (void)rose_write_all(USER_STDOUT_FILENO, text,
                             rose_string_length(text));
}

void rose_print_error(const char *text) {
        (void)rose_write_all(USER_STDERR_FILENO, text,
                             rose_string_length(text));
}

void rose_print_u64(uint64_t value) {
        char digits[20];
        size_t length = 0U;
        do {
                digits[length++] = (char)('0' + value % 10U);
                value /= 10U;
        } while (value != 0U);
        while (length != 0U) {
                char digit = digits[--length];
                (void)rose_write_all(USER_STDOUT_FILENO, &digit, 1U);
        }
}
