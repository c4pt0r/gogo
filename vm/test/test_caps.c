#include "test.h"
#include "platform.h"
#include <string.h>

TEST(caps_ok_table) {
    uint32_t miss = 99;
    CHECK_EQ(vm_caps_ok(0, 0, &miss), 1);
    CHECK_EQ(miss, 0);
    CHECK_EQ(vm_caps_ok(CAP_KBD, CAP_KBD | CAP_NET, &miss), 1);
    CHECK_EQ(vm_caps_ok(CAP_KBD | CAP_SD, CAP_KBD, &miss), 0);
    CHECK_EQ(miss, CAP_SD);
    CHECK_EQ(vm_caps_ok(CAP_NET | CAP_POWER, 0, &miss), 0);
    CHECK_EQ(miss, CAP_NET | CAP_POWER);
    /* AnyPort is never missing, with or without other caps. */
    CHECK_EQ(vm_caps_ok(CAP_ANYPORT, 0, &miss), 1);
    CHECK_EQ(miss, 0);
    CHECK_EQ(vm_caps_ok(CAP_ANYPORT | CAP_KBD, CAP_NET, &miss), 0);
    CHECK_EQ(miss, CAP_KBD);
    CHECK_EQ(vm_caps_ok(CAP_KBD, 0, NULL), 0);   /* missing may be NULL */
}

static void explain(char *buf, uint32_t req, uint32_t miss) {
    size_t n = vm_caps_explain(buf, req, miss);
    CHECK(n < VM_CAPS_MSG_MAX);
    buf[n] = 0;
}

TEST(caps_explain) {
    char b[VM_CAPS_MSG_MAX];
    explain(b, CAP_KBD, CAP_KBD);
    CHECK(strcmp(b, "gogo: image needs kbd; firmware lacks kbd\n") == 0);
    /* a needed bit that is present is named in needs only; AnyPort never */
    explain(b, CAP_KBD | CAP_SD | CAP_ANYPORT, CAP_SD);
    CHECK(strcmp(b, "gogo: image needs kbd sd; firmware lacks sd\n") == 0);
    explain(b, 15, 15);
    CHECK(strcmp(b, "gogo: image needs net kbd power sd; firmware lacks net kbd power sd\n") == 0);
}

/* The weak default of a platform that defines none is tested by the
 * strong unix definition overriding it: all four bits. */
TEST(caps_unix_platform) {
    CHECK_EQ(platform_caps(), CAP_NET | CAP_KBD | CAP_POWER | CAP_SD);
}
