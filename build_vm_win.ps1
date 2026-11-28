# build_vm_win.ps1: builds build\gogovm.exe, the gogo bytecode VM host, for
# Windows x64 with MinGW-w64 gcc. There is no make on this machine, so this
# script mirrors the Makefile's $(BUILD)/gogovm rule (Makefile lines 26-58
# and 170-176) file by file and flag by flag:
#
#   VM_SRCS    runtime/vm/*.c (but check32.c), runtime/floatlib/*.c,
#              runtime/gpio/gpio.c, runtime/net/*.c, runtime/stream/*.c
#   MAIN_SRCS  platform/unix/main.c, platform/unix/screen_unix.c,
#              runtime/ble/*.c, runtime/board/{board_main,board_ports,params}.c,
#              runtime/embed/{hostport,inbox,poster,version}.c,
#              runtime/vfs/*.c, runtime/input/*.c, runtime/dist/*.c,
#              runtime/screen/{screen,screen_port}.c,
#              runtime/console/{console,font}.c
#   platform   platform/unix/gpio_unix.c and platform/unix/input_unix.c are
#              portable given the Windows console layer (console_win.c), so
#              they are compiled as they are; platform/windows/platform_win.c
#              replaces platform_unix.c, hostnet_win.c replaces hostnet.c,
#              ble_win.c replaces ble_unix.c, fs_dir_win.c replaces
#              runtime/embed/fs_dir.c (POSIX-only), and console_win.c adds
#              the console modes and the termios shim.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File build_vm_win.ps1
#   ... -Clean             # wipe build\win first
#   ... -ShowCommands      # print every compiler command
#
# The includes and defines are the Makefile's CFLAGS_BASE / CFLAGS /
# BOARD_PORTS_FLAGS / GOGOVM_FLAGS; -Werror is deliberately not used so the
# port keeps warnings visible without failing the build.

param(
    [string]$Gcc = 'C:\mingw64\bin\gcc.exe',
    [string]$Out = '',                 # default: build\gogovm.exe
    # mbedTLS source tree for the "tls" port (the Makefile's MBEDTLS_DIR): a
    # mbedTLS 3.x/4.x tree with library/ssl_tls.c. Point -MbedtlsDir at yours,
    # or -NoTls to build without the port (a clean message, as unix does when
    # the tree is missing).
    [string]$MbedtlsDir = 'D:\mmgame\mbedtls4',
    [switch]$NoTls,
    [switch]$Clean,
    [switch]$ShowCommands
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not $root) { $root = Split-Path -Parent $MyInvocation.MyCommand.Definition }
Set-Location -LiteralPath $root

$build = Join-Path $root 'build'
$objdir = Join-Path $build 'win\obj'
$exe = if ($Out) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out) } else { Join-Path $build 'gogovm.exe' }

if ($Clean -and (Test-Path -LiteralPath $objdir)) { Remove-Item -Recurse -Force -LiteralPath $objdir }
New-Item -ItemType Directory -Force -Path $objdir | Out-Null

# Adds every *.c of a repo-relative directory to $srcs (sorted, absolute).
$srcs = New-Object System.Collections.Generic.List[string]
function Add-Src([string]$dir) {
    $full = Join-Path $root $dir
    Get-ChildItem -LiteralPath $full -File -Filter *.c | Sort-Object Name | ForEach-Object {
        $srcs.Add($_.FullName)
    }
}
function Add-File([string]$rel) { $srcs.Add((Join-Path $root $rel)) }

Add-Src 'runtime/vm'
Add-Src 'runtime/floatlib'
Add-File 'runtime/gpio/gpio.c'
Add-File 'platform/unix/gpio_unix.c'
Add-File 'platform/unix/input_unix.c'
Add-Src 'runtime/net'
Add-Src 'runtime/stream'
# MAIN_SRCS
Add-File 'platform/unix/main.c'
Add-File 'platform/unix/screen_unix.c'
Add-Src 'runtime/ble'
Add-Src 'runtime/board'
Add-File 'runtime/embed/hostport.c'
Add-File 'runtime/embed/inbox.c'
Add-File 'runtime/embed/poster.c'
Add-File 'runtime/embed/version.c'
Add-Src 'runtime/vfs'
Add-Src 'runtime/input'
Add-File 'runtime/screen/screen.c'
Add-File 'runtime/screen/screen_port.c'
Add-File 'runtime/console/console.c'
Add-File 'runtime/console/font.c'
Add-Src 'runtime/dist'
# the Windows platform layer (only these five .c files exist here)
Add-File 'platform/windows/platform_win.c'
Add-File 'platform/windows/console_win.c'
Add-File 'platform/windows/hostnet_win.c'
Add-File 'platform/windows/ble_win.c'
Add-File 'platform/windows/fs_dir_win.c'
Add-File 'platform/windows/entropy_win.c'

