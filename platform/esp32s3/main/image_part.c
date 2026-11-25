/* The program image lives in the `gogo` data partition (subtype 0x40, see
 * partitions.csv) behind a 16-byte little-endian header written by
 * `gogo flash`:
 *
 *   {magic 0x4D494747 "GGIM", image_bytes, crc32_ieee(image), 0}
 *
 * followed by the image (format v13, runtime/vm/image.h IMAGE_VERSION). The image is used in place through a
 * flash mmap; a blank, half-written or corrupted partition is refused. An
 * image of another version is refused by image_load: when IMAGE_VERSION
 * changes, the firmware must be reflashed along with the image. */
#include "platform_esp.h"

#include <string.h>

#include "esp_partition.h"
#include "esp_rom_crc.h"

#define PART_SUBTYPE 0x40
#define PART_MAGIC   0x4D494747u
#define HDR_BYTES    16u

int image_part_map(const uint32_t **words, uint32_t *nwords) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)PART_SUBTYPE, "gogo");
    if (!part) {
        platform_esp_say("gogo: no image (no gogo partition)");
        return -1;
    }
    uint32_t hdr[4];
    if (esp_partition_read(part, 0, hdr, sizeof hdr) != ESP_OK) {
        platform_esp_say("gogo: bad image (read failed)");
        return -1;
    }
    if (hdr[0] != PART_MAGIC) {      /* blank (0xFF) or not written by gogo flash */
        platform_esp_say("gogo: no image");
        return -1;
    }
    uint32_t n = hdr[1];
    if (n == 0 || n % 4u != 0 || n > part->size - HDR_BYTES) {
        platform_esp_say("gogo: bad image (length %lu)", (unsigned long)n);
        return -1;
    }
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, HDR_BYTES + n, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        platform_esp_say("gogo: bad image (mmap failed)");
        return -1;
    }
    const uint8_t *img = (const uint8_t *)map + HDR_BYTES;
    if (esp_rom_crc32_le(0, img, n) != hdr[2]) {   /* = crc32 IEEE (hash/crc32) */
        esp_partition_munmap(handle);
        platform_esp_say("gogo: bad image (crc)");
        return -1;
    }
    /* The mapping stays for the life of the program. */
    *words = (const uint32_t *)(const void *)img;
    *nwords = n / 4u;
    return 0;
}
