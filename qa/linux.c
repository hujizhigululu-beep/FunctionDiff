#define _POSIX_C_SOURCE 200809L
#include "../compare.h"
#include <dirent.h>
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <sys/stat.h>

void memory_error(void) { fputs("Out of memory\n", stderr); exit(2); }
int path_kind(const char *path)
{
    struct stat attrs;
    if (stat(path, &attrs)) return 0;
    return S_ISDIR(attrs.st_mode) ? 2 : S_ISREG(attrs.st_mode) ? 1 : 0;
}
int collect_paths(const char *root, const char *relative, Paths *paths)
{
    char *path = join_path(root, relative);
    DIR *directory = opendir(path);
    free(path);
    if (!directory) return 0;
    int okay = 1;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(directory);
        if (!item) { if (errno) okay = 0; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..") || !strcmp(item->d_name, ".git"))
            continue;
        char *next = join_path(relative, item->d_name);
        char *full = join_path(root, next);
        struct stat attrs;
        if (lstat(full, &attrs)) okay = 0;
        else if (S_ISDIR(attrs.st_mode)) okay = collect_paths(root, next, paths);
        else if (S_ISREG(attrs.st_mode) && is_source(item->d_name)) add_path(paths, next);
        free(next);
        free(full);
        if (!okay) break;
    }
    closedir(directory);
    return okay;
}
void *open_path(const char *path, int writing) { return fopen(path, writing ? "wb" : "rb"); }
size_t read_data(void *file, char *buffer, size_t size) { return fread(buffer, 1, size, file); }
int read_error(void *file) { return ferror(file); }
int write_data(void *file, const char *buffer, size_t size) { return fwrite(buffer, 1, size, file) == size; }
int close_path(void *file) { return fclose(file) == 0; }
char *convert_utf8(char *data, size_t size, size_t *new_size)
{
    const unsigned char *raw = (const unsigned char *)data;
    const char *encoding = "UTF-8";
    if (size >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
        memmove(data, data + 3, size - 3);
        *new_size = size - 3;
        return data;
    }
    if (size >= 2 && raw[0] == 0xfe && raw[1] == 0xff) encoding = "UTF-16";
    if (size >= 2 && raw[0] == 0xff && raw[1] == 0xfe) encoding = "UTF-16";
    char *converted = grow_array(0, size * 4 + 1);
    size_t remaining = 0;
    int okay = 0;
    for (int i = 0; i < 2; i++) {
        iconv_t cd = iconv_open("UTF-8", i ? "GBK" : encoding);
        if (cd == (iconv_t)-1) continue;
        char *src = data, *dst = converted;
        size_t bytes = size;
        remaining = size * 4;
        okay = iconv(cd, &src, &bytes, &dst, &remaining) != (size_t)-1;
        iconv_close(cd);
        if (okay) break;
        if (strcmp(encoding, "UTF-8")) break;
    }
    free(data);
    if (!okay) { free(converted); return 0; }
    *new_size = size * 4 - remaining;
    return converted;
}
int main(int argc, char **argv)
{
    if (argc != 5) {
        fputs("Usage: compare old new output json|html|after\n", stderr);
        return 2;
    }
    ReportFormat format = REPORT_HTML;
    if (!strcmp(argv[4], "json")) format = REPORT_JSON;
    else if (!strcmp(argv[4], "after")) format = REPORT_JSON_AFTER;
    Summary result = {0};
    if (!compare_paths(argv[1], argv[2], argv[3], format, &result)) {
        fprintf(stderr, "%s %s\n", result.problem, result.problem_path ? result.problem_path : "");
        free(result.problem_path);
        return 1;
    }
    return 0;
}
