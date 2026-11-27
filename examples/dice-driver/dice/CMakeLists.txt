# The dice driver as an ESP-IDF component, built into a gogo firmware by
#   gogo firmware -component examples/dice-driver/dice -board sticks3 -build IMAGE.ggb
# (or idf.py with EXTRA_COMPONENT_DIRS and CONFIG_GOGO_BOARD_PORTS=y). It
# needs only gogo.h (the gogo_api component). WHOLE_ARCHIVE: nothing calls
# into dice_port.c, whose GOGO_BOARD_PORT entry the linker would drop.
idf_component_register(SRCS dice.c dice_port.c
                       INCLUDE_DIRS "."
                       REQUIRES gogo_api
                       WHOLE_ARCHIVE)
