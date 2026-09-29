#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512u
#define TOTAL_SECTORS 720u
#define IMAGE_SIZE (SECTOR_SIZE * TOTAL_SECTORS)
#define RESERVED_SECTORS 1u
#define FAT_COUNT 2u
#define SECTORS_PER_FAT 2u
#define ROOT_ENTRIES 112u
#define ROOT_SECTORS ((ROOT_ENTRIES * 32u + SECTOR_SIZE - 1u) / SECTOR_SIZE)
#define DATA_START_SECTOR (RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT + ROOT_SECTORS)
#define SECTORS_PER_CLUSTER 2u
#define CLUSTER_SIZE (SECTOR_SIZE * SECTORS_PER_CLUSTER)
#define FIRST_DATA_CLUSTER 2u
#define MEDIA_DESCRIPTOR 0xFDu

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    put16(p, (uint16_t)(value & 0xffffu));
    put16(p + 2, (uint16_t)(value >> 16));
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *fp;
    long length;
    uint8_t *data;

    *size_out = 0u;
    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL) { fclose(fp); return NULL; }
    if (length != 0 && fread(data, 1u, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size_out = (size_t)length;
    return data;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) return 0;
    if (size != 0u && fwrite(data, 1u, size, fp) != size) {
        fclose(fp);
        return 0;
    }
    if (fclose(fp) != 0) return 0;
    return 1;
}

static void fat12_set(uint8_t *fat, uint16_t cluster, uint16_t value)
{
    const size_t pos = (size_t)cluster + (size_t)(cluster / 2u);
    value &= 0x0fffu;
    if ((cluster & 1u) == 0u) {
        fat[pos] = (uint8_t)(value & 0xffu);
        fat[pos + 1u] = (uint8_t)((fat[pos + 1u] & 0xf0u) | ((value >> 8) & 0x0fu));
    } else {
        fat[pos] = (uint8_t)((fat[pos] & 0x0fu) | ((value << 4) & 0xf0u));
        fat[pos + 1u] = (uint8_t)(value >> 4);
    }
}

static void build_boot_sector(uint8_t *image)
{
    uint8_t *b = image;
    b[0] = 0xEBu;
    b[1] = 0x3Cu;
    b[2] = 0x90u;
    memcpy(b + 3, "MICRODOS", 8u);
    put16(b + 0x0Bu, SECTOR_SIZE);
    b[0x0Du] = SECTORS_PER_CLUSTER;
    put16(b + 0x0Eu, RESERVED_SECTORS);
    b[0x10u] = FAT_COUNT;
    put16(b + 0x11u, ROOT_ENTRIES);
    put16(b + 0x13u, TOTAL_SECTORS);
    b[0x15u] = MEDIA_DESCRIPTOR;
    put16(b + 0x16u, SECTORS_PER_FAT);
    b[0x1FEu] = 0x55u;
    b[0x1FFu] = 0xAAu;
}

/* Fixed, deterministic directory timestamp for every file in the image:
   1983-03-08 12:00:00 (MS-DOS 2.0's release month). DOS 2 DIR omits the
   date/time columns entirely when the date word is zero. */
#define STAMP_YEAR 1983u
#define STAMP_MONTH 3u
#define STAMP_DAY 8u
#define STAMP_HOUR 12u
#define STAMP_MINUTE 0u
#define STAMP_SECOND 0u
#define FAT_DATE(y, m, d) ((uint16_t)((((y) - 1980u) << 9) | ((m) << 5) | (d)))
#define FAT_TIME(h, m, s) ((uint16_t)(((h) << 11) | ((m) << 5) | ((s) / 2u)))

#define MAX_FILES 16u

typedef struct Fat12Builder {
    uint8_t *image;
    uint16_t next_cluster;
    unsigned next_root_entry;
} Fat12Builder;

static void builder_init(Fat12Builder *b, uint8_t *image)
{
    uint8_t *fat1 = image + RESERVED_SECTORS * SECTOR_SIZE;
    b->image = image;
    b->next_cluster = FIRST_DATA_CLUSTER;
    b->next_root_entry = 0u;
    fat1[0] = MEDIA_DESCRIPTOR;
    fat1[1] = 0xFFu;
    fat1[2] = 0xFFu;
}

static void builder_finish(Fat12Builder *b)
{
    uint8_t *fat1 = b->image + RESERVED_SECTORS * SECTOR_SIZE;
    uint8_t *fat2 = fat1 + SECTORS_PER_FAT * SECTOR_SIZE;
    memcpy(fat2, fat1, SECTORS_PER_FAT * SECTOR_SIZE);
}

/* Convert "NAME.EXT" to the 11-byte space-padded 8.3 directory form.
   Upper-cases; rejects names that do not fit. */
