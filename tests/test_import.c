#include "iso/afs.h"
#include "iso/extract.h"
#include "iso/xdvdfs.h"

#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    } else {
        printf("ok %s\n", msg);
    }
}

static void wu16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wu32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_ent(uint8_t *dir, uint32_t off, uint16_t left, uint16_t right, uint32_t sector, uint32_t size, uint8_t attr, const char *name)
{
    uint8_t *p = dir + off;
    uint8_t nlen = (uint8_t)strlen(name);
    wu16(p, left);
    wu16(p + 2, right);
    wu32(p + 4, sector);
    wu32(p + 8, size);
    p[12] = attr;
    p[13] = nlen;
    memcpy(p + 14, name, nlen);
}

/* A stand-in sf33rd.xbe: the header, then the 21 sound banks the import
 * reads out of it ("SDBK" version 11, named SE and pl00 to pl19, no RPC
 * table, 0x38 bytes each). */
#define FAKE_XBE_N (0x1000u + 21u * 0x38u)

static void fake_xbe(uint8_t *x)
{
    int i;
    memset(x, 0, FAKE_XBE_N);
    memcpy(x, "XBEH", 4);
    for (i = 0; i < 21; i++) {
        uint8_t *b = x + 0x1000u + (size_t)i * 0x38u;
        memcpy(b, "SDBK", 4);
        wu16(b + 4, 11);
        wu32(b + 0x14, 0x38);
        if (i == 0) {
            memcpy(b + 0x28, "SE", 2);
        } else {
            b[0x28] = 'p';
            b[0x29] = 'l';
            b[0x2A] = (uint8_t)('0' + (i - 1) / 10);
            b[0x2B] = (uint8_t)('0' + (i - 1) % 10);
        }
    }
}

static void write_fake_xbe(const char *dir)
{
    static uint8_t x[FAKE_XBE_N];
    char path[4096];
    FILE *fp;
    fake_xbe(x);
    snprintf(path, sizeof path, "%s/sf33rd.xbe", dir);
    fp = fopen(path, "wb");
    if (fp) {
        fwrite(x, 1, sizeof x, fp);
        fclose(fp);
    }
}

