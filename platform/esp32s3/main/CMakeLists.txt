# The ESP platform layer plus the VM core, compiled in place from the repo
# root (no copies).
get_filename_component(GOGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

file(GLOB GOGO_VM_SRCS "${GOGO_ROOT}/vm/*.c")
list(FILTER GOGO_VM_SRCS EXCLUDE REGEX "/check32\\.c$")

# Hook for platform/common (console grid, font; Task 9): add its sources
# here, e.g.
#   file(GLOB GOGO_COMMON_SRCS "${GOGO_ROOT}/platform/common/*.c")
# and add "${GOGO_ROOT}/platform/common" to PRIV_INCLUDE_DIRS.
set(GOGO_COMMON_SRCS "")

idf_component_register(
    SRCS main.c platform_esp.c gpio_esp.c image_part.c ${GOGO_VM_SRCS} ${GOGO_COMMON_SRCS}
    PRIV_INCLUDE_DIRS "." "${GOGO_ROOT}/vm"
    PRIV_REQUIRES esp_partition esp_timer esp_driver_gpio esp_rom freertos)

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
    GOGO_VERSION="${GOGO_VERSION}")
# IDF already passes -Os -Wall -Wextra -Werror but relaxes some of them;
# hold our sources to the host build's -Wall -Wextra -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wall -Wextra -Werror
    -Werror=extra -Wunused-parameter -Wsign-compare -Werror=unused-function
    -Werror=unused-variable -Werror=unused-but-set-variable)
# The VM core is portable C99.
set_source_files_properties(${GOGO_VM_SRCS} PROPERTIES COMPILE_OPTIONS "-std=c99")
