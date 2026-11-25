# runtime/include

`gogo.h` is the embedding API: everything a program needs to run gogo
images inside it. The book chapter
[Embedding gogo](../../docs/book/18-embedding.md) is the guide.

## libgogo.a

```sh
make lib            # build/libgogo.a
cc -std=c99 -Iruntime/include -o host host.c build/libgogo.a -pthread
```

The archive holds the VM core (`runtime/vm`), the file system layer
(`runtime/vfs`), streams (`runtime/stream`) and the embedding layer
(`runtime/embed`), compiled as strict C99 (`-std=c99 -pedantic`) with
`-DGOGO_EMBED`. The keyboard, power, IMU, IR, audio, network and TLS ports
are not in it: a VM made from it grants no capability bits (see
`GOGO_CAP_*` in `gogo.h`). It has no mutable global state
(`tools/globals.sh --objects`), so any number of VMs can run in one
process, on any threads. It needs POSIX threads and clocks for
`gogo_host_posix`, `gogo_new_posix` and `gogo_run_thread`.

## Symbols

Only the `gogo_*` functions declared in `gogo.h` are public. A static
archive cannot hide the library's internal functions, so it also defines
external symbols with these prefixes, which are reserved: a program
linking libgogo.a must not define symbols that start with them.

| prefix | from |
|---|---|
| `vm_`, `va_` | the VM and its allocator |
| `actor_`, `bytes_`, `const_`, `evq_`, `heap_`, `image_`, `interp_`, `module_`, `msg_`, `names_`, `out_`, `port_`, `runq_`, `sig_`, `signal_`, `strs_`, `timer_` | the VM core |
| `vfs_`, `fs_dir_` | the file system layer and the host directory behind `/flash` |
| `stream_` | streams |
| `embed_`, `inbox_` | the embedding layer |

`make lib` runs `tools/libsyms.sh`, which fails if the archive defines a
symbol outside this list (or a `gogo_*` symbol outside `runtime/embed`).
Keep the two in sync.
