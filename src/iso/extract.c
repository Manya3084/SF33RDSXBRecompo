#include "iso/extract.h"

#include "common.h"
#include "iso/afs.h"
#include "iso/archive.h"
#include "iso/xbe.h"
#include "iso/xdvdfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
#endif

static int ends_ci(const char *s, const char *suf)
{
    size_t n = strlen(s);
    size_t m = strlen(suf);
    if (n < m) {
        return 0;
    }
    return strncasecmp(s + n - m, suf, m) == 0;
}

static int eq_ci(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

static void add_note(SfExtractResult *result, const char *msg)
{
    size_t len;
    if (!result || !msg) {
        return;
    }
    if (result->note[0] == 0) {
        snprintf(result->note, sizeof result->note, "%s", msg);
        return;
    }
    len = strlen(result->note);
    snprintf(result->note + len, sizeof result->note - len, " %s", msg);
}

static int mkdir_parent(const char *file, char *err, size_t err_n)
{
    char dir[4096];
    char *slash;
    if (strlen(file) >= sizeof dir) {
        sf_set_err(err, err_n, "path is too long");
        return -1;
    }
    memcpy(dir, file, strlen(file) + 1);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return 0;
    }
    *slash = 0;
    return sf_mkdir_p(dir, err, err_n);
}

static int copy_stream(const char *src, const char *dest, char *err, size_t err_n)
{
    FILE *in;
    FILE *out;
    uint8_t buf[65536];
    size_t n;
    if (mkdir_parent(dest, err, err_n) != 0) {
        return -1;
    }
    in = fopen(src, "rb");
    out = fopen(dest, "wb");
    if (!in || !out) {
        if (in) {
            fclose(in);
        }
        if (out) {
            fclose(out);
        }
        sf_set_err(err, err_n, "cannot copy %s", dest);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            sf_set_err(err, err_n, "failed while writing %s", dest);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        sf_set_err(err, err_n, "failed while reading %s", src);
        return -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        sf_set_err(err, err_n, "failed to finish %s", dest);
        return -1;
    }
    return 0;
}

/* The archive is in place (the import's own check, before the sound
 * banks are read). */
