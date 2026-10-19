#include "iso/archive.h"

#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SF_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

int sf_mkdir_p(const char *path, char *err, size_t err_n)
{
    char buf[4096];
    size_t len;
    size_t i;
    if (!path || !path[0]) {
        sf_set_err(err, err_n, "missing directory");
        return -1;
    }
    len = strlen(path);
    if (len >= sizeof buf) {
        sf_set_err(err, err_n, "directory path is too long");
        return -1;
    }
    memcpy(buf, path, len + 1);
    for (i = 1; i < len; i++) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = 0;
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            sf_set_err(err, err_n, "cannot create %s", buf);
            return -1;
        }
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        sf_set_err(err, err_n, "cannot create %s", buf);
        return -1;
    }
    return 0;
}

static int rm_one(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return 0;
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        return unlink(path);
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char child[4096];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (sf_join(child, sizeof child, path, de->d_name) != 0) {
            closedir(dir);
            return -1;
        }
        if (rm_one(child) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(path);
}

int sf_rm_rf(const char *path)
{
    char resolved[4096];
    if (!path || path[0] != '/') {
        return -1;
    }
    if (!realpath(path, resolved)) {
        return -1;
    }
    if (strcmp(resolved, "/") == 0 || strcmp(resolved, "/tmp") == 0) {
        return -1;
    }
    if (strncmp(resolved, "/tmp/", 5) != 0 && !strstr(resolved, "/.import-tmp")) {
        return -1;
    }
    return rm_one(resolved);
}

static int normalize_rel(const char *in, char *out, size_t n)
{
    size_t o = 0;
    const char *p = in;
    if (!in) {
        return -1;
    }
    while (*p == '/' || *p == '\\') {
        return -1;
    }
    while (*p) {
        size_t len;
        size_t i;
        const char *start;
        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (!*p) {
            break;
        }
        start = p;
        while (*p && *p != '/' && *p != '\\') {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            return -1;
        }
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            if (c < 0x20 || c == 0x7f || c == ':') {
                return -1;
            }
        }
        if (o != 0) {
            if (o + 1 >= n) {
                return -1;
            }
            out[o++] = '/';
        }
        if (o + len >= n) {
            return -1;
        }
        memcpy(out + o, start, len);
        o += len;
    }
    if (o == 0) {
        return -1;
    }
    out[o] = 0;
    return 0;
}

static int mkdir_parent(const char *file, char *err, size_t err_n)
{
    char dir[4096];
    char *slash;
    size_t n = strlen(file);
    if (n >= sizeof dir) {
        sf_set_err(err, err_n, "path is too long");
        return -1;
    }
    memcpy(dir, file, n + 1);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return 0;
    }
    *slash = 0;
    return sf_mkdir_p(dir, err, err_n);
}

#ifdef SF_HAVE_LIBARCHIVE

static int open_archive(const char *archive, struct archive **out, char *err, size_t err_n)
{
    struct archive *a = archive_read_new();
    if (!a) {
        sf_set_err(err, err_n, "out of memory");
        return -1;
    }
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, archive, 65536) != ARCHIVE_OK) {
        sf_set_err(err, err_n, "cannot read archive (%s)", archive_error_string(a));
        archive_read_free(a);
        return -1;
    }
    *out = a;
    return 0;
}

int sf_archive_visit(const char *archive, SfArchiveVisitFn fn, void *user, char *err, size_t err_n)
{
    struct archive *a = NULL;
    struct archive_entry *ent;
    int rc;
    if (open_archive(archive, &a, err, err_n) != 0) {
        return -1;
    }
    while ((rc = archive_read_next_header(a, &ent)) == ARCHIVE_OK) {
        char norm[1024];
        const char *raw = archive_entry_pathname(ent);
        int is_dir;
        int stop;
        mode_t mode = archive_entry_filetype(ent);
        if (normalize_rel(raw, norm, sizeof norm) != 0) {
            archive_read_free(a);
            sf_set_err(err, err_n, "archive path refused");
            return -1;
        }
        is_dir = mode == AE_IFDIR;
        stop = fn ? fn(norm, is_dir, (uint64_t)archive_entry_size(ent), user) : 0;
        archive_read_data_skip(a);
        if (stop != 0) {
            break;
        }
    }
    if (rc != ARCHIVE_EOF && rc != ARCHIVE_OK) {
        sf_set_err(err, err_n, "cannot list archive (%s)", archive_error_string(a));
        archive_read_free(a);
        return -1;
    }
    archive_read_free(a);
    return 0;
}

