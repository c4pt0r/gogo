# runtime/include

`gogo.h` is the embedding API: everything a program needs to run gogo
images inside it. `gogo_errors.h`, which it includes, holds only
constants: the error codes gogo programs see (`gogo/errors`,
`GOGO_E_*`). The API's results are those codes negated (`GOGO_EINVAL`
is `-GOGO_E_BADARG`), and `gogo_strerror` describes both. The book chapter
[Embedding gogo](../../docs/book/18-embedding.md) is the guide.

## libgogo.a

```sh
make lib            # build/libgogo.a (and build/libgogo-debug.a)
cc -std=c99 -Iruntime/include -o host host.c build/libgogo.a -pthread
```

The archive holds the VM core (`runtime/vm`), the file system layer
(`runtime/vfs`), streams (`runtime/stream`) and the embedding layer
(`runtime/embed`), compiled as strict C99 (`-std=c99 -pedantic`) with
`-DGOGO_EMBED`. The keyboard, power, IMU, IR, audio, network and TLS ports
are not in it: its only drivers are the host ports a program registers,
and its only mount is `/flash` on a host directory. It has no mutable global state
(`tools/globals.sh --objects`), so any number of VMs can run in one
process, on any threads. It needs POSIX threads and clocks for
`gogo_host_posix`, `gogo_new_posix` and `gogo_run_thread`.

## Symbols

The library exports exactly the `gogo_*` functions declared in `gogo.h`
(marked `GOGO_API`), which are the list in `libgogo.syms`. `make lib` compiles the sources with
`-fvisibility=hidden` and prelinks them into one relocatable object
(`ld -r`) in which every other function is local: an export list on
Darwin, `objcopy -w --keep-global-symbol='gogo_*'` on ELF (set `LIB_LD`
and `LIB_OBJCOPY` for a cross toolchain). A host may define functions of
any name, `msg_free` or `vm_step` included; `make test-libgogo` links
such a host. `tools/libsyms.sh` fails the build if the archive's global
symbols, or the header's `GOGO_API` functions, are not exactly the names
in `libgogo.syms`: a change to the API is always a reviewed change to
that list.

`build/libgogo-debug.a` holds the same objects archived as they are. On
macOS `ld -r` keeps a debug map that points at `build/lib/*.o` rather
than the DWARF itself, so debugging a program linked with `libgogo.a`
works while those objects exist; to debug elsewhere, link
`libgogo-debug.a` instead. Its internal functions are not hidden (a host
symbol of the same name is a duplicate), so it is for debugging only.
