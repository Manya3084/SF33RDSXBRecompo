#ifndef SF_FS_H
#define SF_FS_H

#include <stddef.h>
#include <stdint.h>

/* Retail fsOpen is 0x59A4D. It rejects a file number the archive does not
   hold and a zero length, then opens that member. This reads the unpacked
   file named by data/afs/index. The count in that index is the limit.
   The decomp constant 1535 is not. */

typedef struct SfFsFile {
    uint32_t fnum;
    uint32_t size;
    char name[48];
    uint8_t *data;
} SfFsFile;

int fsOpen(const char *install, uint32_t fnum, SfFsFile *out, char *err, size_t err_n);
void fsClose(SfFsFile *file);

#endif
