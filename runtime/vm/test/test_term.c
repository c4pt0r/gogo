#include "test.h"
#include "term.h"
#include "opcodes.h"

TEST(int_roundtrip) { int32_t v[] = {0,1,-1,1073741823,-1073741824};
  for (int i=0;i<5;i++){ CHECK(term_int_fits(v[i])); CHECK_EQ(term_to_int(term_from_int(v[i])), v[i]); CHECK(term_is_int(term_from_int(v[i]))); } }
TEST(int_fits_bounds) { CHECK(!term_int_fits(1073741824)); CHECK(!term_int_fits(-1073741825)); }
TEST(tags_disjoint) { term r=term_from_ref(5), p=term_make_pid(7,3), c=term_from_const(9);
  CHECK(term_is_ref(r)&&!term_is_pid(r)&&!term_is_const(r)&&!term_is_int(r));
  CHECK(term_is_pid(p)&&!term_is_ref(p)); CHECK(term_is_const(c)&&!term_is_ref(c));
  CHECK_EQ(term_ref_off(r),5); CHECK_EQ(term_pid_slot(p),7); CHECK_EQ(term_pid_gen(p),3); CHECK_EQ(term_const_off(c),9);
  CHECK(!term_is_ref(T_NIL) && !term_is_ref(T_TRUE) && !term_is_int(T_FALSE)); }
TEST(header) { uint32_t h=hdr_make(5,K_STRUCT); CHECK_EQ(hdr_words(h),5); CHECK_EQ(hdr_kind(h),K_STRUCT); }

TEST(literal_encodings) {
    CHECK_EQ(term_from_int(5), 0xB);
    CHECK_EQ(T_NIL, 0x2);
    CHECK_EQ(T_FALSE, 0x12);
    CHECK_EQ(T_TRUE, 0x22);
    CHECK_EQ(term_make_pid(7, 3), 0x00300076);
    CHECK_EQ(term_from_const(9), 0x9A);
    CHECK_EQ(term_from_ref(5), 0x14);
    CHECK_EQ(hdr_make(5, K_STRUCT), 0x141);
}
TEST(op_ext_table) {
    for (uint32_t op = 0; op <= OP_BUILTIN; op++) {
        bool ext = op == OP_LOADK || op == OP_LOADI32 || op == OP_JMP || op == OP_JMPF ||
                   op == OP_JMPT || op == OP_CALL || op == OP_NEWSTRUCT ||
                   op == OP_MKCLOSURE || op == OP_TYPEIS || op == OP_BUILTIN;
        CHECK_EQ(op_has_ext(op), ext);
    }
}
TEST(op_roundtrip) {
    uint32_t w = OP_MAKE(OP_CALL, 1, 2, 3);
    CHECK_EQ(OP_OP(w), OP_CALL); CHECK_EQ(OP_A(w), 1); CHECK_EQ(OP_B(w), 2); CHECK_EQ(OP_C(w), 3);
    w = OP_MAKE(OP_MOVE, 255, 128, 255);
    CHECK_EQ(OP_OP(w), OP_MOVE); CHECK_EQ(OP_A(w), 255); CHECK_EQ(OP_B(w), 128); CHECK_EQ(OP_C(w), 255);
}
