/* gogo_register_port against a VM whose board has drivers: every one of
 * the board's driver names is refused, whatever the grant, besides the
 * runtime's reserved ones, and a name neither has is not. (The library's
 * own board has no drivers.) */
#include "test.h"
#include "embed.h"
#include <string.h>

static const PortDriver d_gpio = { .name = "gpio" };
static const PortDriver d_tcp = { .name = "tcp" };
static const PortDriver d_screen = { .name = "screen" };
static const PortDriver d_kbd = { .name = "kbd" };
static const PortDriver d_extra = { .name = "board_only" };   /* not a runtime name */
static const PortDriver *const board_ports[] = { &d_gpio, &d_tcp, &d_screen, &d_kbd, &d_extra, NULL };
static const VmBoard board = { .ports = board_ports };

static const gogo_port_driver none = GOGO_PORT_DRIVER_INIT;

static void check_board_names(const gogo_config *cfg) {
    gogo_vm *g = NULL;
    CHECK_EQ(gogo_new_posix(cfg, &g), GOGO_OK);
    CHECK(g != NULL);
    const VmBoard *own = g->vm.board;
    g->vm.board = &board;
    for (const PortDriver *const *b = board.ports; *b; b++)
        CHECK_EQ(gogo_register_port(g, (*b)->name, strlen((*b)->name), &none, NULL), GOGO_EEXIST);
    CHECK_EQ(g->ports.n_host_ports, 0);
    CHECK_EQ(gogo_register_port(g, "dns", 3, &none, NULL), GOGO_OK);
    CHECK_EQ(gogo_register_port(g, "dns", 3, &none, NULL), GOGO_EEXIST);
    g->vm.board = own;
    gogo_free(g);
}

TEST(hostport_board_names_refused) {
    check_board_names(NULL);
    gogo_config cfg = GOGO_CONFIG_INIT;
    static const char *const grant[] = { "dns", NULL };
    cfg.grant = grant;              /* board drivers not granted: still refused */
    check_board_names(&cfg);
    /* On the library's own board, a name only that board had is free. */
    gogo_vm *g = NULL;
    CHECK_EQ(gogo_new_posix(NULL, &g), GOGO_OK);
    CHECK_EQ(gogo_register_port(g, "board_only", 10, &none, NULL), GOGO_OK);
    gogo_free(g);
}

/* The names a host port may take: letters, digits, '_', '-' and '.'
 * (never first, last or doubled), 1 to 31 bytes; a dot is allowed (a
 * vendor prefix, as board ports need) but not needed. */
TEST(hostport_name_rules) {
    gogo_vm *g = NULL;
    CHECK_EQ(gogo_new_posix(NULL, &g), GOGO_OK);
    static const char *const bad[] = { "a b", "a/b", "/sd", "a,b", ".x", "x.", "a..b", "tab\t", "\xc3\xbc", "a=b", NULL };
    for (const char *const *b = bad; *b; b++) CHECK_EQ(gogo_register_port(g, *b, strlen(*b), &none, NULL), GOGO_EINVAL);
    CHECK_EQ(gogo_register_port(g, "a\0b", 3, &none, NULL), GOGO_EINVAL);
    CHECK_EQ(gogo_register_port(g, "abcdefghijklmnopqrstuvwxyz012345", 32, &none, NULL), GOGO_E2BIG);
    CHECK_EQ(gogo_register_port(g, "abcdefghijklmnopqrstuvwxyz01234", 31, &none, NULL), GOGO_OK);
    CHECK_EQ(gogo_register_port(g, "acme.dice", 9, &none, NULL), GOGO_OK);
    CHECK_EQ(gogo_register_port(g, "Ui_2-x", 6, &none, NULL), GOGO_OK);
    CHECK_EQ(g->ports.n_host_ports, 3);
    gogo_free(g);
}
