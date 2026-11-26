/* The FatFs options of the Cardputer build (sdkconfig.cardputer), for
 * building the vendored FatFs (platform/esp32s3/components/fatfs) on the
 * host in make test-fat. */
#define CONFIG_GOGO_FS_EXFAT 1
#define CONFIG_FATFS_VOLUME_COUNT 1
#define CONFIG_FATFS_LFN_STACK 1
#define CONFIG_FATFS_SECTOR_512 1
#define CONFIG_FATFS_CODEPAGE_437 1
#define CONFIG_FATFS_CODEPAGE 437
#define CONFIG_FATFS_MAX_LFN 64
#define CONFIG_FATFS_API_ENCODING_ANSI_OEM 1
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 10000
#define CONFIG_FATFS_USE_STRFUNC_NONE 1
#define CONFIG_FATFS_USE_FASTSEEK 0
#define CONFIG_FATFS_PER_FILE_CACHE 0
#define CONFIG_FATFS_USE_DYN_BUFFERS 0
#define CONFIG_WL_SECTOR_SIZE_512 1
#define CONFIG_WL_SECTOR_SIZE 512
