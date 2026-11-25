/* Firmware capability check (host.h CAP_*): an image's requires word
 * against what the Vm may use. Pure: no I/O; callers decide what to do
 * with a refusal. */
#include "host.h"

int vm_caps_ok(uint32_t requires, uint32_t caps, uint32_t *missing) {
    uint32_t m = requires & ~caps & ~CAP_ANYPORT;
    if (missing) *missing = m;
    return m == 0;
}

static char *put_names(char *p, uint32_t bits) {
    /* the width 6 is "power" plus NUL; widen it with the longest new name */
    static const char names[][6] = {"net", "kbd", "power", "sd", "imu", "irtx", "irrx", "mic", "spk", "tls"};   /* CAP_NET.. in bit order */
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
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