static int to_83(const char *name, uint8_t out[11])
{
    unsigned i = 0u;
    unsigned j;
    memset(out, ' ', 11u);
    for (j = 0u; name[i] != '\0' && name[i] != '.'; ++i, ++j) {
        if (j >= 8u) return 0;
        out[j] = (uint8_t)((name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i]);
    }
    if (j == 0u) return 0;
    if (name[i] == '.') {
        ++i;
        for (j = 8u; name[i] != '\0'; ++i, ++j) {
            if (j >= 11u || name[i] == '.') return 0;
            out[j] = (uint8_t)((name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i]);
        }
    }
    return 1;
}

static int install_file(Fat12Builder *b, const char *dos_name,
                        const uint8_t *bytes, size_t size, uint16_t *cluster_count_out,
                        uint16_t *first_cluster_out)
{
    uint8_t *fat1 = b->image + RESERVED_SECTORS * SECTOR_SIZE;
    uint8_t *root = b->image + (RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT) * SECTOR_SIZE;
    uint8_t *data = b->image + DATA_START_SECTOR * SECTOR_SIZE;
    const uint32_t data_sectors = TOTAL_SECTORS - DATA_START_SECTOR;
    const uint32_t max_cluster = FIRST_DATA_CLUSTER + data_sectors / SECTORS_PER_CLUSTER - 1u;
    const uint32_t clusters = (uint32_t)((size + CLUSTER_SIZE - 1u) / CLUSTER_SIZE);
    const uint16_t first = clusters != 0u ? b->next_cluster : 0u;
    uint8_t *entry;
    uint32_t i;

    if (b->next_root_entry >= ROOT_ENTRIES) return 0;
    if (clusters != 0u && (uint32_t)first + clusters - 1u > max_cluster) return 0;

    entry = root + b->next_root_entry * 32u;
    if (!to_83(dos_name, entry)) return 0;

    for (i = 0u; i < clusters; ++i) {
        const uint16_t cluster = (uint16_t)(first + i);
        const uint16_t next = (i + 1u == clusters) ? 0x0FFFu : (uint16_t)(cluster + 1u);
        fat12_set(fat1, cluster, next);
    }

    entry[11u] = 0x20u; /* archive */
    put16(entry + 22u, FAT_TIME(STAMP_HOUR, STAMP_MINUTE, STAMP_SECOND));
    put16(entry + 24u, FAT_DATE(STAMP_YEAR, STAMP_MONTH, STAMP_DAY));
    put16(entry + 26u, first);
    put32(entry + 28u, (uint32_t)size);

    if (size != 0u) {
        memcpy(data + (size_t)(first - FIRST_DATA_CLUSTER) * CLUSTER_SIZE, bytes, size);
    }

    b->next_cluster = (uint16_t)(b->next_cluster + clusters);
    ++b->next_root_entry;
    if (cluster_count_out != NULL) *cluster_count_out = (uint16_t)clusters;
    if (first_cluster_out != NULL) *first_cluster_out = first;
    return 1;
}

static int selftest(void)
{
    uint8_t command[35];
    uint8_t extra[1500];
    uint8_t *image = (uint8_t *)calloc(1u, IMAGE_SIZE);
    Fat12Builder b;
    uint16_t clusters = 0u;
    uint16_t extra_clusters = 0u;
    uint16_t extra_first = 0u;
    uint16_t first = 0u;
    unsigned i;
    const size_t fat = RESERVED_SECTORS * SECTOR_SIZE;
    const size_t root = (RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT) * SECTOR_SIZE;
    const size_t data = DATA_START_SECTOR * SECTOR_SIZE;
    const uint16_t want_date = FAT_DATE(STAMP_YEAR, STAMP_MONTH, STAMP_DAY);
    uint8_t name83[11];
    int ok;

    if (image == NULL) return 1;
    for (i = 0u; i < sizeof(command); ++i) command[i] = (uint8_t)(i * 7u + 3u);
    for (i = 0u; i < sizeof(extra); ++i) extra[i] = (uint8_t)(i * 13u + 1u);
    build_boot_sector(image);
    builder_init(&b, image);
    ok = install_file(&b, "COMMAND.COM", command, sizeof(command), &clusters, &first) &&
         install_file(&b, "dos2test.com", extra, sizeof(extra), &extra_clusters, &extra_first);
    builder_finish(&b);

    ok = ok && clusters == 1u && first == 2u && extra_clusters == 2u && extra_first == 3u &&
         image[0x0Bu] == 0x00u && image[0x0Cu] == 0x02u && image[0x0Du] == 2u &&
         image[0x15u] == MEDIA_DESCRIPTOR &&
         memcmp(image + root, "COMMAND COM", 11u) == 0 &&
         image[root + 26u] == 2u && image[root + 27u] == 0u &&
         (uint16_t)(image[root + 24u] | (image[root + 25u] << 8)) == want_date &&
         memcmp(image + root + 32u, "DOS2TESTCOM", 11u) == 0 &&
         image[root + 32u + 26u] == 3u &&
         memcmp(image + data, command, sizeof(command)) == 0 &&
         memcmp(image + data + CLUSTER_SIZE, extra, sizeof(extra)) == 0 &&
         /* FAT: 2 -> EOF, 3 -> 4, 4 -> EOF.  Entries 2,3 share bytes 3..5. */
         image[fat + 3u] == 0xFFu && image[fat + 4u] == 0x4Fu && image[fat + 5u] == 0x00u &&
         image[fat + 6u] == 0xFFu && (image[fat + 7u] & 0x0Fu) == 0x0Fu &&
         memcmp(image + fat, image + fat + SECTORS_PER_FAT * SECTOR_SIZE,
                SECTORS_PER_FAT * SECTOR_SIZE) == 0 &&
         !to_83("TOOLONGNAME.COM", name83) && !to_83("A.TOOL", name83);

    free(image);
    if (!ok) {
        fprintf(stderr, "mkfat12 selftest failed\n");
        return 1;
    }
    puts("mkfat12 selftest passed");
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --command COMMAND.COM [--add FILE NAME.EXT]... --output msdos2.img\n", argv0);
}

