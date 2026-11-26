package main

// The exit status of `gogo run` and of the host VM (platform/unix/main.c,
// which computes it; gogo run passes the VM's status through unchanged).
// Every other gogo command exits 0 on success and exitUsage on any error.
//
//	0        ok, or the main actor exited Normal
//	1        usage, toolchain or build error (gogo run: also the VM not found)
//	2        bad image, or an image format this VM does not run
//	3        the image requires a driver or mount the VM lacks
//	4        the main actor crashed (a VM reason: killed, oom, divzero, ...)
//	64..99   main exited with a library failure (actor.Failed + a shared
//	         gogo/errors code), passed through
//	100..255 main exited with a user code (actor.Exit(code), code >= 100);
//	         a code above 255 is 255
//	128+N    gogo run only: the VM was killed by signal N
//
// Only the main actor's exit counts: when main exits Normal, crashes of
// other actors (reported on standard error) leave the status 0.
const (
	exitOK       = 0
	exitUsage    = 1
	exitBadImage = 2
	exitMissing  = 3
	exitCrashed  = 4
)