int sf_archive_export(const char *archive, SfArchiveMapFn map, void *user,
                      void (*progress)(const char *name, void *progress_user), void *progress_user,
                      int *written, char *err, size_t err_n)
{
    struct archive *a = NULL;
    struct archive_entry *ent;
    int rc;
    uint64_t total = 0;
    if (written) {
        *written = 0;
    }
    if (open_archive(archive, &a, err, err_n) != 0) {
        return -1;
    }
    while ((rc = archive_read_next_header(a, &ent)) == ARCHIVE_OK) {
        char norm[1024];
        char dest[4096];
        const char *raw = archive_entry_pathname(ent);
        mode_t mode = archive_entry_filetype(ent);
        int is_dir;
        int decision;
        la_int64_t size;
        if (mode == AE_IFLNK) {
            archive_read_data_skip(a);
            continue;
        }
        if (normalize_rel(raw, norm, sizeof norm) != 0) {
            archive_read_free(a);
            sf_set_err(err, err_n, "archive path refused");
            return -1;
        }
        is_dir = mode == AE_IFDIR;
        dest[0] = 0;
        decision = map ? map(norm, is_dir, dest, sizeof dest, user) : 0;
        if (decision < 0) {
            archive_read_free(a);
            if (err && err_n && err[0] == 0) {
                sf_set_err(err, err_n, "archive extract aborted");
            }
            return -1;
        }
        if (decision == 0 || is_dir) {
            archive_read_data_skip(a);
            continue;
        }
        size = archive_entry_size(ent);
        if (size < 0 || (uint64_t)size > (16ull << 30)) { /* an Xbox DVD-9 image is up to 7.6 GB */
            archive_read_free(a);
            sf_set_err(err, err_n, "archive member is too large");
            return -1;
        }
        total += (uint64_t)size;
        if (total > (4ull << 30)) {
            archive_read_free(a);
            sf_set_err(err, err_n, "refusing to extract more than the Xbox 3rd Strike files");
            return -1;
        }
        if (mkdir_parent(dest, err, err_n) != 0) {
            archive_read_free(a);
            return -1;
        }
        FILE *out = fopen(dest, "wb");
        if (!out) {
            archive_read_free(a);
            sf_set_err(err, err_n, "cannot write %s", dest);
            return -1;
        }
        for (;;) {
            char block[65536];
            la_ssize_t got = archive_read_data(a, block, sizeof block);
            if (got == 0) {
                break;
            }
            if (got < 0 || fwrite(block, 1, (size_t)got, out) != (size_t)got) {
                fclose(out);
                archive_read_free(a);
                sf_set_err(err, err_n, "failed while writing %s", dest);
                return -1;
            }
        }
        if (fclose(out) != 0) {
            archive_read_free(a);
            sf_set_err(err, err_n, "failed to finish %s", dest);
            return -1;
        }
        if (written) {
            (*written)++;
        }
        if (progress) {
            progress(norm, progress_user);
        }
    }
    if (rc != ARCHIVE_EOF && rc != ARCHIVE_OK) {
        sf_set_err(err, err_n, "cannot extract archive (%s)", archive_error_string(a));
        archive_read_free(a);
        return -1;
    }
    archive_read_free(a);
    return 0;
}

#else

int sf_archive_visit(const char *archive, SfArchiveVisitFn fn, void *user, char *err, size_t err_n)
{
    (void)archive;
    (void)fn;
    (void)user;
    sf_set_err(err, err_n, "libarchive is required to read a zip or 7z of the Xbox disc");
    return -1;
}

int sf_archive_export(const char *archive, SfArchiveMapFn map, void *user,
                      void (*progress)(const char *name, void *progress_user), void *progress_user,
                      int *written, char *err, size_t err_n)
{
    (void)archive;
    (void)map;
    (void)user;
    (void)progress;
    (void)progress_user;
    (void)written;
    sf_set_err(err, err_n, "libarchive is required to read a zip or 7z of the Xbox disc");
    return -1;
}

#endif