typedef struct ExtraFile {
    const char *path;
    const char *name;
} ExtraFile;

int main(int argc, char **argv)
{
    const char *command_path = NULL;
    const char *output_path = NULL;
    ExtraFile extras[MAX_FILES];
    unsigned extra_count = 0u;
    uint8_t *command;
    uint8_t *image;
    size_t command_size;
    uint16_t clusters = 0u;
    uint16_t first = 0u;
    Fat12Builder builder;
    unsigned e;
    int i;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--command") == 0 && i + 1 < argc) {
            command_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--add") == 0 && i + 2 < argc && extra_count < MAX_FILES) {
            extras[extra_count].path = argv[++i];
            extras[extra_count].name = argv[++i];
            ++extra_count;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (command_path == NULL || output_path == NULL) {
        usage(argv[0]);
        return 2;
    }

    command = read_file(command_path, &command_size);
    if (command == NULL) {
        fprintf(stderr, "mkfat12: unable to read %s: %s\n", command_path, strerror(errno));
        return 2;
    }
    image = (uint8_t *)calloc(1u, IMAGE_SIZE);
    if (image == NULL) {
        free(command);
        return 2;
    }

    build_boot_sector(image);
    builder_init(&builder, image);
    if (!install_file(&builder, "COMMAND.COM", command, command_size, &clusters, &first)) {
        fprintf(stderr, "mkfat12: COMMAND.COM is too large for the 360 KiB image\n");
        free(image);
        free(command);
        return 2;
    }

    printf("mkfat12: %s -> %s\n", command_path, output_path);
    printf("  image:    %u bytes (%u x %u)\n", (unsigned)IMAGE_SIZE,
           (unsigned)TOTAL_SECTORS, (unsigned)SECTOR_SIZE);
    printf("  COMMAND:  %zu bytes, %u clusters, first cluster %u\n",
           command_size, (unsigned)clusters, (unsigned)first);

    for (e = 0u; e < extra_count; ++e) {
        size_t size = 0u;
        uint8_t *bytes = read_file(extras[e].path, &size);
        if (bytes == NULL) {
            fprintf(stderr, "mkfat12: unable to read %s: %s\n", extras[e].path, strerror(errno));
            free(image);
            free(command);
            return 2;
        }
        if (!install_file(&builder, extras[e].name, bytes, size, &clusters, &first)) {
            fprintf(stderr, "mkfat12: cannot add %s as %s (bad 8.3 name or image full)\n",
                    extras[e].path, extras[e].name);
            free(bytes);
            free(image);
            free(command);
            return 2;
        }
        printf("  %-12s %zu bytes, %u clusters, first cluster %u\n",
               extras[e].name, size, (unsigned)clusters, (unsigned)first);
        free(bytes);
    }
    builder_finish(&builder);

    printf("  stamp:    %04u-%02u-%02u %02u:%02u for all entries\n",
           (unsigned)STAMP_YEAR, (unsigned)STAMP_MONTH, (unsigned)STAMP_DAY,
           (unsigned)STAMP_HOUR, (unsigned)STAMP_MINUTE);
    printf("  FAT/root: sectors 1-4 / 5-11, data starts at sector %u\n",
           (unsigned)DATA_START_SECTOR);

    if (!write_file(output_path, image, IMAGE_SIZE)) {
        fprintf(stderr, "mkfat12: unable to write %s: %s\n", output_path, strerror(errno));
        free(image);
        free(command);
        return 2;
    }

    free(image);
    free(command);
    return 0;
}
