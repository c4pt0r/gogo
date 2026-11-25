# fatfs (vendored ESP-IDF component, exFAT on)

A copy of ESP-IDF **v6.1.1** `components/fatfs` that overrides the IDF
component of the same name (a project `components/` directory takes
precedence). Copied: `CMakeLists.txt`, `Kconfig`, `sbom.yml`, `diskio/`,
`port/`, `src/`, `vfs/` (tests, host tests and the image-generator Python
tools are left out; nothing here uses `fatfs_create_partition_image`).

Why: `/sd` (`main/fs_fat.c`, Cardputer ADV) must mount exFAT cards, and
IDF's `src/ffconf.h` hard-codes `FF_FS_EXFAT 0` with no Kconfig option.
Every other FatFs option still comes from Kconfig (`sdkconfig.cardputer`).

The changes from IDF v6.1.1, both in `src/ffconf.h`:

    -#define FF_FS_EXFAT		0
    +#define FF_FS_EXFAT		1

and, because exFAT code uses `FF_USE_LABEL` in a C expression (not only in
`#if`) and `CONFIG_FATFS_USE_LABEL` is undefined when the Kconfig option is
off (the IDF build fails with exFAT on otherwise):

    -#define FF_USE_LABEL	CONFIG_FATFS_USE_LABEL
    +#if defined(CONFIG_FATFS_USE_LABEL)
    +#define FF_USE_LABEL	CONFIG_FATFS_USE_LABEL
    +#else
    +#define FF_USE_LABEL	0
    +#endif

On an IDF upgrade: copy the same files from the new IDF, reapply these two
changes, and check `diff -r ~/esp/esp-idf/components/fatfs/src src` shows only
them.
The StickS3 build compiles this component too but links none of it
(`gogo.bin` unchanged, no `f_*` symbols in its elf).
