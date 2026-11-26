package main

// gogo setup, the firmware side: download a release's firmware for one
// board and check it against the release's SHA256SUMS, or build it here
// with ESP-IDF when there is no release to download.
//
// Release assets (tools/release.sh and .github/workflows/release.yml make
// them; the names are fixed because this file depends on them):
//
//	SHA256SUMS                     sha256sum format, every other asset
//	<board>-flasher_args.json      ESP-IDF's build/flasher_args.json
//	<board>-bootloader.bin         \
//	<board>-partition-table.bin     } the files it names, each renamed to
//	<board>-gogo.bin               /  <board>-<base name>
//	gogo-<os>-<arch>[.exe]         the gogo command
//
// <board> is sticks3 or cardputer.

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strings"
)

const (
	releaseRepo   = "https://github.com/c4pt0r/gogo/releases"
	releaseSums   = "SHA256SUMS"
	maxAssetBytes = 16 << 20
)

// errUnavailable marks a release that cannot be fetched at all (no
// network, no such release, no such asset), as opposed to one that is
// wrong (a checksum mismatch), which is never worked around.
var errUnavailable = errors.New("release not available")

// releaseBase is the asset directory of the release to use: -release-url
// or $GOGO_RELEASE_URL if set, else the GitHub release named by -version,
// else the one matching this gogo's own version if it is a release build,
// else the latest release.
func releaseBase(flagURL, flagVersion string) string {
	switch {
	case flagURL != "":
		return flagURL
	case os.Getenv("GOGO_RELEASE_URL") != "":
		return os.Getenv("GOGO_RELEASE_URL")
	case flagVersion != "":
		return releaseRepo + "/download/" + flagVersion
	case isReleaseVersion(version):
		return releaseRepo + "/download/" + version
	}
	return releaseRepo + "/latest/download"
}

// releaseVersionRe matches the tags of releases: v1.2.3, v1.2.3-rc1,
// v1.2.3-beta.2. A `git describe` version such as v1.2.3-4-gabcdef or a
// local name like v1.2.3-local is not one: such a gogo uses the latest
// release.
var releaseVersionRe = regexp.MustCompile(`^v\d+\.\d+\.\d+(-(alpha|beta|rc)\.?\d+)?$`)

func isReleaseVersion(v string) bool { return releaseVersionRe.MatchString(v) }

// releaseSource reads assets from an http(s) URL or a local directory
// (a path or a file:// URL), e.g. one filled by `gh release download`.
type releaseSource struct {
	base   string
	client *http.Client
}

// latestSuffix ends the URL of GitHub's latest release, which redirects
// each asset to /download/<tag>/<asset>.
const latestSuffix = "/latest/download"

func (r releaseSource) isHTTP() bool {
	return strings.HasPrefix(r.base, "http://") || strings.HasPrefix(r.base, "https://")
}

// get reads one asset. For the latest release, the first request pins the
// release: r.base becomes the /download/<tag> it redirects to, so every
// file comes from the same release even if a new one is published
// meanwhile.
func (r *releaseSource) get(name string) ([]byte, error) {
	if r.isHTTP() {
		u := strings.TrimSuffix(r.base, "/") + "/" + url.PathEscape(name)
		client := *r.client
		var pinned string
		if strings.HasSuffix(strings.TrimSuffix(r.base, "/"), latestSuffix) {
			client.CheckRedirect = func(req *http.Request, via []*http.Request) error {
				if len(via) == 1 && strings.Contains(req.URL.Path, "/download/") && path.Base(req.URL.Path) == name {
					d := *req.URL
					d.Path = path.Dir(d.Path)
					d.RawQuery = ""
					pinned = d.String()
				}
				if r.client.CheckRedirect != nil {
					return r.client.CheckRedirect(req, via)
				}
				if len(via) >= 10 {
					return errors.New("too many redirects")
				}
				return nil
			}
		}
		resp, err := client.Get(u)
		if err != nil {
			return nil, fmt.Errorf("%w: %v", errUnavailable, err)
		}
		defer resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			return nil, fmt.Errorf("%w: %s: %s", errUnavailable, u, resp.Status)
		}
		b, err := io.ReadAll(io.LimitReader(resp.Body, maxAssetBytes+1))
		if err != nil {
			return nil, fmt.Errorf("%w: %s: %v", errUnavailable, u, err)
		}
		if len(b) > maxAssetBytes {
			return nil, fmt.Errorf("%s: larger than %d bytes", u, maxAssetBytes)
		}
		if pinned != "" {
			r.base = pinned
		}
		return b, nil
	}
	dir := strings.TrimPrefix(r.base, "file://")
	b, err := os.ReadFile(filepath.Join(dir, name))
	if err != nil {
		return nil, fmt.Errorf("%w: %v", errUnavailable, err)
	}
	return b, nil
}

