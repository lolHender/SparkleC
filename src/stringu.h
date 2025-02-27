#include <stdlib.h>

char* basename(const char* const path);
char* get_file_extension(const char* const filename);
char* normalize_filename(char* filename);
char* normalize_directory(char* directory);
char from_hex(const char ch);
size_t intlen(const int value);
int isnumeric(const char* const s);
char* get_parent_directory(const char* const source, char* const destination, const size_t depth);
int hashs(const char* const s);

#pragma once
