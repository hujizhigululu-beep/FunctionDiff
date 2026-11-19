#ifndef FUNCTION_DIFF_COMPARE_H
#define FUNCTION_DIFF_COMPARE_H

#include <stddef.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} Paths;

typedef struct {
    size_t compared;
    size_t files;
    size_t added;
    size_t modified;
    size_t removed;
    size_t outside;
    char *problem_path;
    const char *problem;
} Summary;

typedef enum { REPORT_HTML, REPORT_JSON, REPORT_JSON_AFTER } ReportFormat;

#ifdef TARGET_WINDOWS
#include "import_slots.h"
extern void * volatile *win_iat;
#define IMPORT(name, return_type, signature) ((return_type (*) signature) win_iat[SLOT_##name])
#define malloc IMPORT(malloc, void *, (size_t))
#define realloc IMPORT(realloc, void *, (void *, size_t))
#define free IMPORT(free, void, (void *))
#define memcpy IMPORT(memcpy, void *, (void *, const void *, size_t))
#define memcmp IMPORT(memcmp, int, (const void *, const void *, size_t))
#define memset IMPORT(memset, void *, (void *, int, size_t))
#define strlen IMPORT(strlen, size_t, (const char *))
#define strcmp IMPORT(strcmp, int, (const char *, const char *))
#else
#include <stdlib.h>
#include <string.h>
#endif

void *grow_array(void *memory, size_t size);
char *copy_bytes(const char *text, size_t size);
char *join_path(const char *directory, const char *name);
const char *base_name(const char *path);
int path_compare(const char *left, const char *right);
int is_source(const char *name);
void add_path(Paths *paths, const char *name);
int path_kind(const char *path);
int collect_paths(const char *root, const char *relative, Paths *paths);
void *open_path(const char *path, int writing);
size_t read_data(void *file, char *buffer, size_t size);
int read_error(void *file);
int write_data(void *file, const char *buffer, size_t size);
int close_path(void *file);
char *convert_utf8(char *bytes, size_t size, size_t *new_size);
void memory_error(void);

int compare_paths(const char *old_path, const char *new_path, const char *output,
                  ReportFormat format, Summary *result);
#endif