static int afs_ready(const char *install_dir)
{
    char path[4096];
    struct stat st;
    if (!install_dir || sf_join(path, sizeof path, install_dir, "data/afs/SF33RDXB.AFS") != 0) {
        return 0;
    }
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

int sf_data_ready(const char *install_dir)
{
    char path[4096];
    struct stat st;
    if (!install_dir || sf_join(path, sizeof path, install_dir, "data/afs/SF33RDXB.AFS") != 0) {
        return 0;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    return sf_sound_banks_ready(install_dir);
}

/* The 21 sound banks the import reads out of sf33rd.xbe (the sound
 * effects' cue tables): data imported before they were has to be
 * imported again. */
int sf_sound_banks_ready(const char *install_dir)
{
    char path[4096];
    char name[32];
    struct stat st;
    int i;
    for (i = 0; i < SF_SOUND_BANKS; i++) {
        snprintf(name, sizeof name, "data/audio/sdbk/%02d.sdbk", i);
        if (sf_join(path, sizeof path, install_dir, name) != 0 || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size <= 0) {
            return 0;
        }
    }
    return 1;
}

static int has_cvs(const char *rel)
{
    const char *p = rel;
    while (*p) {
        const char *start = p;
        size_t n = 0;
        while (p[n] && p[n] != '/') {
            n++;
        }
        if (n == 3 && strncasecmp(start, "CVS", 3) == 0) {
            return 1;
        }
        p += n;
        if (*p == '/') {
            p++;
        }
    }
    return 0;
}

static int run_convert(const char *tool, const char *src, const char *dst)
{
#ifdef _WIN32
    /* Windows: python, if it is on the PATH */
    const char *argv[6];
    intptr_t rc;
    argv[0] = "python";
    argv[1] = tool;
    argv[2] = src;
    argv[3] = "-o";
    argv[4] = dst;
    argv[5] = NULL;
    rc = _spawnvp(_P_WAIT, "python", argv);
    return rc == 0 ? 0 : -1;
#else
    char *argv[6];
    pid_t pid;
    int status = 0;
    argv[0] = "python3";
    argv[1] = (char *)tool;
    argv[2] = (char *)src;
    argv[3] = "-o";
    argv[4] = (char *)dst;
    argv[5] = NULL;
    if (posix_spawnp(&pid, "python3", NULL, NULL, argv, environ) != 0) {
        return -1;
    }
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0) {
            return -1;
        }
        break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
#endif
}

static int convert_dir(const char *dir, const char *rel, const char *spirv_root, const char *tool, SfExtractResult *result)
{
    char full[4096];
    DIR *d;
    struct dirent *de;
    if (rel[0] == 0) {
        snprintf(full, sizeof full, "%s", dir);
    } else if (sf_join(full, sizeof full, dir, rel) != 0) {
        return -1;
    }
    d = opendir(full);
    if (!d) {
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        char child_rel[1024];
        char child_full[4096];
        struct stat st;
        int wrote;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        wrote = rel[0] == 0 ? snprintf(child_rel, sizeof child_rel, "%s", de->d_name)
                            : snprintf(child_rel, sizeof child_rel, "%s/%s", rel, de->d_name);
        if (wrote < 0 || (size_t)wrote >= sizeof child_rel || sf_join(child_full, sizeof child_full, dir, child_rel) != 0) {
            closedir(d);
            return -1;
        }
        if (lstat(child_full, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (convert_dir(dir, child_rel, spirv_root, tool, result) != 0) {
                closedir(d);
                return -1;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || has_cvs(child_rel)) {
            continue;
        }
        if (!ends_ci(de->d_name, ".xvu") && !ends_ci(de->d_name, ".xpu")) {
            continue;
        }
        result->shaders++;
        if (!tool) {
            continue;
        }
        {
            char spv_rel[1024];
            char spv_path[4096];
            size_t len = strlen(child_rel);
            if (len < 4 || len >= sizeof spv_rel) {
                result->spirv_failed++;
                continue;
            }
            memcpy(spv_rel, child_rel, len - 3);
            memcpy(spv_rel + len - 3, "spv", 4);
            if (sf_join(spv_path, sizeof spv_path, spirv_root, spv_rel) != 0 || mkdir_parent(spv_path, result->error, sizeof result->error) != 0) {
                closedir(d);
                return -1;
            }
            if (run_convert(tool, child_full, spv_path) == 0) {
                result->spirv++;
            } else {
                result->spirv_failed++;
                unlink(spv_path);
            }
        }
    }
    closedir(d);
    return 0;
}

static int count_ext(const char *dir, const char *rel, int *count)
{
    char full[4096];
    DIR *d;
    struct dirent *de;
    if (rel[0] == 0) {
        snprintf(full, sizeof full, "%s", dir);
    } else if (sf_join(full, sizeof full, dir, rel) != 0) {
        return -1;
    }
    d = opendir(full);
    if (!d) {
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        char child_rel[1024];
        char child_full[4096];
        struct stat st;
        int wrote;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        wrote = rel[0] == 0 ? snprintf(child_rel, sizeof child_rel, "%s", de->d_name)
                            : snprintf(child_rel, sizeof child_rel, "%s/%s", rel, de->d_name);
        if (wrote < 0 || (size_t)wrote >= sizeof child_rel) {
            closedir(d);
            return -1;
        }
        if (sf_join(child_full, sizeof child_full, dir, child_rel) != 0) {
            closedir(d);
            return -1;
        }
        if (lstat(child_full, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (count_ext(dir, child_rel, count) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode) && ends_ci(de->d_name, ".spv")) {
            (*count)++;
        }
    }
    closedir(d);
    return 0;
}

static int write_manifest(const char *install, const SfExtractResult *result)
{
    char path[4096];
    FILE *fp;
    if (sf_join(path, sizeof path, install, "data/manifest") != 0 || mkdir_parent(path, NULL, 0) != 0) {
        return -1;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    fprintf(fp, "sf33rd-data 1\nafs yes\nafs_files %d\nshaders %d\nspirv %d\nspirv_failed %d\nshader_api vulkan opengl\n",
            result->afs_files, result->shaders, result->spirv, result->spirv_failed);
    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static int finish_shaders(const char *install, const char *tool, SfExtractResult *result)
{
    char nv2a[4096];
    char spirv[4096];
    if (sf_join(nv2a, sizeof nv2a, install, "data/shaders/nv2a") != 0 || sf_join(spirv, sizeof spirv, install, "data/shaders/spirv") != 0) {
        sf_set_err(result->error, sizeof result->error, "shader path is too long");
        return -1;
    }
    result->shaders = 0;
    if (convert_dir(nv2a, "", spirv, tool, result) != 0) {
        return -1;
    }
    if (result->shaders > 0 && !tool) {
        add_note(result, "Shader tool was not found. SPIR-V was not written.");
    } else if (result->spirv_failed > 0) {
        add_note(result, "NV2A shaders were copied. SPIR-V conversion failed for some of them.");
    } else if (result->spirv > 0) {
        add_note(result, "NV2A shaders were converted to SPIR-V for Vulkan and OpenGL.");
    }
    if (write_manifest(install, result) != 0) {
        add_note(result, "The data manifest was not written.");
    }
    return 0;
}

static int ci_child(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d) {
        return 1;
    }
    while ((de = readdir(d)) != NULL) {
        if (strcasecmp(de->d_name, name) == 0) {
            int rc = sf_join(out, n, dir, de->d_name);
            closedir(d);
            return rc == 0 ? 0 : -1;
        }
    }
    closedir(d);
    return 1;
}

static int ci_resolve(const char *root, const char *rel, char *out, size_t n)
{
    char cur[4096];
    const char *p = rel;
    if (strlen(root) >= sizeof cur) {
        return -1;
    }
    memcpy(cur, root, strlen(root) + 1);
    while (*p) {
        char name[256];
        char next[4096];
        size_t nlen = 0;
        int rc;
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        while (p[nlen] && p[nlen] != '/') {
            if (nlen + 1 >= sizeof name) {
                return -1;
            }
            name[nlen] = p[nlen];
            nlen++;
        }
        name[nlen] = 0;
        p += nlen;
        rc = ci_child(cur, name, next, sizeof next);
        if (rc != 0) {
            return rc;
        }
        if (strlen(next) >= sizeof cur) {
            return -1;
        }
        memcpy(cur, next, strlen(next) + 1);
    }
    if (strlen(cur) >= n) {
        return -1;
    }
    memcpy(out, cur, strlen(cur) + 1);
    return 0;
}

static int copy_tree(const char *src_dir, const char *rel, const char *dest_root, int shaders, int *files, SfExtractProgress progress, void *user, char *err, size_t err_n)
{
    char full[4096];
    DIR *d;
    struct dirent *de;
    if (rel[0] == 0) {
        snprintf(full, sizeof full, "%s", src_dir);
    } else if (sf_join(full, sizeof full, src_dir, rel) != 0) {
        sf_set_err(err, err_n, "path is too long");
        return -1;
    }
    d = opendir(full);
    if (!d) {
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        char child_rel[1024];
        char child_full[4096];
        char dest[4096];
        struct stat st;
        int wrote;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        wrote = rel[0] == 0 ? snprintf(child_rel, sizeof child_rel, "%s", de->d_name)
                            : snprintf(child_rel, sizeof child_rel, "%s/%s", rel, de->d_name);
        if (wrote < 0 || (size_t)wrote >= sizeof child_rel || sf_join(child_full, sizeof child_full, src_dir, child_rel) != 0) {
            closedir(d);
            sf_set_err(err, err_n, "path is too long");
            return -1;
        }
        if (lstat(child_full, &st) != 0 || S_ISLNK(st.st_mode) || has_cvs(child_rel)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree(src_dir, child_rel, dest_root, shaders, files, progress, user, err, err_n) != 0) {
                closedir(d);
                return -1;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (shaders && !ends_ci(de->d_name, ".xvu") && !ends_ci(de->d_name, ".xpu")) {
            continue;
        }
        if (sf_join(dest, sizeof dest, dest_root, child_rel) != 0 || copy_stream(child_full, dest, err, err_n) != 0) {
            closedir(d);
            return -1;
        }
        (*files)++;
        if (progress) {
            progress(child_rel, user);
        }
    }
    closedir(d);
    return 0;
}

/* The XBE is copied to data/.import-tmp only for its sound banks (the
 * cue tables the sound effects play from), which go to data/audio/sdbk.
 * The XBE itself is not kept. */
#define XBE_TMP "data/.import-tmp/sf33rd.xbe"

/* The sound effects' cue tables, out of the disc's sf33rd.xbe (copied to
 * XBE_TMP by the reader and gone after): the game needs them. */
static int import_sound_banks(const char *install, SfExtractResult *result)
{
    char xbe[4096];
    char out[4096];
    char err[256];
    struct stat st;
    if (sf_join(xbe, sizeof xbe, install, XBE_TMP) != 0 || sf_join(out, sizeof out, install, "data/audio/sdbk") != 0) {
        sf_set_err(result->error, sizeof result->error, "the install path is too long");
        return -1;
    }
    if (stat(xbe, &st) != 0) {
        sf_set_err(result->error, sizeof result->error, "sf33rd.xbe is not on the disc: it holds the sound effects");
        return -1;
    }
    err[0] = 0;
    if (sf_mkdir_p(out, err, sizeof err) != 0 || sf_xbe_sound_banks(xbe, out, err, sizeof err) != 0) {
        sf_set_err(result->error, sizeof result->error, "the sound effects were not read from sf33rd.xbe: %s", err);
        unlink(xbe);
        return -1;
    }
    result->files += SF_SOUND_BANKS;
    unlink(xbe);
    return 0;
}

static int finish_import(const char *install, const char *tool, SfExtractProgress progress, void *user, SfExtractResult *result)
{
    char afs[4096];
    char dir[4096];
    char index[4096];
    int written = 0;
    int rc;
    if (sf_join(afs, sizeof afs, install, "data/afs/SF33RDXB.AFS") != 0 || sf_join(dir, sizeof dir, install, "data/afs/files") != 0 || sf_join(index, sizeof index, install, "data/afs/index") != 0) {
        sf_set_err(result->error, sizeof result->error, "AFS path is too long");
        return -1;
    }
    if (import_sound_banks(install, result) != 0) {
        return -1;
    }
    rc = sf_afs_unpack(afs, dir, index, progress, user, &written, result->error, sizeof result->error);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        add_note(result, "SF33RDXB.AFS was copied. It is not an AFS archive, so its files were not unpacked.");
    } else {
        result->files += written;
        result->afs_files = written;
    }
    return finish_shaders(install, tool, result);
}

static int import_files_from_dir(const char *root, const char *install, const char *tool, SfExtractProgress progress, void *user, SfExtractResult *result)
{
    char src[4096];
    char dest[4096];
    char tree[4096];
    if (ci_resolve(root, "XB_HostPC/SF33RDXB.AFS", src, sizeof src) != 0) {
        sf_set_err(result->error, sizeof result->error, "The Xbox data has no XB_HostPC/SF33RDXB.AFS.");
        return -1;
    }
    if (sf_join(dest, sizeof dest, install, "data/afs/SF33RDXB.AFS") != 0 || copy_stream(src, dest, result->error, sizeof result->error) != 0) {
        unlink(dest);
        return -1;
    }
    result->files++;
    if (progress) {
        progress("XB_HostPC/SF33RDXB.AFS", user);
    }
    if (ci_resolve(root, "XB_HostPC/ST3_3_OP.aix", src, sizeof src) == 0) {
        if (sf_join(dest, sizeof dest, install, "data/audio/ST3_3_OP.aix") != 0 || copy_stream(src, dest, result->error, sizeof result->error) != 0) {
            return -1;
        }
        result->files++;
        if (progress) {
            progress("XB_HostPC/ST3_3_OP.aix", user);
        }
    }
    if (ci_resolve(root, "sf33rd.xbe", src, sizeof src) == 0) {
        if (sf_join(dest, sizeof dest, install, XBE_TMP) != 0 || mkdir_parent(dest, result->error, sizeof result->error) != 0 || copy_stream(src, dest, result->error, sizeof result->error) != 0) {
            return -1;
        }
    }
    if (ci_resolve(root, "media/Fonts", tree, sizeof tree) == 0) {
        char font_dest[4096];
        if (sf_join(font_dest, sizeof font_dest, install, "data/fonts") != 0 || copy_tree(tree, "", font_dest, 0, &result->files, progress, user, result->error, sizeof result->error) != 0) {
            return -1;
        }
    } else {
        add_note(result, "Fonts were not in the Xbox data and were skipped.");
    }
    if (ci_resolve(root, "media/Shaders", tree, sizeof tree) == 0) {
        char shader_dest[4096];
        if (sf_join(shader_dest, sizeof shader_dest, install, "data/shaders/nv2a") != 0 || copy_tree(tree, "", shader_dest, 1, &result->files, progress, user, result->error, sizeof result->error) != 0) {
            return -1;
        }
    }
    return finish_import(install, tool, progress, user, result);
}

struct WalkCopy {
    SfXdvdfs *img;
    const char *install;
    const char *dest_rel;
    int shaders;
    int *files;
    SfExtractProgress progress;
    void *user;
    char *err;
    size_t err_n;
    int failed;
};

static int on_iso_file(const char *path, const SfXdvdfsEnt *ent, void *user)
{
    struct WalkCopy *w = user;
    const char *base;
    char dest[4096];
    char rel[1024];
    if (!ent || ent->is_dir || w->failed) {
        return 0;
    }
    if (has_cvs(path)) {
        return 0;
    }
    if (strncasecmp(path, "media/Fonts/", 12) == 0) {
        base = path + 12;
    } else if (strncasecmp(path, "media/Shaders/", 14) == 0) {
        base = path + 14;
    } else {
        base = strrchr(path, '/');
        base = base ? base + 1 : path;
    }
    if (w->shaders && !ends_ci(base, ".xvu") && !ends_ci(base, ".xpu")) {
        return 0;
    }
    if (snprintf(rel, sizeof rel, "%s", base) >= (int)sizeof rel) {
        return 0;
    }
    {
        char root[4096];
        if (sf_join(root, sizeof root, w->install, w->dest_rel) != 0 || sf_join(dest, sizeof dest, root, rel) != 0) {
            w->failed = 1;
            sf_set_err(w->err, w->err_n, "path is too long");
            return 1;
        }
    }
    if (mkdir_parent(dest, w->err, w->err_n) != 0 || sf_xdvdfs_save(w->img, ent, dest, w->err, w->err_n) != 0) {
        w->failed = 1;
        return 1;
    }
    (*w->files)++;
    if (w->progress) {
        w->progress(path, w->user);
    }
    return 0;
}

static int import_iso(const char *source, const char *install, const char *tool, SfExtractProgress progress, void *user, SfExtractResult *result)
{
    SfXdvdfs *img;
    SfXdvdfsEnt ent;
    char dest[4096];
    int rc;
    struct WalkCopy walk;
    img = sf_xdvdfs_open(source, result->error, sizeof result->error);
    if (!img) {
        return -1;
    }
    rc = sf_xdvdfs_find(img, "XB_HostPC/SF33RDXB.AFS", &ent, result->error, sizeof result->error);
    if (rc != 0) {
        sf_xdvdfs_close(img);
        if (rc == 1) {
            sf_set_err(result->error, sizeof result->error, "This Xbox image has no XB_HostPC/SF33RDXB.AFS.");
        }
        return -1;
    }
    if (sf_join(dest, sizeof dest, install, "data/afs/SF33RDXB.AFS") != 0 || mkdir_parent(dest, result->error, sizeof result->error) != 0 || sf_xdvdfs_save(img, &ent, dest, result->error, sizeof result->error) != 0) {
        unlink(dest);
        sf_xdvdfs_close(img);
        return -1;
    }
    result->files++;
    if (progress) {
        progress("XB_HostPC/SF33RDXB.AFS", user);
    }
    if (sf_xdvdfs_find(img, "XB_HostPC/ST3_3_OP.aix", &ent, result->error, sizeof result->error) == 0) {
        if (sf_join(dest, sizeof dest, install, "data/audio/ST3_3_OP.aix") != 0 || mkdir_parent(dest, result->error, sizeof result->error) != 0 || sf_xdvdfs_save(img, &ent, dest, result->error, sizeof result->error) != 0) {
            sf_xdvdfs_close(img);
            return -1;
        }
        result->files++;
        if (progress) {
            progress("XB_HostPC/ST3_3_OP.aix", user);
        }
    }
    if (sf_xdvdfs_find(img, "sf33rd.xbe", &ent, result->error, sizeof result->error) == 0) {
        if (sf_join(dest, sizeof dest, install, XBE_TMP) != 0 || mkdir_parent(dest, result->error, sizeof result->error) != 0 || sf_xdvdfs_save(img, &ent, dest, result->error, sizeof result->error) != 0) {
            sf_xdvdfs_close(img);
            return -1;
        }
    }
    result->error[0] = 0;
    memset(&walk, 0, sizeof walk);
    walk.img = img;
    walk.install = install;
    walk.files = &result->files;
    walk.progress = progress;
    walk.user = user;
    walk.err = result->error;
    walk.err_n = sizeof result->error;
    walk.dest_rel = "data/fonts";
    if (sf_xdvdfs_walk(img, "media/Fonts", on_iso_file, &walk, result->error, sizeof result->error) < 0 || walk.failed) {
        sf_xdvdfs_close(img);
        return -1;
    }
    walk.shaders = 1;
    walk.dest_rel = "data/shaders/nv2a";
    if (sf_xdvdfs_walk(img, "media/Shaders", on_iso_file, &walk, result->error, sizeof result->error) < 0 || walk.failed) {
        sf_xdvdfs_close(img);
        return -1;
    }
    sf_xdvdfs_close(img);
    return finish_import(install, tool, progress, user, result);
}

static int find_disc_root(const char *dir, char *out, size_t n, int depth, int *ps2)
{
    char probe[4096];
    DIR *d;
    struct dirent *de;
    struct stat st;
    if (ci_resolve(dir, "XB_HostPC/SF33RDXB.AFS", probe, sizeof probe) == 0 && stat(probe, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        if (strlen(dir) >= n) {
            return -1;
        }
        memcpy(out, dir, strlen(dir) + 1);
        return 0;
    }
    if (ci_resolve(dir, "SYSTEM.CNF", probe, sizeof probe) == 0) {
        *ps2 = 1;
    }
    if (depth >= 5) {
        return 1;
    }
    d = opendir(dir);
    if (!d) {
        return 1;
    }
    while ((de = readdir(d)) != NULL) {
        char child[4096];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (sf_join(child, sizeof child, dir, de->d_name) != 0) {
            continue;
        }
        if (lstat(child, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (find_disc_root(child, out, n, depth + 1, ps2) == 0) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return 1;
}

static int find_payload(const char *dir, char *out, size_t n, int depth, int *left)
{
    DIR *d;
    struct dirent *de;
    if (*left <= 0 || depth > 5) {
        return 1;
    }
    d = opendir(dir);
    if (!d) {
        return 1;
    }
    while ((de = readdir(d)) != NULL && *left > 0) {
        char child[4096];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (sf_join(child, sizeof child, dir, de->d_name) != 0) {
            continue;
        }
        if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (find_payload(child, out, n, depth + 1, left) == 0) {
                closedir(d);
                return 0;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (ends_ci(de->d_name, ".iso") || ends_ci(de->d_name, ".zip") || ends_ci(de->d_name, ".7z")) {
            if (strlen(child) >= n) {
                continue;
            }
            memcpy(out, child, strlen(child) + 1);
            (*left)--;
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return 1;
}

static int import_any(const char *source, const char *install, const char *tool, SfExtractProgress progress, void *user, SfExtractResult *result, int depth);

static int suffix_prefix(const char *path, const char *suffix, char *prefix, size_t n)
{
    size_t pl = strlen(path);
    size_t sl = strlen(suffix);
    size_t pre;
    if (pl < sl || strcasecmp(path + pl - sl, suffix) != 0) {
        return 0;
    }
    if (pl == sl) {
        prefix[0] = 0;
        return 1;
    }
    if (path[pl - sl - 1] != '/') {
        return 0;
    }
    pre = pl - sl - 1;
    if (pre >= n) {
        return 0;
    }
    memcpy(prefix, path, pre);
    prefix[pre] = 0;
    return 1;
}

struct ArchivePlan {
    int afs;
    int ps2;
    char prefix[1024];
    char iso_name[1024];
    const char *install;
    char error[512];
};

static const char *rel_under(const char *name, const char *prefix)
{
    size_t n;
    if (!prefix[0]) {
        return name;
    }
    n = strlen(prefix);
    if (strncasecmp(name, prefix, n) != 0 || name[n] != '/') {
        return NULL;
    }
    return name + n + 1;
}

static int visit_plan(const char *name, int is_dir, uint64_t size, void *user)
{
    struct ArchivePlan *plan = user;
    (void)size;
    if (is_dir) {
        return 0;
    }
    if (suffix_prefix(name, "XB_HostPC/SF33RDXB.AFS", plan->prefix, sizeof plan->prefix)) {
        plan->afs = 1;
    }
    if (eq_ci(name, "SYSTEM.CNF") || ends_ci(name, "/SYSTEM.CNF") || ends_ci(name, "/THIRD/SF33RD.AFS")) {
        plan->ps2 = 1;
    }
    if (plan->iso_name[0] == 0 && ends_ci(name, ".iso")) {
        snprintf(plan->iso_name, sizeof plan->iso_name, "%s", name);
    }
    return 0;
}

static int map_xbox(const char *name, int is_dir, char *dest, size_t dest_n, void *user)
{
    struct ArchivePlan *plan = user;
    const char *rel;
    if (is_dir || !plan->afs) {
        return 0;
    }
    rel = rel_under(name, plan->prefix);
    if (!rel || has_cvs(rel)) {
        return 0;
    }
    if (eq_ci(rel, "XB_HostPC/SF33RDXB.AFS")) {
        return sf_join(dest, dest_n, plan->install, "data/afs/SF33RDXB.AFS") == 0 ? 1 : -1;
    }
    if (eq_ci(rel, "XB_HostPC/ST3_3_OP.aix")) {
        return sf_join(dest, dest_n, plan->install, "data/audio/ST3_3_OP.aix") == 0 ? 1 : -1;
    }
    if (eq_ci(rel, "sf33rd.xbe")) {
        return sf_join(dest, dest_n, plan->install, XBE_TMP) == 0 ? 1 : -1;
    }
    if (strncasecmp(rel, "media/Fonts/", 12) == 0) {
        char rest[4096];
        if (sf_join(rest, sizeof rest, plan->install, "data/fonts") != 0) {
            return -1;
        }
        return sf_join(dest, dest_n, rest, rel + 12) == 0 ? 1 : -1;
    }
    if (strncasecmp(rel, "media/Shaders/", 14) == 0 && (ends_ci(rel, ".xvu") || ends_ci(rel, ".xpu"))) {
        char rest[4096];
        if (sf_join(rest, sizeof rest, plan->install, "data/shaders/nv2a") != 0) {
            return -1;
        }
        return sf_join(dest, dest_n, rest, rel + 14) == 0 ? 1 : -1;
    }
    return 0;
}

static int map_one_iso(const char *name, int is_dir, char *dest, size_t dest_n, void *user)
{
    struct ArchivePlan *plan = user;
    if (is_dir || strcmp(name, plan->iso_name) != 0) {
        return 0;
    }
    return sf_join(dest, dest_n, plan->install, "data/.import-tmp/disc.iso") == 0 ? 1 : -1;
}

static int is_archive_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t b[8];
    size_t n;
    if (!fp) {
        return 0;
    }
    n = fread(b, 1, sizeof b, fp);
    fclose(fp);
    if (n >= 2 && b[0] == 'P' && b[1] == 'K') {
        return 1;
    }
    if (n >= 6 && b[0] == 0x37 && b[1] == 0x7a && b[2] == 0xbc && b[3] == 0xaf && b[4] == 0x27 && b[5] == 0x1c) {
        return 1;
    }
    if (ends_ci(path, ".zip") || ends_ci(path, ".7z") || ends_ci(path, ".7zip")) {
        return 1;
    }
    return 0;
}

static int import_archive(const char *source, const char *install, const char *tool, SfExtractProgress progress, void *user, SfExtractResult *result, int depth)
{
    struct ArchivePlan plan;
    int written = 0;
    memset(&plan, 0, sizeof plan);
    plan.install = install;
    if (sf_archive_visit(source, visit_plan, &plan, result->error, sizeof result->error) != 0) {
        return -1;
    }
    if (!plan.afs && plan.iso_name[0] && depth < 2) {
        char iso_path[4096];
        char temp[4096];
        int rc;
        if (sf_join(temp, sizeof temp, install, "data/.import-tmp") != 0 || sf_mkdir_p(temp, result->error, sizeof result->error) != 0) {
            return -1;
        }
        if (sf_archive_export(source, map_one_iso, &plan, progress, user, &written, result->error, sizeof result->error) != 0) {
            sf_rm_rf(temp);
            return -1;
        }
        if (sf_join(iso_path, sizeof iso_path, install, "data/.import-tmp/disc.iso") != 0) {
            sf_rm_rf(temp);
            return -1;
        }
        rc = import_any(iso_path, install, tool, progress, user, result, depth + 1);
        sf_rm_rf(temp);
        return rc;
    }
    if (!plan.afs) {
        if (plan.ps2) {
            sf_set_err(result->error, sizeof result->error, "This is not the Xbox version of the game.");
        } else {
            sf_set_err(result->error, sizeof result->error, "No Xbox disc data was found. Choose an Xbox XDVDFS image, or a zip or 7z of that Xbox disc.");
        }
        return -1;
    }
    if (sf_archive_export(source, map_xbox, &plan, progress, user, &written, result->error, sizeof result->error) != 0) {
        return -1;
    }
    result->files += written;
    if (!afs_ready(install)) {
        sf_set_err(result->error, sizeof result->error, "The archive did not contain the Xbox 3rd Strike files.");
        return -1;
    }
    return finish_import(install, tool, progress, user, result);
}

static int import_any(const char *source, const char *install, const char *tool, SfExtractProgress progress, void *user, SfExtractResult *result, int depth)
{
    struct stat st;
    char root[4096];
    int ps2 = 0;
    if (stat(source, &st) != 0) {
        sf_set_err(result->error, sizeof result->error, "cannot read %s", source);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (find_disc_root(source, root, sizeof root, 0, &ps2) == 0) {
            return import_files_from_dir(root, install, tool, progress, user, result);
        }
        if (depth < 2) {
            char payload[4096];
            int left = 8;
            if (find_payload(source, payload, sizeof payload, 0, &left) == 0) {
                return import_any(payload, install, tool, progress, user, result, depth + 1);
            }
        }
        if (ps2) {
            sf_set_err(result->error, sizeof result->error, "This is not the Xbox version of the game.");
        } else {
            sf_set_err(result->error, sizeof result->error, "No Xbox disc data was found. Choose an Xbox XDVDFS image, a zip or 7z of that Xbox disc, or the extracted Xbox folder.");
        }
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        sf_set_err(result->error, sizeof result->error, "not an Xbox disc");
        return -1;
    }
    if (is_archive_file(source)) {
        return import_archive(source, install, tool, progress, user, result, depth);
    }
    return import_iso(source, install, tool, progress, user, result);
}

int sf_extract(const char *source, const char *install_dir, const char *shader_tool, SfExtractProgress progress, void *user, SfExtractResult *result)
{
    char data[4096];
    if (!result) {
        return -1;
    }
    memset(result, 0, sizeof *result);
    if (!source || !install_dir) {
        sf_set_err(result->error, sizeof result->error, "missing source or install directory");
        return -1;
    }
    if (sf_join(data, sizeof data, install_dir, "data") != 0 || sf_mkdir_p(data, result->error, sizeof result->error) != 0) {
        return -1;
    }
    return import_any(source, install_dir, shader_tool, progress, user, result, 0);
}

int sf_retry_shaders(const char *install_dir, const char *shader_tool, SfExtractResult *result)
{
    char spirv[4096];
    int existing = 0;
    if (!result) {
        return -1;
    }
    memset(result, 0, sizeof *result);
    if (!install_dir || !shader_tool || !sf_data_ready(install_dir)) {
        return 0;
    }
    if (sf_join(spirv, sizeof spirv, install_dir, "data/shaders/spirv") != 0) {
        return -1;
    }
    if (count_ext(spirv, "", &existing) != 0) {
        return -1;
    }
    if (existing > 0) {
        return 0;
    }
    if (finish_shaders(install_dir, shader_tool, result) != 0) {
        return -1;
    }
    return 0;
}
