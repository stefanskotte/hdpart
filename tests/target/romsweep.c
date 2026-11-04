/* Runs the shipped safety_driver_is_ide on every scsi.device module in the Kickstart ROMs under
   the given directories, and checks the answer against the hardware base found in the module.
   Reads images of 256K to 1M in steps of 256K, including those with KickIt's 8-byte header or bytes after the
   end, and those built to run at another address such as ReKick's $200000. Byte-swapped images and split
   hi/lo files are not read.

     cc -std=c99 -Wall -Wextra -o out/romsweep tests/target/romsweep.c src/safety.c
     out/romsweep [-v] dir...          or set QUICK_ROM_DIRS=dir:dir

   Prints a Markdown table. Exits 1 if any answer disagrees with its base, or a module cannot be classified. */
#define _XOPEN_SOURCE 600
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/safety.h"

#define MAX_GROUPS 64
#define MAX_HASHES 64
typedef struct {
    char build[16], name[24], id[48], at[8];
    int answer, files, n_hash;
    uint64_t hash[MAX_HASHES];
} Group;

static Group g_groups[MAX_GROUPS];
static int g_n, g_verbose, g_images, g_bad;
static uint8_t g_rom[1 << 20];

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* From the hardware base addresses alone, independently of the function under test. */
static const char *build_of(const uint8_t *m, uint32_t len)
{
    int sdmac = 0, ncr = 0, gayle = 0, a4000 = 0;
    uint32_t i;
    for (i = 0; i + 4 <= len; i += 2) {
        uint32_t v = rd32(m + i);
        if (v == 0x00DD0000UL) sdmac = 1;
        if (v == 0x00DD0040UL) ncr = 1;
        if (v == 0x00DA2000UL) gayle = 1;
        if (v == 0x00DD2020UL) a4000 = 1;
    }
    if (sdmac + ncr + gayle + a4000 != 1) return "unknown";
    return sdmac ? "SDMAC" : ncr ? "NCR" : gayle ? "Gayle IDE" : "A4000 IDE";
}

static void record(const char *path, const char *build, const char *name, const char *id, const char *at,
                   int answer, const uint8_t *m, uint32_t len)
{
    Group *g = 0;
    uint64_t h = 14695981039346656037ULL;
    uint32_t i;
    int k;
    for (i = 0; i < len; i++) h = (h ^ m[i]) * 1099511628211ULL;
    for (k = 0; k < g_n; k++)
        if (!strcmp(g_groups[k].build, build) && !strcmp(g_groups[k].name, name) &&
            !strcmp(g_groups[k].id, id) && !strcmp(g_groups[k].at, at) && g_groups[k].answer == answer) g = &g_groups[k];
    if (!g) {
        if (g_n == MAX_GROUPS) { fprintf(stderr, "too many kinds of module\n"); exit(2); }
        g = &g_groups[g_n++];
        strncpy(g->build, build, sizeof(g->build) - 1);
        strncpy(g->name, name, sizeof(g->name) - 1);
        strncpy(g->id, id, sizeof(g->id) - 1);
        strncpy(g->at, at, sizeof(g->at) - 1);
        g->answer = answer;
    }
    g->files++;
    for (k = 0; k < g->n_hash && g->hash[k] != h; k++) { }
    if (k == g->n_hash && g->n_hash < MAX_HASHES) g->hash[g->n_hash++] = h;
    if (g_verbose) printf("%d\t%s\t%s\t%s\t%s\t%s\n", answer, build, name, id, at, path);
}

