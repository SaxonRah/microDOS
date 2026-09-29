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

static int install_command(uint8_t *image, const uint8_t *command, size_t command_size,
                           uint16_t *cluster_count_out)
{
    uint8_t *fat1 = image + RESERVED_SECTORS * SECTOR_SIZE;
    uint8_t *fat2 = fat1 + SECTORS_PER_FAT * SECTOR_SIZE;
    uint8_t *root = image + (RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT) * SECTOR_SIZE;
    uint8_t *data = image + DATA_START_SECTOR * SECTOR_SIZE;
    const uint32_t data_sectors = TOTAL_SECTORS - DATA_START_SECTOR;
    const uint32_t max_clusters = data_sectors / SECTORS_PER_CLUSTER;
    const uint32_t clusters = (uint32_t)((command_size + CLUSTER_SIZE - 1u) / CLUSTER_SIZE);
    uint32_t i;

    if (clusters == 0u || clusters > max_clusters || clusters > 0x0FFDu) return 0;

    fat1[0] = MEDIA_DESCRIPTOR;
    fat1[1] = 0xFFu;
    fat1[2] = 0xFFu;

    for (i = 0u; i < clusters; ++i) {
        const uint16_t cluster = (uint16_t)(FIRST_DATA_CLUSTER + i);
        const uint16_t next = (i + 1u == clusters) ? 0x0FFFu : (uint16_t)(cluster + 1u);
        fat12_set(fat1, cluster, next);
    }
    memcpy(fat2, fat1, SECTORS_PER_FAT * SECTOR_SIZE);

    memcpy(root + 0u, "COMMAND COM", 11u);
    root[11u] = 0x20u; /* archive */
    put16(root + 26u, FIRST_DATA_CLUSTER);
    put32(root + 28u, (uint32_t)command_size);

    memcpy(data, command, command_size);
    *cluster_count_out = (uint16_t)clusters;
    return 1;
}


static int selftest(void)
{
    uint8_t command[35];
    uint8_t *image = (uint8_t *)calloc(1u, IMAGE_SIZE);
    uint16_t clusters = 0u;
    unsigned i;
    const size_t root = (RESERVED_SECTORS + FAT_COUNT * SECTORS_PER_FAT) * SECTOR_SIZE;
    const size_t data = DATA_START_SECTOR * SECTOR_SIZE;

    if (image == NULL) return 1;
    for (i = 0u; i < sizeof(command); ++i) command[i] = (uint8_t)(i * 7u + 3u);
    build_boot_sector(image);
    if (!install_command(image, command, sizeof(command), &clusters) || clusters != 1u ||
        image[0x0Bu] != 0x00u || image[0x0Cu] != 0x02u || image[0x0Du] != 2u ||
        image[0x15u] != MEDIA_DESCRIPTOR ||
        memcmp(image + root, "COMMAND COM", 11u) != 0 ||
        image[root + 26u] != 2u || image[root + 27u] != 0u ||
        memcmp(image + data, command, sizeof(command)) != 0) {
        free(image);
        fprintf(stderr, "mkfat12 selftest failed\n");
        return 1;
    }
    free(image);
    puts("mkfat12 selftest passed");
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --command COMMAND.COM --output msdos2.img\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) return selftest();
    const char *command_path = NULL;
    const char *output_path = NULL;
    uint8_t *command;
    uint8_t *image;
    size_t command_size;
    uint16_t clusters = 0u;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--command") == 0 && i + 1 < argc) {
            command_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
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
    if (!install_command(image, command, command_size, &clusters)) {
        fprintf(stderr, "mkfat12: COMMAND.COM is too large for the 360 KiB image\n");
        free(image);
        free(command);
        return 2;
    }
    if (!write_file(output_path, image, IMAGE_SIZE)) {
        fprintf(stderr, "mkfat12: unable to write %s: %s\n", output_path, strerror(errno));
        free(image);
        free(command);
        return 2;
    }

    printf("mkfat12: %s -> %s\n", command_path, output_path);
    printf("  image:    %u bytes (%u x %u)\n", (unsigned)IMAGE_SIZE,
           (unsigned)TOTAL_SECTORS, (unsigned)SECTOR_SIZE);
    printf("  COMMAND:  %zu bytes, %u clusters, first cluster %u\n",
           command_size, (unsigned)clusters, (unsigned)FIRST_DATA_CLUSTER);
    printf("  FAT/root: sectors 1-4 / 5-11, data starts at sector %u\n",
           (unsigned)DATA_START_SECTOR);

    free(image);
    free(command);
    return 0;
}
