#ifndef SLOP_DRIVER_SOURCE_FILE_H
#define SLOP_DRIVER_SOURCE_FILE_H

#include <stddef.h>

/* Reads a whole file into one malloc'd, NUL-terminated buffer and stores its
 * length in *out_len. Returns NULL after printing why it could not.
 *
 * The passes borrow this buffer and never copy out of it, so it has to stay
 * alive until the last tree built from it is gone. The caller frees it. */
char *source_file_read(const char *path, size_t *out_len);

#endif /* SLOP_DRIVER_SOURCE_FILE_H */