// parseSums reads a SHA256SUMS file: "<64 hex>  <name>" (or " *<name>").
func parseSums(b []byte) (map[string]string, error) {
	sums := map[string]string{}
	for _, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		f := strings.Fields(line)
		if len(f) != 2 || len(f[0]) != 64 {
			return nil, fmt.Errorf("%s: bad line %q", releaseSums, line)
		}
		if _, err := hex.DecodeString(f[0]); err != nil {
			return nil, fmt.Errorf("%s: bad line %q", releaseSums, line)
		}
		sums[strings.TrimPrefix(f[1], "*")] = strings.ToLower(f[0])
	}
	if len(sums) == 0 {
		return nil, fmt.Errorf("%s is empty", releaseSums)
	}
	return sums, nil
}

// releaseAssetName is the asset holding the file flasher_args.json names
// (e.g. bootloader/bootloader.bin) for board.
func releaseAssetName(board, file string) string { return board + "-" + path.Base(file) }

// firmwareSet is one board's firmware, ready to flash.
type firmwareSet struct {
	source string              // where it came from, for the messages
	plan   *firmwarePlan       // nil in a dry run of a local build
	path   func(string) string // flasher_args.json file name -> file here
}

// fetchAsset returns the cached file for name, downloading and checking
// it against sums first if it is not cached yet. The cache is keyed by
// the checksum, so a file in it is never stale.
func (s *setup) fetchAsset(src *releaseSource, sums map[string]string, name string) (string, error) {
	want, ok := sums[name]
	if !ok {
		return "", fmt.Errorf("%w: %s is not in %s", errUnavailable, name, releaseSums)
	}
	dir := filepath.Join(s.env.cacheDir, "release")
	file := filepath.Join(dir, want)
	if b, err := os.ReadFile(file); err == nil && sha256hex(b) == want {
		fmt.Fprintf(s.env.out, "  %s (cached)\n", name)
		return file, nil
	}
	b, err := src.get(name)
	if err != nil {
		return "", err
	}
	if got := sha256hex(b); got != want {
		return "", fmt.Errorf("%s: checksum mismatch (got %s, %s says %s): refusing to use it", name, got, releaseSums, want)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	tmp, err := os.CreateTemp(dir, ".dl-*")
	if err != nil {
		return "", err
	}
	_, err = tmp.Write(b)
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err == nil {
		err = os.Rename(tmp.Name(), file)
	}
	if err != nil {
		os.Remove(tmp.Name())
		return "", err
	}
	fmt.Fprintf(s.env.out, "  %s (%d bytes, sha256 ok)\n", name, len(b))
	return file, nil
}

func sha256hex(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

// releaseFirmware downloads and checks board's firmware from the release.
func (s *setup) releaseFirmware(board string) (*firmwareSet, error) {
	src := &releaseSource{base: releaseBase(s.o.releaseURL, s.o.version), client: s.env.client}
	fmt.Fprintf(s.env.out, "  release: %s\n", src.base)
	b, err := src.get(releaseSums)
	if err != nil {
		return nil, err
	}
	if base := releaseBase(s.o.releaseURL, s.o.version); src.base != base {
		fmt.Fprintf(s.env.out, "  that is %s\n", src.base)
	}
	sums, err := parseSums(b)
	if err != nil {
		// e.g. a captive portal's page instead of the file
		return nil, fmt.Errorf("%w: %s/%s is not a checksum list (%v)", errUnavailable, src.base, releaseSums, err)
	}
	faFile, err := s.fetchAsset(src, sums, board+"-flasher_args.json")
	if err != nil {
		return nil, err
	}
	fb, err := os.ReadFile(faFile)
	if err != nil {
		return nil, err
	}
	fa, err := parseFlasherArgs(fb)
	if err != nil {
		return nil, err
	}
	files := map[string]string{}
	for _, name := range fa.FlashFiles {
		f, err := s.fetchAsset(src, sums, releaseAssetName(board, name))
		if err != nil {
			return nil, err
		}
		files[name] = f
	}
	p := func(name string) string { return files[name] }
	plan, err := planFirmware(fa, p, s.boardNVS)
	if err != nil {
		return nil, err
	}
	return &firmwareSet{source: src.base, plan: plan, path: p}, nil
}

// localBuild says how to build the firmware here, or why it cannot be.
type localBuild struct {
	repo   string // gogo source tree
	export string // ESP-IDF export.sh; "" if idf.py is already on PATH
}

// findLocalBuild looks for a gogo source tree (around the current
// directory or this executable) and ESP-IDF (idf.py on PATH, $IDF_PATH or
// ~/esp/esp-idf).
func (s *setup) findLocalBuild() (localBuild, error) {
	var lb localBuild
	if s.env.goos == "windows" {
		return lb, errors.New("building the firmware with gogo setup is not supported on Windows")
	}
	repo, ok := s.env.repoDir()
	if !ok {
		return lb, errors.New("no gogo source tree here (run gogo setup inside a clone of the gogo repository)")
	}
	lb.repo = repo
	if _, err := s.env.lookPath("idf.py"); err == nil {
		return lb, nil
	}
	var dirs []string
	if d := s.env.getenv("IDF_PATH"); d != "" {
		dirs = append(dirs, d)
	}
	if home := s.env.getenv("HOME"); home != "" {
		dirs = append(dirs, filepath.Join(home, "esp", "esp-idf"))
	}
	for _, d := range dirs {
		if _, err := os.Stat(filepath.Join(d, "export.sh")); err == nil {
			lb.export = filepath.Join(d, "export.sh")
			return lb, nil
		}
	}
	return lb, errors.New("ESP-IDF not found (no idf.py on PATH, no $IDF_PATH, no ~/esp/esp-idf)")
}

// localFirmware builds board's default firmware (every feature the board
// has) with `gogo firmware -build`, in <repo>/build/setup-<board>.
func (s *setup) localFirmware(board string, lb localBuild) (*firmwareSet, error) {
	tmp, err := os.MkdirTemp("", "gogo-setup-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmp)
	hello := filepath.Join(tmp, "hello"+imageExt)
	if err := writeHello(board, hello); err != nil {
		return nil, err
	}
	dir := filepath.Join(lb.repo, "build", "setup-"+board)
	var with []string
	for _, f := range fwFeatures {
		if fwBoards[board].has[f.name] {
			with = append(with, f.name)
		}
	}
	args := []string{"-board", board, "-o", dir, "-with", strings.Join(with, ","), hello}
	if !s.o.dryRun {
		args = append([]string{"-build"}, args...)
	}
	run := func(name string, idfArgs ...string) error { return s.env.runIdf(lb, idfArgs) }
	if err := firmwareCmd(args, s.env.out, run); err != nil {
		return nil, fmt.Errorf("local firmware build failed: %v", err)
	}
	p := func(name string) string { return filepath.Join(dir, name) }
	fs := &firmwareSet{source: "local build in " + dir, path: p}
	if s.o.dryRun {
		return fs, nil
	}
	fb, err := os.ReadFile(filepath.Join(dir, "flasher_args.json"))
	if err != nil {
		return nil, err
	}
	fa, err := parseFlasherArgs(fb)
	if err != nil {
		return nil, err
	}
	if fs.plan, err = planFirmware(fa, p, s.boardNVS); err != nil {
		return nil, err
	}
	return fs, nil
}

// runIdfLocal runs idf.py with args from the gogo source tree, sourcing
// ESP-IDF's export.sh first when idf.py is not on PATH.
func runIdfLocal(out io.Writer) func(lb localBuild, args []string) error {
	return func(lb localBuild, args []string) error {
		var cmd *exec.Cmd
		if lb.export == "" {
			cmd = exec.Command("idf.py", args...)
		} else {
			script := `. "$1" >/dev/null && shift && exec idf.py "$@"`
			cmd = exec.Command("sh", append([]string{"-c", script, "sh", lb.export}, args...)...)
		}
		cmd.Dir = lb.repo
		cmd.Stdout, cmd.Stderr = out, out
		return cmd.Run()
	}
}
