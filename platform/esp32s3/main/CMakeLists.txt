# The ESP platform layer plus the VM core, compiled in place from the repo
# root (no copies).
get_filename_component(GOGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

file(GLOB GOGO_VM_SRCS "${GOGO_ROOT}/runtime/vm/*.c")
list(FILTER GOGO_VM_SRCS EXCLUDE REGEX "/check32\\.c$")
# The net stack (pure C99, chip-independent; spec M5): its net_rx_offer /
# net_link_event are what wifi.c feeds.
# Without GOGO_NET (main/Kconfig.projbuild) neither it nor wifi.c is built.
set(GOGO_NET_SRCS "")
set(GOGO_WIFI_SRCS "")
if(CONFIG_GOGO_NET)
    file(GLOB GOGO_NET_SRCS "${GOGO_ROOT}/runtime/net/*.c")
    set(GOGO_WIFI_SRCS wifi.c)
endif()
# The "tls" port (GOGO_TLS, spec HTTPS): runtime/net/tls/tls.c, compiled against
# ESP-IDF's own mbedTLS component (4.1.1, the same source the host build
# uses; configured in sdkconfig.defaults), not the repo's runtime/net/tls/config build:
# the hardware AES / SHA / MPI and the certificate bundle come with it, and
# Wi-Fi's WPA crypto links it anyway. Its hooks are in platform_esp.c. Off,
# tls.c is not built and nothing references mbedTLS's TLS layer.
set(GOGO_TLS_SRCS "")
if(CONFIG_GOGO_TLS)
    set(GOGO_TLS_SRCS "${GOGO_ROOT}/runtime/net/tls/tls.c" "${GOGO_ROOT}/runtime/net/tls/anchor.c")
endif()
# Distributed actors (GOGO_DIST): the "node" port (pure C99).
set(GOGO_DIST_SRCS "")
if(CONFIG_GOGO_DIST)
    file(GLOB GOGO_DIST_SRCS "${GOGO_ROOT}/runtime/dist/*.c")
endif()
# Storage (spec storage §5): the vfs core and the B_FS builtin (pure C99),
# with LittleFS (runtime/vfs/lfs) on the partition fs_lfs.c describes as
# /flash.
file(GLOB GOGO_VFS_SRCS "${GOGO_ROOT}/runtime/vfs/*.c")
# The LittleFS and FatFs backends (runtime/vfs/lfs, runtime/vfs/fat: C99,
# but compiled with IDF's flags, since lfs.h and ff.h pull in IDF headers
# that are not C99): LittleFS over a BlockDev (fs_lfs.c is the partition's).
set(GOGO_FSBE_SRCS "${GOGO_ROOT}/runtime/vfs/lfs/lfs_be.c")
# The `gpio` port driver (pure C99); gpio_esp.c is its GpioHal.
set(GOGO_GPIO_SRCS "${GOGO_ROOT}/runtime/gpio/gpio.c")
# Input (spec input): the kbd and power drivers and event source (pure C99);
# input_esp.c / i2c_bus.c are the hardware.
file(GLOB GOGO_INPUT_SRCS "${GOGO_ROOT}/runtime/input/*.c")
# The gogo-config protocol (pure C99); config_esp.c is the NVS store and
# the board's requests.
set(GOGO_SERVICE_SRCS "${GOGO_ROOT}/runtime/configsvc/configsvc.c")
# The one boot path (pure C99): main.c hands it the image.
list(APPEND GOGO_SERVICE_SRCS "${GOGO_ROOT}/runtime/board/board_main.c" "${GOGO_ROOT}/runtime/board/params.c")
# Streams (spec input2 §1): Chunk / Out / Done for the streaming drivers
# (pure C99).
file(GLOB GOGO_STREAM_SRCS "${GOGO_ROOT}/runtime/stream/*.c")

# Board ports (GOGO_BOARD_PORTS): the host-port layer (runtime/embed's
# hostport.c, inbox.c, poster.c) and runtime/board/board_ports.c, which
# registers at boot the GOGO_BOARD_PORT drivers of other components (gogo.h;
# `gogo firmware -component` turns the switch on). Up to 8 of them; the
# post inbox takes a fifth event source.
set(GOGO_BOARD_PORTS_SRCS "")
set(GOGO_BOARD_PORTS_DEFS "")
if(CONFIG_GOGO_BOARD_PORTS)
    set(GOGO_BOARD_PORTS_SRCS "${GOGO_ROOT}/runtime/board/board_ports.c" "${GOGO_ROOT}/runtime/embed/hostport.c"
        "${GOGO_ROOT}/runtime/embed/inbox.c" "${GOGO_ROOT}/runtime/embed/poster.c" "${GOGO_ROOT}/runtime/embed/version.c")
    set(GOGO_BOARD_PORTS_DEFS GOGO_BOARD_PORTS=1 EMBED_HOST_PORTS=8 EMBED_BOARD_PORTS_MAX=32)
endif()

# Event sources (runtime/vm/config.h GOGO_MAX_SOURCES, 4 by default): the
# net stack's and the input drivers' always, the screen's and gpio's
# debounce while used, the ble port's while open (GOGO_BLE), the board
# ports' post inbox (GOGO_BOARD_PORTS).
set(GOGO_SOURCES 4)
if(CONFIG_GOGO_BLE)
    math(EXPR GOGO_SOURCES "${GOGO_SOURCES} + 1")
endif()
if(CONFIG_GOGO_BOARD_PORTS)
    math(EXPR GOGO_SOURCES "${GOGO_SOURCES} + 1")
endif()

# runtime/screen (GOGO_SCREEN): the "screen" port's grid, driver and LCD
# rendering (pure C99); console_task.c draws it.
set(GOGO_SCREEN_SRCS "")
if(CONFIG_GOGO_SCREEN)
    file(GLOB GOGO_SCREEN_SRCS "${GOGO_ROOT}/runtime/screen/*.c")
endif()

# runtime/floatlib (GOGO_FLOAT): gogo/math and strconv's float functions
# (pure C99, no libm); the VM's own float code is in runtime/vm/floats.c,
# built with GOGO_FLOAT=1 (below) or compiled out with 0.
set(GOGO_FLOATLIB_SRCS "")
set(GOGO_FLOAT_DEF GOGO_FLOAT=0)
if(CONFIG_GOGO_FLOAT)
    file(GLOB GOGO_FLOATLIB_SRCS "${GOGO_ROOT}/runtime/floatlib/*.c")
    set(GOGO_FLOAT_DEF GOGO_FLOAT=1)
endif()

# runtime/console: the console grid and the font (shared with the host
# tests), plus the font blob embedded as _binary_gogo12_font_{start,end}.
set(GOGO_COMMON_SRCS "${GOGO_ROOT}/runtime/console/console.c" "${GOGO_ROOT}/runtime/console/font.c")

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
#       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble;sdkconfig.cardputer" build
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
        # FatFs as a vfs backend over a DiskHal (fs_fat.c is the card's).
        list(APPEND GOGO_FSBE_SRCS "${GOGO_ROOT}/runtime/vfs/fat/fat_be.c")
    endif()
    set(GOGO_BOARD_DEF GOGO_BOARD_CARDPUTER)
else()
    message(FATAL_ERROR "GOGO_BOARD must be sticks3 or cardputer (got '${GOGO_BOARD}')")
endif()

# The shared I2C bus (i2c_bus.c) is for the StickS3's PM1, the Cardputer's
# keyboard and both boards' BMI270 and ES8311; without GOGO_KBD, GOGO_IMU and GOGO_AUDIO
# (main/Kconfig.projbuild) the Cardputer needs none, and the IDF I2C driver is
# not linked.
set(GOGO_I2C_SRCS "")
if(GOGO_BOARD STREQUAL "sticks3" OR CONFIG_GOGO_KBD OR CONFIG_GOGO_IMU OR CONFIG_GOGO_AUDIO)
    set(GOGO_I2C_SRCS i2c_bus.c)
endif()

# The BMI270 (GOGO_IMU: imu_esp.c and the 8 KB legacy configuration, Bosch
# BSD-3) and IR on the RMT (GOGO_IR: ir_esp.c). Off, the file is not built,
# its hooks stay NULL (port.Open fails, the driver is absent) and, for
# IR, the IDF RMT driver is not linked.
set(GOGO_IMU_SRCS "")
if(CONFIG_GOGO_IMU)
    set(GOGO_IMU_SRCS imu_esp.c bmi270_legacy_config.c)
endif()
set(GOGO_IR_SRCS "")
if(CONFIG_GOGO_IR)
    set(GOGO_IR_SRCS ir_esp.c)
endif()
# Audio (GOGO_AUDIO: audio_esp.c, the ES8311 codec on I2S). Off, the hooks
# stay NULL and the IDF I2S driver is not linked.
set(GOGO_AUDIO_SRCS "")
if(CONFIG_GOGO_AUDIO)
    set(GOGO_AUDIO_SRCS audio_esp.c)
endif()

# BLE (GOGO_BLE: the "ble" port, runtime/ble/ble.c over NimBLE in
# ble_esp.c). ble_host.c, the NimBLE host's shared lifetime (and the weak
# ble_prov_active), is built whenever NimBLE is (sdkconfig.ble), for the
# port and for Wi-Fi provisioning alike. Off, nothing references the bt
# component and it is not linked.
set(GOGO_BLE_SRCS "")
if(CONFIG_BT_NIMBLE_ENABLED)
    list(APPEND GOGO_BLE_SRCS ble_host.c)
endif()
set(GOGO_BLE_CORE_SRCS "")
if(CONFIG_GOGO_BLE)
    list(APPEND GOGO_BLE_SRCS ble_esp.c)
    set(GOGO_BLE_CORE_SRCS "${GOGO_ROOT}/runtime/ble/ble.c")
endif()

# Wi-Fi provisioning over BLE (GOGO_BLE_PROV: needs GOGO_BLE and GOGO_NET):
# ble_prov.c and the provisioning messages (runtime/prov, pure C99), on the
# shared NimBLE host above. Off, neither is built and protocomm is not
# linked.
set(GOGO_PROV_SRCS "")
set(GOGO_PROVMSG_SRCS "")
if(CONFIG_GOGO_BLE_PROV)
    set(GOGO_PROV_SRCS ble_prov.c)
    set(GOGO_PROVMSG_SRCS "${GOGO_ROOT}/runtime/prov/provmsg.c")
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
         ${GOGO_IMU_SRCS} ${GOGO_IR_SRCS} ${GOGO_AUDIO_SRCS} ${GOGO_BLE_SRCS} ${GOGO_BLE_CORE_SRCS} ${GOGO_PROV_SRCS} ${GOGO_PROVMSG_SRCS}
         ${GOGO_VM_SRCS} ${GOGO_FLOATLIB_SRCS} ${GOGO_NET_SRCS} ${GOGO_TLS_SRCS} ${GOGO_DIST_SRCS} ${GOGO_GPIO_SRCS} ${GOGO_SERVICE_SRCS} ${GOGO_VFS_SRCS} ${GOGO_FSBE_SRCS} ${GOGO_INPUT_SRCS} ${GOGO_STREAM_SRCS} ${GOGO_SCREEN_SRCS} ${GOGO_COMMON_SRCS} ${GOGO_BOARD_PORTS_SRCS}
    PRIV_INCLUDE_DIRS "." "${GOGO_ROOT}/runtime/include" "${GOGO_ROOT}/runtime/embed" "${GOGO_ROOT}/runtime/vm" "${GOGO_ROOT}/runtime/net" "${GOGO_ROOT}/runtime/net/tls" "${GOGO_ROOT}/runtime/dist" "${GOGO_ROOT}/runtime/vfs" "${GOGO_ROOT}/runtime/vfs/lfs" "${GOGO_ROOT}/runtime/vfs/fat" "${GOGO_ROOT}/runtime/gpio" "${GOGO_ROOT}/runtime/configsvc" "${GOGO_ROOT}/runtime/board" "${GOGO_ROOT}/runtime/input" "${GOGO_ROOT}/runtime/stream" "${GOGO_ROOT}/runtime/ble" "${GOGO_ROOT}/runtime/screen" "${GOGO_ROOT}/runtime/console" "${GOGO_ROOT}/runtime/prov"
    EMBED_FILES "${GOGO_ROOT}/runtime/console/font/${GOGO_FONT_FILE}"
    LDFRAGMENTS linker.lf
    PRIV_REQUIRES esp_partition esp_timer esp_driver_gpio esp_driver_spi esp_driver_i2c esp_adc
                  esp_rom freertos heap nvs_flash esp_driver_usb_serial_jtag
                  # ir_esp.c (GOGO_IR); like esp_wifi below, unreferenced
                  # (so not linked) when the switch is off.
                  esp_driver_rmt
                  # audio_esp.c (GOGO_AUDIO), likewise.
                  esp_driver_i2s
                  # ble_host.c / ble_esp.c (NimBLE, GOGO_BLE), likewise.
                  bt
                  joltwallet__littlefs
                  # wifi.c (GOGO_NET): listed unconditionally because IDF
                  # expands requirements before CONFIG_GOGO_NET is known;
                  # without GOGO_NET nothing references them, so nothing links.
                  esp_wifi esp_event
                  # tls.c (GOGO_TLS) and its hooks in platform_esp.c, likewise.
                  mbedtls
                  # ble_prov.c (GOGO_BLE_PROV), likewise.
                  protocomm
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

# Firmware version for the boot line and `gogo device info`: the app's
# PROJECT_VER, `git describe` of the repo (../CMakeLists.txt).
idf_build_get_property(GOGO_FIRMWARE_VERSION PROJECT_VER)
if(NOT GOGO_FIRMWARE_VERSION)
    set(GOGO_FIRMWARE_VERSION "dev")
endif()

# The wall-clock floor (runtime/vm/host.h): the build time, Unix ms.
string(TIMESTAMP GOGO_BUILD_S "%s" UTC)

# ESP values of the runtime/vm/config.h knobs.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GOGO_BUILD_UNIX_MS=${GOGO_BUILD_S}000LL
    # tls.c: IDF's mbedTLS is shared (Wi-Fi's WPA crypto), so the port
    # installs no allocator or time hook and defines no RNG; platform_esp.c
    # and sdkconfig.defaults set them up (runtime/net/tls/tls.h).
    GOGO_TLS_MBEDTLS_HOOKS=0
    GOGO_DEFAULT_MAX_WORDS=4096u
    GOGO_TABLE_CAP=1024u
    GOGO_MAX_PORTS=16u
    GOGO_EVQ_CAP=64u
    GOGO_ARENA_BYTES=163840u
    # vfs: 4 open files; a slot holds lfs_be.c's Lf (lfs_file_t 84 +
    # lfs_file_config 12 + 64-byte cache = 160, checked there) or, on the
    # Cardputer, a FatFs FIL (smaller, checked in fat_be.c).
    GOGO_FS_MAX_OPEN=4u
    GOGO_FS_SLOT_BYTES=160u
    ${GOGO_FLOAT_DEF}
    ${GOGO_BOARD_PORTS_DEFS}
    GOGO_MAX_SOURCES=${GOGO_SOURCES}u
    # IDF's esp_stdio exports a global console_write (stdio_vfs.c); rename
    # ours at link level for this component only.
    console_write=gogo_console_write
    GOGO_ESP_CONSOLE=${GOGO_CONSOLE}
    GOGO_ESP_CONSOLE_VERBOSE=${GOGO_CONSOLE_VERBOSE}
    # (not GOGO_VERSION: that is gogo.h's numeric API version)
    GOGO_FIRMWARE_VERSION="${GOGO_FIRMWARE_VERSION}"
    ${GOGO_BOARD_DEF})
# IDF already passes -Os -Wall -Wextra -Werror but relaxes some of them;
# hold our sources to the host build's -Wall -Wextra -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wall -Wextra -Werror
    -Werror=extra -Wunused-parameter -Wsign-compare -Werror=unused-function
    -Werror=unused-variable -Werror=unused-but-set-variable)
# The VM core is portable C99.
set_source_files_properties(${GOGO_VM_SRCS} ${GOGO_FLOATLIB_SRCS} ${GOGO_NET_SRCS} ${GOGO_TLS_SRCS} ${GOGO_DIST_SRCS} ${GOGO_GPIO_SRCS} ${GOGO_SERVICE_SRCS} ${GOGO_VFS_SRCS} ${GOGO_INPUT_SRCS} ${GOGO_STREAM_SRCS} ${GOGO_SCREEN_SRCS} ${GOGO_COMMON_SRCS} ${GOGO_BOARD_PORTS_SRCS} ${GOGO_BLE_CORE_SRCS} ${GOGO_PROVMSG_SRCS} PROPERTIES COMPILE_OPTIONS "-std=c99")
