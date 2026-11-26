# packages: your own packages and shared ones (GOGOPATH)

A program finds an imported package in two places, in this order
(book chapter 3):

1. **The build root**: the directory you build. Its name starts its own
   imports: building `app/`, `import "app/sensors"` is `app/sensors/`.
2. **The search path**, `$GOGOPATH` (default `~/gogo`): packages shared
   between programs. `import "units/temp"` is `<dir>/units/temp/` for the
   first directory of `$GOGOPATH` that has it.

```
examples/packages/
  app/                    the program (build root "app")
    main.gg               package main: imports app/sensors and units/temp
    sensors/sensors.gg    package sensors: the program's own; imports units/stats and units/temp
  gopath/                 a GOGOPATH directory
    units/temp/temp.gg    package temp: type Deci, the exported struct Range
    units/stats/stats.gg  package stats: imports units/temp, returns a temp.Range
  hello.gg                a script (#!) that imports units/temp
```

So it shows a package of the program (`app/sensors`), shared packages
(`units/...`), packages that import packages (`app/sensors` imports both
`units` packages, `units/stats` imports `units/temp`), and an exported
struct (`temp.Range`) built in one package (`units/stats`) and read in
another (`main`).

From the repository root (with `build/gogo` and `build/gogovm`: `make vm`
and `go build -o build/gogo ./cmd/gogo`):

```sh
$ GOGOPATH=examples/packages/gopath build/gogo run examples/packages/app
desk: 4 samples, 21.2C to 21.9C; max 71 F
window: 4 samples, -1.5C to 2.1C; max 35 F
```

Without `GOGOPATH` the build stops at the first shared import and lists
where it looked:

```text
examples/packages/app/sensors/sensors.gg:7:2: gogo: import "units/stats": package not found; searched:
	examples/packages/app (build root: serves only imports starting with "app/")
	...
```

`gogo env` shows the search path a build would use (`-path dirs` sets it
for one command instead of `$GOGOPATH`):

```text
$ GOGOPATH=examples/packages/gopath build/gogo env
version   dev (image format 15.1)
GOGOPATH  /.../examples/packages/gopath ($GOGOPATH)
search    1. the build root (imports starting with its directory's name)
          2. /.../examples/packages/gopath
gogovm    /.../build/gogovm (next to gogo)
```

The script runs by itself once `gogo` is on your `PATH`; it imports from
`$GOGOPATH` like a program does:

```sh
$ PATH=$PWD/build:$PATH GOGOPATH=examples/packages/gopath examples/packages/hello.gg
hello from a script; room 21.5C
```

On a board, build the image the same way and flash it
(`GOGOPATH=examples/packages/gopath build/gogo build examples/packages/app -o app.ggb`,
then `gogo flash app.ggb`): packages are compiled into the image, so the
board needs no GOGOPATH.

`make test-examples` builds and runs both, with `GOGOPATH` set from the
file `gogopath` here (a path relative to this directory), and compares
their output with `app/expected.out` and `hello.expected.out`.
