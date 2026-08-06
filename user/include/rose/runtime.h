#ifndef ROSE_USER_RUNTIME_H
#define ROSE_USER_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small, allocation-free userspace helpers shared by commands. */
size_t rose_string_length(const char *text);
bool rose_strings_equal(const char *left, const char *right);
bool rose_string_starts_with(const char *text, const char *prefix);
void rose_string_copy(char *destination, const char *source);
bool rose_parse_u64(const char *text, uint64_t *value);
bool rose_write_all(int descriptor, const void *buffer, size_t length);
void rose_print(const char *text);
void rose_print_error(const char *text);
void rose_print_u64(uint64_t value);

#endif
