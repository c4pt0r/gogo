# The ESP platform layer plus the VM core, compiled in place from the repo
# root (no copies).
get_filename_component(GOGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

file(GLOB GOGO_VM_SRCS "${GOGO_ROOT}/vm/*.c")
list(FILTER GOGO_VM_SRCS EXCLUDE REGEX "/check32\\.c$")
# The net stack (pure C99, chip-independent; spec M5): its net_rx_offer /
# net_link_event are what wifi.c feeds.
# Without GOGO_NET (main/Kconfig.projbuild) neither it nor wifi.c is built.
set(GOGO_NET_SRCS "")
set(GOGO_WIFI_SRCS "")
if(CONFIG_GOGO_NET)
    file(GLOB GOGO_NET_SRCS "${GOGO_ROOT}/net/*.c")
    set(GOGO_WIFI_SRCS wifi.c)
endif()
# Storage (spec storage §5): the vfs core and the B_FS builtin (pure C99),
# with fs_lfs.c (/flash on LittleFS) as the backend.
file(GLOB GOGO_VFS_SRCS "${GOGO_ROOT}/vfs/*.c")
# Input (spec input): the kbd and power drivers and event source (pure C99);
# input_esp.c / i2c_bus.c are the hardware.
file(GLOB GOGO_INPUT_SRCS "${GOGO_ROOT}/input/*.c")

# platform/common: the console grid and the font (shared with the host
# tests), plus the font blob embedded as _binary_gogo12_font_{start,end}.
set(GOGO_COMMON_SRCS "${GOGO_ROOT}/platform/common/console.c" "${GOGO_ROOT}/platform/common/font.c")

# On-screen console switch (timing baseline: idf.py -B build-noconsole
# -DGOGO_CONSOLE=0 build) and serial bring-up/frame logging.
if(NOT DEFINED GOGO_CONSOLE)
    set(GOGO_CONSOLE 1)
endif()
if(NOT DEFINED GOGO_CONSOLE_VERBOSE)
    set(GOGO_CONSOLE_VERBOSE 0)
endif()

# Board (main/board.h): GOGO_BOARD=sticks3 (default) or cardputer, given
# to idf.py as -DGOGO_BOARD=...; each board has its own build directory:
#   idf.py -C platform/esp32s3 build                         (StickS3, build/)
#   idf.py -C platform/esp32s3 -B platform/esp32s3/build-cardputer \
#       -DGOGO_BOARD=cardputer -DSDKCONFIG=build-cardputer/sdkconfig \
#       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cardputer" build
# The StickS3 has the M5PM1 (pm1.c) and no SD; the Cardputer ADV has no
# PM1 and /sd on FatFs (fs_fat.c, FatFs options in sdkconfig.cardputer).
if(NOT DEFINED GOGO_BOARD)
    set(GOGO_BOARD sticks3)
endif()
if(GOGO_BOARD STREQUAL "sticks3")
    set(GOGO_BOARD_SRCS pm1.c)
    set(GOGO_BOARD_DEF GOGO_BOARD_STICKS3)
elseif(GOGO_BOARD STREQUAL "cardputer")
    # fs_fat.c is /sd (GOGO_SD, main/Kconfig.projbuild).
    set(GOGO_BOARD_SRCS "")
    if(CONFIG_GOGO_SD)
        set(GOGO_BOARD_SRCS fs_fat.c)
    endif()
    set(GOGO_BOARD_DEF GOGO_BOARD_CARDPUTER)
else()
    message(FATAL_ERROR "GOGO_BOARD must be sticks3 or cardputer (got '${GOGO_BOARD}')")
endif()

# The shared I2C bus (i2c_bus.c) is for the StickS3's PM1 and the Cardputer's
# keyboard; without GOGO_KBD (main/Kconfig.projbuild) the Cardputer needs none,
# and the IDF I2C driver is not linked.
set(GOGO_I2C_SRCS "")
if(GOGO_BOARD STREQUAL "sticks3" OR CONFIG_GOGO_KBD)
    set(GOGO_I2C_SRCS i2c_bus.c)
endif()

# The console font blob (GOGO_FONT_CJK): the embedded symbol is named after
# the file, console_task.c picks it by the same switch.
if(CONFIG_GOGO_FONT_CJK)
    set(GOGO_FONT_FILE gogo12.font)
else()
    set(GOGO_FONT_FILE gogo12-ascii.font)
endif()

idf_component_register(
    SRCS main.c platform_esp.c gpio_esp.c image_part.c console_task.c lcd_st7789.c
         ${GOGO_WIFI_SRCS} config_esp.c fs_lfs.c input_esp.c ${GOGO_I2C_SRCS} ${GOGO_BOARD_SRCS}
         ${GOGO_VM_SRCS} ${GOGO_NET_SRCS} ${GOGO_VFS_SRCS} ${GOGO_INPUT_SRCS} ${GOGO_COMMON_SRCS}
    PRIV_INCLUDE_DIRS "." "${GOGO_ROOT}/vm" "${GOGO_ROOT}/net" "${GOGO_ROOT}/vfs" "${GOGO_ROOT}/input" "${GOGO_ROOT}/platform/common"
    EMBED_FILES "${GOGO_ROOT}/platform/common/font/${GOGO_FONT_FILE}"
    LDFRAGMENTS linker.lf
    PRIV_REQUIRES esp_partition esp_timer esp_driver_gpio esp_driver_spi esp_driver_i2c esp_adc
                  esp_rom freertos heap nvs_flash esp_driver_usb_serial_jtag
                  joltwallet__littlefs
                  # wifi.c (GOGO_NET): listed unconditionally because IDF
                  # expands requirements before CONFIG_GOGO_NET is known;
                  # without GOGO_NET nothing references them, so nothing links.
                  esp_wifi esp_event
                  # fs_fat.c (Cardputer); listed for both boards because IDF
                  # expands requirements before GOGO_BOARD is known. The
                  # StickS3 links none of it (no references).
                  fatfs sdmmc esp_driver_sdspi)

if(GOGO_BOARD STREQUAL "cardputer" AND CONFIG_GOGO_SD)
    # FatFs is built reentrant (IDF's ffconf.h hard-codes FF_FS_REENTRANT 1)
    # with heap-allocated mutexes; only the VM task calls it, so its
    # ff_mutex_* are replaced by fs_fat.c's no-op __wrap_ versions. get_fattime
    # is wrapped to a fixed time: IDF's calls localtime_r (tzset, vfscanf: 7.6 KB).
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=ff_mutex_create" "-Wl,--wrap=ff_mutex_delete"
        "-Wl,--wrap=ff_mutex_take" "-Wl,--wrap=ff_mutex_give"
        "-Wl,--wrap=get_fattime")
