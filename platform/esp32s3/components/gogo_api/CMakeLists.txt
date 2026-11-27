# gogo's stable C API (runtime/include: gogo.h, gogo_errors.h) for driver
# components outside this tree: REQUIRES gogo_api gives them the headers
# and nothing else (no VM headers). Header-only; the calls are main's.
idf_component_register(INCLUDE_DIRS "../../../../runtime/include")