# check32.c is a build-time size check, not part of the VM.
$srcs = @($srcs | Where-Object { (Split-Path $_ -Leaf) -ne 'check32.c' } | Select-Object -Unique)

# --- the "tls" port (optional, as on unix: the Makefile's MBEDTLS_DIR) ---
# With a mbedTLS source tree the VM gets the tls port and HTTPS works; without
# one it is built exactly like `make vm` on a machine without ESP-IDF: no tls
# port, --has-tls exits 1, and an image that requires it is refused cleanly.
$tls = $false
$tls4 = $false                    # mbedTLS 4.x / TF-PSA-Crypto layout (the Makefile's)
$mbedtlsInc = ''
$mbedtlsLib = Join-Path $build 'win\libgogo_mbedtls.a'
if (-not $NoTls -and $MbedtlsDir -and (Test-Path -LiteralPath (Join-Path $MbedtlsDir 'library\ssl_tls.c'))) {
    $tls = $true
    $tls4 = Test-Path -LiteralPath (Join-Path $MbedtlsDir 'tf-psa-crypto\include')
    $mbedtlsInc = (Join-Path $MbedtlsDir 'include') -replace '\\', '/'
    $srcs += @(
        (Join-Path $root 'runtime/net/tls/tls.c'),
        (Join-Path $root 'runtime/net/tls/anchor.c'),
        # tls_unix.c is pure mbedTLS configuration (no sockets): it compiles
        # here as it is, given platform/windows/include/sys/random.h.
        (Join-Path $root 'platform/unix/tls_unix.c')
    )
}

# --- flags: the Makefile's CFLAGS_BASE, BOARD_PORTS_FLAGS, GOGOVM_FLAGS ---
$buildUnixMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$defines = @(
    "-DGOGO_BUILD_UNIX_MS=$($buildUnixMs)LL",
    '-DGOGO_BOARD_PORTS=1',
    '-DEMBED_HOST_PORTS=8',
    '-DEMBED_BOARD_PORTS_MAX=32',
    '-DGOGO_BOARD_POST_BYTES=1048576u',
    '-DGOGO_MAX_SOURCES=5u',
    '-D__USE_MINGW_ANSI_STDIO=1'   # C99 printf: %zu, %llu, ...
)
$includes = @(
    '-Iruntime/input', '-Iruntime/vm', '-Iruntime/net', '-Iruntime/vfs',
    '-Iruntime/stream', '-Iruntime/screen', '-Iruntime/console',
    '-Iruntime/embed', '-Iruntime/gpio', '-Iruntime/board', '-Iruntime/dist',
    '-Iruntime/ble', '-Iruntime/include',
    '-Iplatform/unix', '-Iplatform/windows', '-Iplatform/windows/include'
)
# 4.x needs the repo's mbedTLS config headers selected with -D...="file",
# which PowerShell cannot pass to a native program intact (it drops the
# quotes), so those go through a response file for every compile.
$tlsRsp = $null
if ($tls) {
    # The Makefile's TLS_CFLAGS: GOGO_TLS and mbedTLS's public headers.
    $defines += '-DGOGO_TLS'
    $includes += '-Iruntime/net/tls'
    if ($tls4) {
        $M = $MbedtlsDir -replace '\\', '/'
        $includes += @(
            '-Iruntime/net/tls/config',
            "-I$M/include", "-I$M/tf-psa-crypto/include",
            "-I$M/tf-psa-crypto/drivers/builtin/include"
        )
        $tlsRsp = Join-Path $build 'win\tls_defs.rsp'
        Set-Content -LiteralPath $tlsRsp -Encoding ASCII -Value @(
            '-DMBEDTLS_CONFIG_FILE=\"gogo_mbedtls_config.h\"',
            '-DTF_PSA_CRYPTO_CONFIG_FILE=\"gogo_crypto_config.h\"'
        )
    } else {
        # mbedTLS 3.x: its own default config; the platform's mbedTLS
        # supplies allocator/clock/entropy (GOGO_TLS_MBEDTLS_HOOKS 0), and
        # restartable ECC is what slices a handshake there (4.x has
        # interruptible PSA built in).
        $defines += @('-DGOGO_TLS_MBEDTLS_HOOKS=0', '-DMBEDTLS_ECP_RESTARTABLE')
        $includes += "-I$mbedtlsInc"
    }
}
$cflags = @('-std=c99', '-O2', '-g', '-Wall', '-Wextra') + $defines + $includes
$cflags += '-pthread'   # ble_win.c's radio thread; tls.c serializes tls_init

$winCompat = (Join-Path $root 'platform/windows/win_compat.h') -replace '\\', '/'