endif()

# Firmware version for the boot line: `git describe` of the repo at
# configure time.
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${GOGO_ROOT}"
    OUTPUT_VARIABLE GOGO_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GOGO_VERSION)
    set(GOGO_VERSION "dev")
endif()

# ESP values of the vm/config.h knobs (plan: Global Constraints).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GOGO_DEFAULT_MAX_WORDS=4096u
    GOGO_TABLE_CAP=1024u
    GOGO_MAX_PORTS=16u
    GOGO_EVQ_CAP=64u
    GOGO_ARENA_BYTES=163840u
    # vfs: 4 open files; a slot holds fs_lfs.c's Lf (lfs_file_t 84 +
    # lfs_file_config 12 + 64-byte cache = 160, checked there) or, on the
    # Cardputer, a FatFs FIL (smaller, checked in fs_fat.c).
    GOGO_FS_MAX_OPEN=4u
    GOGO_FS_SLOT_BYTES=160u
    # IDF's esp_stdio exports a global console_write (stdio_vfs.c); rename
    # ours at link level for this component only.
    console_write=gogo_console_write
    GOGO_ESP_CONSOLE=${GOGO_CONSOLE}
    GOGO_ESP_CONSOLE_VERBOSE=${GOGO_CONSOLE_VERBOSE}
    GOGO_VERSION="${GOGO_VERSION}"
    ${GOGO_BOARD_DEF})
# IDF already passes -Os -Wall -Wextra -Werror but relaxes some of them;
# hold our sources to the host build's -Wall -Wextra -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wall -Wextra -Werror
    -Werror=extra -Wunused-parameter -Wsign-compare -Werror=unused-function
    -Werror=unused-variable -Werror=unused-but-set-variable)
# The VM core is portable C99.
set_source_files_properties(${GOGO_VM_SRCS} ${GOGO_NET_SRCS} ${GOGO_VFS_SRCS} ${GOGO_INPUT_SRCS} ${GOGO_COMMON_SRCS} PROPERTIES COMPILE_OPTIONS "-std=c99")
