#include "driver/source_file.h"

#include <stdio.h>
#include <stdlib.h>

char *source_file_read(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "slop: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "slop: cannot seek %s\n", path);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "slop: cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fprintf(stderr, "slop: out of memory\n");
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[got] = '\0';
    *out_len = got;
    return buf;
}