static uint8_t *build_iso(size_t *out_n)
{
    const uint32_t base = 4096;
    const uint32_t sectors = 47;
    size_t n = base + (size_t)sectors * 2048u;
    uint8_t *img = calloc(1, n);
    uint8_t *vol;
    uint8_t *root;
    uint8_t *host;
    uint8_t *media;
    uint8_t *shaders;
    uint8_t *fonts;
    if (!img) {
        return NULL;
    }
    memset(img, 0x11, base);
    vol = img + base + 32u * 2048u;
    memcpy(vol, "MICROSOFT*XBOX*MEDIA", 20);
    memcpy(vol + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    wu32(vol + 0x14, 34);
    wu32(vol + 0x18, 2048);
    root = img + base + 34u * 2048u;
    put_ent(root, 0, 6, 11, 35, 2048, 0x10, "XB_HostPC");
    put_ent(root, 24, 0, 16, 36, 2048, 0x10, "media");
    put_ent(root, 44, 0, 0, 42, 3, 0x80, "zz.txt");
    put_ent(root, 64, 0, 0, 44, FAKE_XBE_N, 0x80, "sf33rd.xbe");
    host = img + base + 35u * 2048u;
    put_ent(host, 0, 0, 7, 39, 4, 0x80, "SF33RDXB.AFS");
    put_ent(host, 28, 0, 0, 41, 4, 0x80, "ST3_3_OP.aix");
    media = img + base + 36u * 2048u;
    put_ent(media, 0, 6, 0, 37, 2048, 0x10, "Shaders");
    put_ent(media, 24, 0, 0, 38, 2048, 0x10, "Fonts");
    shaders = img + base + 37u * 2048u;
    put_ent(shaders, 0, 0, 0, 40, 4, 0x80, "dummy.xvu");
    fonts = img + base + 38u * 2048u;
    put_ent(fonts, 0, 0, 0, 43, 4, 0x80, "note.txt");
    memcpy(img + base + 39u * 2048u, "AFS!", 4);
    memcpy(img + base + 40u * 2048u, "XVU!", 4);
    memcpy(img + base + 41u * 2048u, "AIX!", 4);
    memcpy(img + base + 42u * 2048u, "ZZ!", 3);
    memcpy(img + base + 43u * 2048u, "FONT", 4);
    fake_xbe(img + base + 44u * 2048u);
    *out_n = n;
    return img;
}

static uint32_t crc32(const void *data, size_t n)
{
    static uint32_t table[256];
    static int ready;
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!ready) {
        uint32_t nibble;
        for (nibble = 0; nibble < 256; nibble++) {
            uint32_t v = nibble;
            int b;
            for (b = 0; b < 8; b++) {
                v = (v & 1u) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            }
            table[nibble] = v;
        }
        ready = 1;
    }
    for (i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void zip_file(FILE *fp, const char *name, const void *data, size_t n, uint32_t *offset, uint8_t *cd, size_t *cd_n, int *count)
{
    uint32_t crc = crc32(data, n);
    uint8_t local[30];
    uint8_t central[46];
    uint16_t nlen = (uint16_t)strlen(name);
    memset(local, 0, sizeof local);
    wu32(local, 0x04034b50u);
    wu16(local + 4, 20);
    wu16(local + 8, 0);
    wu32(local + 14, crc);
    wu32(local + 18, (uint32_t)n);
    wu32(local + 22, (uint32_t)n);
    wu16(local + 26, nlen);
    fwrite(local, 1, sizeof local, fp);
    fwrite(name, 1, nlen, fp);
    fwrite(data, 1, n, fp);
    memset(central, 0, sizeof central);
    wu32(central, 0x02014b50u);
    wu16(central + 4, 20);
    wu16(central + 6, 20);
    wu32(central + 16, crc);
    wu32(central + 20, (uint32_t)n);
    wu32(central + 24, (uint32_t)n);
    wu16(central + 28, nlen);
    wu32(central + 42, *offset);
    memcpy(cd + *cd_n, central, sizeof central);
    memcpy(cd + *cd_n + sizeof central, name, nlen);
    *cd_n += sizeof central + nlen;
    *offset += 30u + nlen + (uint32_t)n;
    (*count)++;
}

static int write_tree_zip(const char *path, const uint8_t *iso, size_t iso_n, int with_iso, int ps2)
{
    FILE *fp = fopen(path, "wb");
    uint8_t cd[4096];
    size_t cd_n = 0;
    uint32_t offset = 0;
    int count = 0;
    uint8_t end[22];
    if (!fp) {
        return -1;
    }
    if (ps2) {
        zip_file(fp, "SYSTEM.CNF", "BOOT2", 5, &offset, cd, &cd_n, &count);
        zip_file(fp, "THIRD/SF33RD.AFS", "PS2!", 4, &offset, cd, &cd_n, &count);
    } else if (with_iso) {
        zip_file(fp, "disc.iso", iso, iso_n, &offset, cd, &cd_n, &count);
    } else {
        static uint8_t x[FAKE_XBE_N];
        fake_xbe(x);
        zip_file(fp, "sf33rd.xbe", x, sizeof x, &offset, cd, &cd_n, &count);
        zip_file(fp, "XB_HostPC/SF33RDXB.AFS", "AFS!", 4, &offset, cd, &cd_n, &count);
        zip_file(fp, "XB_HostPC/ST3_3_OP.aix", "AIX!", 4, &offset, cd, &cd_n, &count);
        zip_file(fp, "media/Fonts/note.txt", "FONT", 4, &offset, cd, &cd_n, &count);
        zip_file(fp, "media/Shaders/dummy.xvu", "XVU!", 4, &offset, cd, &cd_n, &count);
        zip_file(fp, "hyper/movie.sfd", "MOV!", 4, &offset, cd, &cd_n, &count);
    }
    fwrite(cd, 1, cd_n, fp);
    memset(end, 0, sizeof end);
    wu32(end, 0x06054b50u);
    wu16(end + 8, (uint16_t)count);
    wu16(end + 10, (uint16_t)count);
    wu32(end + 12, (uint32_t)cd_n);
    wu32(end + 16, offset);
    fwrite(end, 1, sizeof end, fp);
    fclose(fp);
    return 0;
}

static int read_file(const char *path, char *dst, size_t n)
{
    FILE *fp = fopen(path, "rb");
    size_t got;
    if (!fp) {
        return -1;
    }
    got = fread(dst, 1, n, fp);
    fclose(fp);
    return got == n ? 0 : -1;
}

static char *make_dir(void)
{
    char tmpl[] = "/tmp/sf33rd-test-XXXXXX";
    return mkdtemp(tmpl) ? strdup(tmpl) : NULL;
}

static void check_ready(const char *install, int expect_shader_fail, const char *msg)
{
    char path[4096];
    char buf[8];
    struct stat st;
    snprintf(path, sizeof path, "%s/data/afs/SF33RDXB.AFS", install);
    expect(read_file(path, buf, 4) == 0 && memcmp(buf, "AFS!", 4) == 0, msg);
    snprintf(path, sizeof path, "%s/hyper/movie.sfd", install);
    expect(stat(path, &st) != 0, "movies stay out");
    snprintf(path, sizeof path, "%s/data/shaders/nv2a/dummy.xvu", install);
    expect(read_file(path, buf, 4) == 0 && memcmp(buf, "XVU!", 4) == 0, "shader copied");
    snprintf(path, sizeof path, "%s/data/audio/sdbk/20.sdbk", install);
    expect(read_file(path, buf, 4) == 0 && memcmp(buf, "SDBK", 4) == 0, "sound banks from the xbe");
    snprintf(path, sizeof path, "%s/data/.import-tmp/sf33rd.xbe", install);
    expect(stat(path, &st) != 0, "the xbe is not kept");
    if (expect_shader_fail) {
        snprintf(path, sizeof path, "%s/data/shaders/spirv/dummy.spv", install);
        expect(stat(path, &st) != 0, "bad shader is not spirv");
    }
}

static int run_ok(const char *source, const char *install, const char *tool, int shader_fail, const char *msg)
{
    SfExtractResult result;
    int rc = sf_extract(source, install, tool, NULL, NULL, &result);
    expect(rc == 0 && sf_data_ready(install), msg);
    if (rc == 0) {
        check_ready(install, shader_fail, msg);
        if (shader_fail) {
            expect(result.spirv_failed > 0, "conversion failure is recorded");
        }
    }
    return rc;
}

int main(void)
{
    size_t iso_n = 0;
    uint8_t *iso = build_iso(&iso_n);
    char *dir;
    char path[4096];
    SfXdvdfs *img;
    SfXdvdfsEnt ent;
    char err[512];
    SfExtractResult result;
    expect(iso != NULL, "synthetic xbox image");
    snprintf(path, sizeof path, "/tmp/sf33rd-test-image.iso");
    {
        FILE *fp = fopen(path, "wb");
        expect(fp && fwrite(iso, 1, iso_n, fp) == iso_n, "write iso");
        if (fp) {
            fclose(fp);
        }
    }
    img = sf_xdvdfs_open(path, err, sizeof err);
    expect(img != NULL, "open xbox xdvdfs");
    if (img) {
        expect(sf_xdvdfs_find(img, "XB_HostPC/SF33RDXB.AFS", &ent, err, sizeof err) == 0 && ent.size == 4, "find afs");
        expect(sf_xdvdfs_find(img, "media/Shaders/dummy.xvu", &ent, err, sizeof err) == 0, "find shader");
        expect(sf_xdvdfs_find(img, "zz.txt", &ent, err, sizeof err) == 0 && ent.size == 3, "right child");
        expect(sf_xdvdfs_find(img, "media", &ent, err, sizeof err) == 0 && ent.is_dir, "left child");
        sf_xdvdfs_close(img);
    }
    dir = make_dir();
    expect(dir != NULL, "temp install");
    if (dir) {
        expect(run_ok(path, dir, SF_SHADER_TOOL, 1, "extract xbox iso") == 0, "iso import");
        free(dir);
    }
    dir = make_dir();
    snprintf(path, sizeof path, "/tmp/sf33rd-tree.zip");
    expect(write_tree_zip(path, iso, iso_n, 0, 0) == 0, "write zip");
    if (dir) {
        run_ok(path, dir, NULL, 0, "extract zip of xbox tree");
        free(dir);
    }
    dir = make_dir();
    snprintf(path, sizeof path, "/tmp/sf33rd-iso.zip");
    {
        FILE *fp = fopen("/tmp/sf33rd-test-image.iso", "rb");
        expect(fp != NULL, "reread iso");
        if (fp) {
            fclose(fp);
        }
    }
    expect(write_tree_zip(path, iso, iso_n, 1, 0) == 0, "write iso zip");
    if (dir) {
        run_ok(path, dir, NULL, 0, "extract zip of xbox iso");
        free(dir);
    }
    snprintf(path, sizeof path, "/tmp/sf33rd-ps2.zip");
    expect(write_tree_zip(path, iso, iso_n, 0, 1) == 0, "write ps2 zip");
    dir = make_dir();
    if (dir) {
        int rc = sf_extract(path, dir, NULL, NULL, NULL, &result);
        expect(rc != 0 && strstr(result.error, "Xbox") != NULL, "reject non-xbox archive");
        expect(!sf_data_ready(dir), "ps2 archive writes no afs");
        free(dir);
    }
    dir = make_dir();
    if (dir) {
        FILE *fp;
        snprintf(path, sizeof path, "%s/junk.bin", dir);
        fp = fopen(path, "wb");
        fwrite("nope", 1, 4, fp);
        fclose(fp);
        expect(sf_extract(path, dir, NULL, NULL, NULL, &result) != 0, "reject a file that is not an xbox iso");
        free(dir);
    }
    {
        char src[] = "/tmp/sf33rd-test-XXXXXX";
        char *tree = mkdtemp(src);
        char *install;
        expect(tree != NULL, "extracted folder");
        if (tree) {
            char file[4096];
            write_fake_xbe(tree);
            snprintf(file, sizeof file, "%s/XB_HostPC", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/XB_HostPC/SF33RDXB.AFS", tree);
            {
                FILE *fp = fopen(file, "wb");
                fwrite("AFS!", 1, 4, fp);
                fclose(fp);
            }
            snprintf(file, sizeof file, "%s/media/Shaders", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/media", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/media/Shaders", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/media/Shaders/dummy.xvu", tree);
            {
                FILE *fp = fopen(file, "wb");
                fwrite("XVU!", 1, 4, fp);
                fclose(fp);
            }
            install = make_dir();
            if (install) {
                run_ok(tree, install, NULL, 0, "extract xbox folder");
                free(install);
            }
        }
    }
    {
        char staged[] = "/tmp/sf33rd-test-XXXXXX";
        char *tree = mkdtemp(staged);
        char archive[] = "/tmp/sf33rd-tree.7z";
        if (tree) {
            pid_t pid;
            char *argv[] = {"7z", "a", "-bd", "-y", archive, "XB_HostPC", "media", "sf33rd.xbe", NULL};
            char file[4096];
            int status = 0;
            write_fake_xbe(tree);
            snprintf(file, sizeof file, "%s/XB_HostPC", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/XB_HostPC/SF33RDXB.AFS", tree);
            {
                FILE *fp = fopen(file, "wb");
                fwrite("AFS!", 1, 4, fp);
                fclose(fp);
            }
            snprintf(file, sizeof file, "%s/media/Shaders", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/media", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/media/Shaders", tree);
            mkdir(file, 0755);
            snprintf(file, sizeof file, "%s/media/Shaders/dummy.xvu", tree);
            {
                FILE *fp = fopen(file, "wb");
                fwrite("XVU!", 1, 4, fp);
                fclose(fp);
            }
            if (chdir(tree) == 0 && posix_spawnp(&pid, "7z", NULL, NULL, argv, environ) == 0) {
                waitpid(pid, &status, 0);
                expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "write 7z");
                dir = make_dir();
                if (dir) {
                    run_ok(archive, dir, NULL, 0, "extract 7z of xbox tree");
                    free(dir);
                }
            } else {
                expect(0, "spawn 7z");
            }
        }
    }
    {
        char tmpl[] = "/tmp/sf33rd-afs-XXXXXX";
        char *dir = mkdtemp(tmpl);
        char afs_path[256];
        char files[256];
        char index[256];
        char got[8];
        unsigned char image[168];
        int written = 0;
        char err[128];
        FILE *fp;
        expect(dir != NULL, "afs temp");
        if (dir) {
            memset(image, 0, sizeof image);
            memcpy(image, "AFS", 3);
            wu32(image + 4, 2);
            wu32(image + 8, 64);
            wu32(image + 12, 4);
            wu32(image + 16, 68);
            wu32(image + 20, 4);
            wu32(image + 24, 72);
            wu32(image + 28, 96);
            memcpy(image + 64, "ONE!", 4);
            memcpy(image + 68, "TWO!", 4);
            memcpy(image + 72, "One.bin", 7);
            memcpy(image + 120, "Two.bin", 7);
            snprintf(afs_path, sizeof afs_path, "%s/pack.afs", dir);
            snprintf(files, sizeof files, "%s/files", dir);
            snprintf(index, sizeof index, "%s/index", dir);
            fp = fopen(afs_path, "wb");
            expect(fp && fwrite(image, 1, sizeof image, fp) == sizeof image, "write tiny afs");
            if (fp) {
                fclose(fp);
            }
            expect(sf_afs_unpack(afs_path, files, index, NULL, NULL, &written, err, sizeof err) == 0 && written == 2, "unpack afs");
            snprintf(afs_path, sizeof afs_path, "%s/files/One.bin", dir);
            expect(read_file(afs_path, got, 4) == 0 && memcmp(got, "ONE!", 4) == 0, "first afs file");
            snprintf(afs_path, sizeof afs_path, "%s/files/Two.bin", dir);
            expect(read_file(afs_path, got, 4) == 0 && memcmp(got, "TWO!", 4) == 0, "second afs file");
        }
    }
    free(iso);
    unlink("/tmp/sf33rd-test-image.iso");
    unlink("/tmp/sf33rd-tree.zip");
    unlink("/tmp/sf33rd-iso.zip");
    unlink("/tmp/sf33rd-ps2.zip");
    unlink("/tmp/sf33rd-tree.7z");
    return g_fail ? 1 : 0;
}