# Newest header under runtime/ and platform/: a changed header rebuilds every
# object (simple and safe; the build is small).
$newestHeader = Get-ChildItem -Path (Join-Path $root 'runtime'), (Join-Path $root 'platform') `
    -Recurse -File -Filter *.h | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$headerTime = if ($newestHeader) { $newestHeader.LastWriteTime } else { [DateTime]::MinValue }

$objs = New-Object System.Collections.Generic.List[string]
$compiled = 0
$failed = New-Object System.Collections.Generic.List[string]
foreach ($src in $srcs) {
    $rel = $src.Substring($root.Length).TrimStart('\', '/')
    $objName = ($rel -replace '[\\/]', '_') -replace '\.c$', '.o'
    $obj = Join-Path $objdir $objName
    $objs.Add($obj)
    $need = $true
    if (Test-Path -LiteralPath $obj) {
        $objTime = (Get-Item -LiteralPath $obj).LastWriteTime
        if ($objTime -gt (Get-Item -LiteralPath $src).LastWriteTime -and $objTime -gt $headerTime) { $need = $false }
    }
    if (-not $need) { continue }
    $compiled++
    # main.c is the one unix file that needs the compatibility header (its
    # mkdir(dir, 0777) and SIGPIPE); everything else uses the CRT as is.
    $extra = @()
    if ((Split-Path $src -Leaf) -eq 'main.c') { $extra = @('-include', $winCompat) }
    $gargs = @('-c', $src, '-o', $obj) + $cflags + $extra
    if ($tlsRsp) { $gargs += ('@' + ($tlsRsp -replace '\\', '/')) }
    if ($ShowCommands) { Write-Host "gcc $($gargs -join ' ')" }
    & $Gcc @gargs
    if ($LASTEXITCODE -ne 0) {
        $failed.Add($rel)
        Write-Host "FAILED: $rel" -ForegroundColor Red
    }
}
if ($failed.Count -gt 0) {
    Write-Host "compile failed: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "compiled $compiled file(s) of $($srcs.Count)"

# --- mbedTLS: the static library behind the tls port ---
# The Makefile's $(BUILD)/libgogo_mbedtls.a. Two source layouts:
#   4.x + TF-PSA-Crypto (what the Makefile builds against ESP-IDF's tree):
#     library/*.c, tf-psa-crypto/core/*.c, tf-psa-crypto/drivers/builtin/src/*.c,
#     with the repo's own config headers (runtime/net/tls/config) selected by
#     the -D...="file" defines in tlsRsp, and -Iruntime/net/tls/host for the
#     headers that redirect private includes;
#   3.x: library/*.c with the tree's default config.
# -w: it is not our code to warn about.
$linkExtra = @()
if ($tls) {
    if ($tls4) { $mobjdir = Join-Path $build 'win\mbedtls4' } else { $mobjdir = Join-Path $build 'win\mbedtls' }
    New-Item -ItemType Directory -Force -Path $mobjdir | Out-Null
    $mobjs = New-Object System.Collections.Generic.List[string]
    $mcompiled = 0
    $mfail = New-Object System.Collections.Generic.List[string]
    $M = $MbedtlsDir -replace '\\', '/'
    $msrcs = @()
    $msrcs += @(Get-ChildItem -LiteralPath (Join-Path $MbedtlsDir 'library') -File -Filter *.c)
    if ($tls4) {
        $tf = "$M/tf-psa-crypto"
        # The *_config.c files only #include generated config-check headers;
        # they define nothing and are left out.
        $msrcs = @($msrcs | Where-Object { $_.BaseName -notmatch '_config$' })
        $msrcs += @(Get-ChildItem -LiteralPath (Join-Path $MbedtlsDir 'tf-psa-crypto\core') -File -Filter *.c |
            Where-Object { $_.BaseName -notmatch '_config$' })
        $msrcs += @(Get-ChildItem -LiteralPath (Join-Path $MbedtlsDir 'tf-psa-crypto\drivers\builtin\src') -File -Filter *.c)
        $margsBase = @('-std=c99', '-O2', '-w',
            '-Iruntime/net/tls/config', '-Iruntime/net/tls/host',
            "-I$M/include", "-I$tf/include", "-I$tf/drivers/builtin/include",
            "-I$M/library", "-I$tf/core", "-I$tf/drivers/builtin/src")
    } else {
        $margsBase = @('-std=c99', '-O2', '-w', '-DMBEDTLS_ECP_RESTARTABLE',
            "-I$M/include", "-I$M/library")
    }
    if ($tls4) { $mrsp = Join-Path $build 'win\mbedtls4_flags.rsp' } else { $mrsp = Join-Path $build 'win\mbedtls_flags.rsp' }
    $mlines = @()
    if ($tls4) { $mlines += @('-DMBEDTLS_CONFIG_FILE=\"gogo_mbedtls_config.h\"', '-DTF_PSA_CRYPTO_CONFIG_FILE=\"gogo_crypto_config.h\"') }
    foreach ($a in $margsBase) { $mlines += $a }
    Set-Content -LiteralPath $mrsp -Encoding ASCII -Value $mlines
    foreach ($src in ($msrcs | Sort-Object Name)) {
        if ($src.BaseName -eq 'godot_core_mbedtls_platform') { continue }   # a vendor's glue, not mbedTLS
        $obj = Join-Path $mobjdir ($src.BaseName + '.o')
        $mobjs.Add($obj)
        if ((Test-Path -LiteralPath $obj) -and
            ((Get-Item -LiteralPath $obj).LastWriteTime -gt $src.LastWriteTime)) { continue }
        $mcompiled++
        $margs = @(('@' + ($mrsp -replace '\\', '/')), '-c', $src.FullName, '-o', $obj)
        if ($ShowCommands) { Write-Host "gcc $($margs -join ' ')" }
        & $Gcc @margs
        if ($LASTEXITCODE -ne 0) { $mfail.Add($src.Name) }
    }
    if ($mfail.Count -gt 0) {
        Write-Host ("mbedTLS compile failed: " + ($mfail -join ', ')) -ForegroundColor Red
        exit 1
    }
    $ar = Join-Path (Split-Path $Gcc -Parent) 'ar.exe'
    if (-not (Test-Path -LiteralPath $ar)) { $ar = 'ar' }
    if (Test-Path -LiteralPath $mbedtlsLib) { Remove-Item -Force -LiteralPath $mbedtlsLib }
    & $ar rcs $mbedtlsLib @($mobjs)
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'ar failed' -ForegroundColor Red
        exit 1
    }
    $mver = '3.x'; if ($tls4) { $mver = '4.x + TF-PSA-Crypto' }
    Write-Host ("mbedTLS {0}: compiled {1}, lib {2:N0} bytes" -f $mver, $mcompiled, (Get-Item -LiteralPath $mbedtlsLib).Length)
    $linkExtra = @('-pthread', ($mbedtlsLib -replace '\\', '/'))
} elseif ($true) {
    $linkExtra = @('-pthread')
} else {
    Write-Host 'no mbedTLS source: building WITHOUT the tls port (as the Makefile does)' -ForegroundColor Yellow
}

# --- link ---
# Console subsystem (gcc's default), an 8 MB stack like the unix default (the
# Vm lives on main's stack), ws2_32 for the net stack, bcrypt for
# BCryptGenRandom in platform_win.c. Objects go through a response file so
# the command line never gets too long.
$rsp = Join-Path $build 'win\objs.rsp'
$lines = @()
# gcc's @file parser treats a backslash as an escape: use forward slashes.
foreach ($o in $objs) { $lines += ('"' + ($o -replace '\\', '/') + '"') }
Set-Content -LiteralPath $rsp -Value $lines -Encoding ASCII
# The mbedTLS library goes BEFORE the system libraries: like ELF linkers,
# MinGW's ld resolves a library's imports from what follows it.
$linkBase = @(('@' + $rsp)) + $linkExtra + @('-lws2_32', '-lbcrypt', '-Wl,--stack,8388608')
$largs = @('-o', $exe) + $linkBase
if ($ShowCommands) { Write-Host "gcc $($largs -join ' ')" }
& $Gcc @largs
if ($LASTEXITCODE -ne 0 -and (Test-Path -LiteralPath $exe)) {
    # A running .exe cannot be overwritten (another session's `gogo run`
    # may be executing it right now). Windows does allow renaming it, so
    # link beside it and swap the two names.
    Write-Host 'output is in use: linking beside it and swapping' -ForegroundColor Yellow
    $tmp = Join-Path $build 'win\gogovm_new.exe'
    $prev = Join-Path $build 'win\gogovm_prev.exe'
    $largs2 = @('-o', $tmp) + $linkBase
    & $Gcc @largs2
    if ($LASTEXITCODE -eq 0) {
        Move-Item -Force -LiteralPath $exe -Destination $prev -ErrorAction SilentlyContinue
        Move-Item -Force -LiteralPath $tmp -Destination $exe
        Remove-Item -Force -LiteralPath $prev -ErrorAction SilentlyContinue
    }
}
if ($LASTEXITCODE -ne 0) {
    Write-Host 'link failed' -ForegroundColor Red
    exit 1
}
$size = (Get-Item -LiteralPath $exe).Length
Write-Host ("built {0} ({1:N0} bytes)" -f $exe, $size) -ForegroundColor Green