static void sweep_image(const char *path, const uint8_t *d, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 26 <= n; i += 2) {
        uint32_t self, base, nm, idp, end, len;
        const char *name, *build;
        char id[48] = "", at[8];
        int answer;
        if (d[i] != 0x4A || d[i + 1] != 0xFC) continue;
        self = rd32(d + i + 2);
        if (self < i || ((self - i) & 0xFFFF)) continue;
        base = self - i;
        if (base + n > 0x1000000UL) continue;
        nm = rd32(d + i + 14) - base;
        if (nm >= n) continue;
        name = (const char *)d + nm;
        if (strcmp(name, "scsi.device") && strcmp(name, "NCR scsi.device")) continue;
        idp = rd32(d + i + 18) - base;
        if (idp < n) {
            strncpy(id, (const char *)d + idp, sizeof(id) - 1);
            id[strcspn(id, " \r\n")] = 0;   /* name only */
            if (strchr((const char *)d + idp, ' ')) {
                size_t l = strlen(id);
                const char *v = strchr((const char *)d + idp, ' ') + 1;
                id[l++] = ' ';
                while (l < sizeof(id) - 1 && *v && *v != ' ' && *v != '\r' && *v != '\n') id[l++] = *v++;
                id[l] = 0;
            }
        }
        end = rd32(d + i + 6);
        if (end <= self || end - base > n) {
            printf("BAD END SKIP: %s in %s\n", name, path);
            g_bad++;
            continue;
        }
        len = end - self;
        answer = safety_driver_is_ide(d + i, len);
        build = build_of(d + i, len);
        if (!strcmp(build, "unknown") || answer != (strstr(build, "IDE") != 0)) {
            printf("DISAGREES: answer %d, build %s, %s in %s\n", answer, build, name, path);
            g_bad++;
        }
        snprintf(at, sizeof(at), "$%06X", (unsigned)base);
        record(path, build, name, id[0] ? id : "(no ID string)", at, answer, d + i, len);
    }
}

static int visit(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    FILE *f;
    uint32_t extra, image, skip;
    (void)ftw;
    if (type != FTW_F || st->st_size < 0x40000 || st->st_size > 0x110000) return 0;
    extra = (uint32_t)st->st_size % 0x40000;
    image = (uint32_t)st->st_size - extra;
    if (extra >= 0x10000 || image > (uint32_t)sizeof(g_rom)) return 0;
    skip = extra == 8 ? 8 : 0;                 /* KickIt's header; anything else is trailing */
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, (long)skip, SEEK_SET) == 0 && fread(g_rom, 1, image, f) == image) {
        g_images++;
        sweep_image(path, g_rom, image);
    }
    fclose(f);
    return 0;
}

static int by_group(const void *a, const void *b)
{
    const Group *x = a, *y = b;
    int c = strcmp(x->build, y->build);
    if (!c) c = strcmp(x->name, y->name);
    if (!c) c = strcmp(x->id, y->id);
    return c ? c : strcmp(x->at, y->at);
}

int main(int argc, char **argv)
{
    int i, a = 1, dirs = 0, files = 0;
    char *env;
    if (a < argc && !strcmp(argv[a], "-v")) { g_verbose = 1; a++; }
    for (i = a; i < argc; i++, dirs++) nftw(argv[i], visit, 16, FTW_PHYS);
    env = getenv("QUICK_ROM_DIRS");
    if (!dirs && env) {
        char *d;
        for (d = strtok(env, ":"); d; d = strtok(0, ":"), dirs++) nftw(d, visit, 16, FTW_PHYS);
    }
    if (!dirs) { fprintf(stderr, "usage: romsweep [-v] dir...   or set QUICK_ROM_DIRS=dir:dir\n"); return 2; }
    qsort(g_groups, (size_t)g_n, sizeof(Group), by_group);
    printf("| Base found | Module | ID string | Image built for | Files | Different modules | Answer |\n|---|---|---|---|---|---|---|\n");
    for (i = 0; i < g_n; i++) {
        printf("| %s | `%s` | %s | %s | %d | %d | %d |\n", g_groups[i].build, g_groups[i].name, g_groups[i].id, g_groups[i].at,
               g_groups[i].files, g_groups[i].n_hash, g_groups[i].answer);
        files += g_groups[i].files;
    }
    printf("\n%d images read, %d modules, %d problems.\n", g_images, files, g_bad);
    return g_bad ? 1 : 0;
}
