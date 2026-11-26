/* gogo_errors.h: the one error code space, as the C side spells it. The
 * same numbers as gogo/errors (lib/errors/errors.gg), name for name:
 * errors.NotFound is GOGO_E_NOTFOUND. tests/errors_test.go keeps the two
 * equal. Constants only: no functions, no types.
 *
 * 0 is OK; 1..35 are the shared meanings (28..35 free for later ones), so
 * that actor.Failed (64) + a shared code is an exit reason below 100;
 * 36..99 are reserved; codes >= 100 belong to one package each, in blocks
 * of 20 (gogo/errors lists them). These are the codes gogo
 * programs see. They are not the embedding API's results: gogo.h's GOGO_EINVAL,
 * GOGO_EBUSY ... are negative and unrelated. */
#ifndef GOGO_ERRORS_H
#define GOGO_ERRORS_H

/* The largest shared code (actor.Failed + it < 100). */
#define GOGO_E_SHARED_MAX 35

enum {
    GOGO_E_OK = 0,
    GOGO_E_NOTFOUND = 1,
    GOGO_E_EXISTS = 2,
    GOGO_E_NOMEMORY = 3,
    GOGO_E_TOOMANY = 4,
    GOGO_E_BUSY = 5,
    GOGO_E_BADARG = 6,
    GOGO_E_CLOSED = 7,
    GOGO_E_IO = 8,
    GOGO_E_TIMEOUT = 9,
    GOGO_E_UNSUPPORTED = 10,
    GOGO_E_NOSPACE = 11,
    GOGO_E_BADPATH = 12,
    GOGO_E_ISDIR = 13,
    GOGO_E_NOTDIR = 14,
    GOGO_E_NOTEMPTY = 15,
    GOGO_E_NOTMOUNTED = 16,
    GOGO_E_BADIMAGE = 17,
    GOGO_E_REFUSED = 18,
    GOGO_E_RESET = 19,
    GOGO_E_NOROUTE = 20,
    GOGO_E_BADCERT = 21,
    GOGO_E_NOTIME = 22,
    GOGO_E_TLS = 23,
    GOGO_E_OVERFLOW = 24,
    GOGO_E_BADURL = 25,
    GOGO_E_BADFORMAT = 26,
    GOGO_E_BADVERSION = 27
};

#endif
