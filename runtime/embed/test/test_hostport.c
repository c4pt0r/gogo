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
static const VmBoard board = { board_ports, NULL, NULL, NULL, NULL };

static const gogo_port_driver none = { NULL, NULL, NULL };

static void check_board_names(const gogo_config *cfg) {
    gogo_vm *g = gogo_new_posix(cfg);
    CHECK(g != NULL);
    const VmBoard *own = g->vm.board;
    g->vm.board = &board;
    for (const PortDriver *const *b = board.ports; *b; b++)
        CHECK_EQ(gogo_register_port(g, (*b)->name, &none, NULL), GOGO_EEXIST);
    CHECK_EQ(g->n_host_ports, 0);
    CHECK_EQ(gogo_register_port(g, "dns", &none, NULL), GOGO_OK);
    CHECK_EQ(gogo_register_port(g, "dns", &none, NULL), GOGO_EEXIST);
    g->vm.board = own;
    gogo_free(g);
}

TEST(hostport_board_names_refused) {
    check_board_names(NULL);
    gogo_config cfg;
    memset(&cfg, 0, sizeof cfg);
    static const char *const grant[] = { "dns", NULL };
    cfg.grant = grant;              /* board drivers not granted: still refused */
    check_board_names(&cfg);
    /* On the library's own board, a name only that board had is free. */
    gogo_vm *g = gogo_new_posix(NULL);
    CHECK_EQ(gogo_register_port(g, "board_only", &none, NULL), GOGO_OK);
    gogo_free(g);
}
