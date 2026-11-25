/* Firmware capability check (platform.h CAP_*): an image's requires word
 * against what the firmware provides. Pure: no I/O; callers decide what to
 * do with a refusal. */
#include "platform.h"

/* Default for a platform that defines none: basic I/O only. */
__attribute__((weak)) uint32_t platform_caps(void) { return 0; }

int vm_caps_ok(uint32_t requires, uint32_t caps, uint32_t *missing) {
    uint32_t m = requires & ~caps & ~CAP_ANYPORT;
    if (missing) *missing = m;
    return m == 0;
}

static char *put_names(char *p, uint32_t bits) {
    static const char names[4][6] = {"net", "kbd", "power", "sd"};   /* CAP_NET.. in bit order */
    for (int i = 0; i < 4; i++) {
        if (!(bits & (1u << i))) continue;
        if (p[-1] != ' ') *p++ = ' ';
        for (const char *s = names[i]; *s; s++) *p++ = *s;
    }
    return p;
}

size_t vm_caps_explain(char *buf, uint32_t requires, uint32_t missing) {
    static const char a[] = "gogo: image needs", b[] = "; firmware lacks";
    char *p = buf;
    for (const char *s = a; *s; s++) *p++ = *s;
    p = put_names(p, requires & ~CAP_ANYPORT);
    for (const char *s = b; *s; s++) *p++ = *s;
    p = put_names(p, missing);
    *p++ = '\n';
    return (size_t)(p - buf);
}
