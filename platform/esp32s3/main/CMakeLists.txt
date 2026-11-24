# The ESP platform layer plus the VM core, compiled in place from the repo
# root (no copies).
get_filename_component(GOGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

file(GLOB GOGO_VM_SRCS "${GOGO_ROOT}/vm/*.c")
list(FILTER GOGO_VM_SRCS EXCLUDE REGEX "/check32\\.c$")

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

idf_component_register(
    SRCS main.c platform_esp.c gpio_esp.c image_part.c console_task.c pm1.c lcd_st7789.c
         ${GOGO_VM_SRCS} ${GOGO_COMMON_SRCS}
    PRIV_INCLUDE_DIRS "." "${GOGO_ROOT}/vm" "${GOGO_ROOT}/platform/common"
    EMBED_FILES "${GOGO_ROOT}/platform/common/font/gogo12.font"
    LDFRAGMENTS linker.lf
    PRIV_REQUIRES esp_partition esp_timer esp_driver_gpio esp_driver_spi esp_driver_i2c
                  esp_rom freertos heap)

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
    # IDF's esp_stdio exports a global console_write (stdio_vfs.c); rename
    # ours at link level for this component only.
    console_write=gogo_console_write
    GOGO_ESP_CONSOLE=${GOGO_CONSOLE}
    GOGO_ESP_CONSOLE_VERBOSE=${GOGO_CONSOLE_VERBOSE}
    GOGO_VERSION="${GOGO_VERSION}")
# IDF already passes -Os -Wall -Wextra -Werror but relaxes some of them;
# hold our sources to the host build's -Wall -Wextra -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wall -Wextra -Werror
    -Werror=extra -Wunused-parameter -Wsign-compare -Werror=unused-function
    -Werror=unused-variable -Werror=unused-but-set-variable)
# The VM core is portable C99.
set_source_files_properties(${GOGO_VM_SRCS} ${GOGO_COMMON_SRCS} PROPERTIES COMPILE_OPTIONS "-std=c99")
