/* win_compat.h: the small POSIX-to-Windows shims the shared unix platform
 * sources need when they are compiled for the Windows host. Forced in with
 * gcc's -include for platform/unix/main.c only (build_vm_win.ps1), so no
 * shared source is edited.
 *
 * Only things whose Windows form differs in *signature* live here (main.c's
 * two-argument mkdir, SIGPIPE). Everything else the MinGW-w64 CRT already
 * provides under the POSIX name (read, write, isatty, nanosleep,
 * clock_gettime, getenv, signal, stat, S_ISDIR, ...). The console (termios,
 * VT output, UTF-8 code page) is platform/windows/console_win.c's job.
 */
#ifndef GOGO_WIN_COMPAT_H
#define GOGO_WIN_COMPAT_H

#ifndef _WIN32
#error "win_compat.h is for the Windows port only"
#endif

/* The headers that declare mkdir/_mkdir before the macro below is defined:
 * MinGW's io.h declares both, so include it first and then redirect the
 * POSIX two-argument spelling (platform/unix/main.c's --fs-root) to the
 * one-argument CRT call. */
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIGPIPE
/* No SIGPIPE on Windows: main.c ignores it at startup, and a broken pipe
 * is a failed write instead (13 is not a signal the CRT knows, so
 * signal(SIGPIPE, SIG_IGN) just returns SIG_ERR). */
#define SIGPIPE 13
#endif

/* main.c: mkdir(path, 0777) / mkdir(dir, 0777). */
#define mkdir(path, mode) _mkdir(path)

#endif /* GOGO_WIN_COMPAT_H */
