package main

// gogo setup, the flash side: which board is on the port, what the
// firmware's flasher_args.json asks to write, and the checks that keep
// every write away from the NVS partition (where the Wi-Fi settings live).
//
// The rules, enforced here and nowhere else:
//   - the chip is never erased as a whole (no erase-flash, erase-region,
//     --erase-all), and no merged image is ever written (its gap fill
//     would overwrite NVS);
//   - every firmware write lies in the bootloader area (below the
//     partition table), on the partition table sector, or inside an app
//     partition of the partition table being written;
//   - no write, rounded out to whole 4 KB flash sectors (esptool erases
//     whole sectors before writing), overlaps an NVS partition.

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const flashSector = 0x1000

// Partition types and subtypes (ESP-IDF partition table format).
const (
	partTypeApp    = 0x00
	partTypeData   = 0x01
	partSubNVS     = 0x02
	partSubImage   = 0x40 // the `gogo` image partition (partitions.csv)
	partTableBytes = 0xC00
)

// partition is one entry of a binary ESP-IDF partition table.
type partition struct {
	label        string
	typ, subtype uint8
	offset, size uint32
}

func (p partition) end() uint32 { return p.offset + p.size }

// parsePartitions decodes a binary partition table (32-byte entries
// starting 0xAA 0x50, then an optional MD5 entry starting 0xEB 0xEB, then
// 0xFF padding).
func parsePartitions(b []byte) ([]partition, error) {
	var parts []partition
	for i := 0; i+32 <= len(b); i += 32 {
		e := b[i : i+32]
		switch {
		case e[0] == 0xAA && e[1] == 0x50:
			label := string(e[12:28])
			if j := strings.IndexByte(label, 0); j >= 0 {
				label = label[:j]
			}
			parts = append(parts, partition{
				label:   label,
				typ:     e[2],
				subtype: e[3],
				offset:  binary.LittleEndian.Uint32(e[4:]),
				size:    binary.LittleEndian.Uint32(e[8:]),
			})
		case e[0] == 0xEB && e[1] == 0xEB, e[0] == 0xFF && e[1] == 0xFF:
			if len(parts) == 0 {
				return nil, errors.New("empty partition table")
			}
			return parts, nil
		default:
			return nil, fmt.Errorf("not a partition table (bad entry at byte %d)", i)
		}
	}
	if len(parts) == 0 {
		return nil, errors.New("empty partition table")
	}
	return parts, nil
}

// nvsParts returns the NVS partitions of a table.
func nvsParts(parts []partition) []partition {
	var out []partition
	for _, p := range parts {
		if p.typ == partTypeData && p.subtype == partSubNVS {
			out = append(out, p)
		}
	}
	return out
}

// defaultNVS is the NVS partition of platform/esp32s3/partitions.csv, used
// when no partition table has been read yet.
var defaultNVS = []partition{{label: "nvs", typ: partTypeData, subtype: partSubNVS, offset: 0x9000, size: 0x6000}}

// flasherArgs is the part of ESP-IDF's build/flasher_args.json that gogo
// setup uses.
type flasherArgs struct {
	FlashSettings struct {
		Mode string `json:"flash_mode"`
		Size string `json:"flash_size"`
		Freq string `json:"flash_freq"`
	} `json:"flash_settings"`
	FlashFiles     map[string]string `json:"flash_files"`
	PartitionTable struct {
		Offset string `json:"offset"`
		File   string `json:"file"`
	} `json:"partition-table"`
	Extra struct {
		Chip string `json:"chip"`
	} `json:"extra_esptool_args"`
}

func parseFlasherArgs(b []byte) (*flasherArgs, error) {
	var fa flasherArgs
	if err := json.Unmarshal(b, &fa); err != nil {
		return nil, fmt.Errorf("flasher_args.json: %v", err)
	}
	if fa.Extra.Chip != "esp32s3" {
		return nil, fmt.Errorf("flasher_args.json: firmware for chip %q, not esp32s3", fa.Extra.Chip)
	}
	if len(fa.FlashFiles) == 0 || fa.PartitionTable.File == "" {
		return nil, errors.New("flasher_args.json: no flash files or no partition table")
	}
	word := regexp.MustCompile(`^[A-Za-z0-9]+$`)
	for _, v := range []string{fa.FlashSettings.Mode, fa.FlashSettings.Size, fa.FlashSettings.Freq} {
		if !word.MatchString(v) {
			return nil, fmt.Errorf("flasher_args.json: bad flash setting %q", v)
		}
	}
	return &fa, nil
}

func parseOffset(s string) (uint32, error) {
	v, err := strconv.ParseUint(s, 0, 32)
	if err != nil {
		return 0, fmt.Errorf("bad flash offset %q", s)
	}
	return uint32(v), nil
}

// flashWrite is one file esptool writes.
type flashWrite struct {
	offset uint32
	name   string // as named in flasher_args.json
	path   string // on this computer
	size   uint32
}

// firmwarePlan is what flashing one firmware writes, checked.
type firmwarePlan struct {
	args   *flasherArgs
	writes []flashWrite // sorted by offset
	parts  []partition  // the partition table being written
}

// planFirmware turns flasher_args.json into a checked write list. path
// maps a file named in flasher_args.json to the file on this computer.
func planFirmware(fa *flasherArgs, path func(string) string) (*firmwarePlan, error) {
	ptOff, err := parseOffset(fa.PartitionTable.Offset)
	if err != nil {
		return nil, err
	}
	var writes []flashWrite
	ptListed := false
	for off, name := range fa.FlashFiles {
		o, err := parseOffset(off)
		if err != nil {
			return nil, err
		}
		fi, err := os.Stat(path(name))
		if err != nil {
			return nil, err
		}
		if fi.Size() == 0 || fi.Size() > 16<<20 {
			return nil, fmt.Errorf("%s: unexpected size %d", name, fi.Size())
		}
		if o == ptOff && name == fa.PartitionTable.File {
			ptListed = true
		}
		writes = append(writes, flashWrite{offset: o, name: name, path: path(name), size: uint32(fi.Size())})
	}
	if !ptListed {
		return nil, errors.New("flasher_args.json: the partition table is not among the flash files")
	}
	sort.Slice(writes, func(i, j int) bool { return writes[i].offset < writes[j].offset })
	pt, err := os.ReadFile(path(fa.PartitionTable.File))
	if err != nil {
		return nil, err
	}
	parts, err := parsePartitions(pt)
	if err != nil {
		return nil, fmt.Errorf("%s: %v", fa.PartitionTable.File, err)
	}
	if err := checkPartitions(parts); err != nil {
		return nil, err
	}
	if err := checkWrites(writes, parts, ptOff); err != nil {
		return nil, err
	}
	return &firmwarePlan{args: fa, writes: writes, parts: parts}, nil
}

// checkPartitions checks the table has the NVS and image partitions gogo
// expects (the image partition's place is a constant of `gogo flash`).
func checkPartitions(parts []partition) error {
	if len(nvsParts(parts)) == 0 {
		return errors.New("partition table has no NVS partition")
	}
	off, _ := parseOffset(partOffset)
	for _, p := range parts {
		if p.typ == partTypeData && p.subtype == partSubImage {
			if p.offset != off || p.size != partSize {
				return fmt.Errorf("image partition at %#x size %#x, gogo writes images at %s size %#x", p.offset, p.size, partOffset, partSize)
			}
			return nil
		}
	}
	return errors.New("partition table has no gogo image partition")
}

// sectorSpan is the range of whole flash sectors that writing size bytes
// at offset erases.
func sectorSpan(offset, size uint32) (uint32, uint32) {
	start := offset &^ (flashSector - 1)
	end := (uint64(offset) + uint64(size) + flashSector - 1) &^ (flashSector - 1)
	if end > 1<<32-1 {
		end = 1<<32 - flashSector
	}
	return start, uint32(end)
}

func overlaps(aStart, aEnd, bStart, bEnd uint32) bool { return aStart < bEnd && bStart < aEnd }

// checkWrites enforces the rules at the top of this file for a firmware
// write list against the partition table being written (ptOff: its
// offset).
func checkWrites(writes []flashWrite, parts []partition, ptOff uint32) error {
	nvs := nvsParts(parts)
	if len(nvs) == 0 {
		return errors.New("no NVS partition in the partition table: refusing to flash")
	}
	for i, w := range writes {
		start, end := sectorSpan(w.offset, w.size)
		for _, p := range nvs {
			if overlaps(start, end, p.offset, p.end()) {
				return fmt.Errorf("refusing to flash: %s at %#x..%#x would overwrite the NVS partition %q (%#x..%#x)",
					w.name, start, end, p.label, p.offset, p.end())
			}
		}
		for _, v := range writes[:i] {
			vs, ve := sectorSpan(v.offset, v.size)
			if overlaps(start, end, vs, ve) {
				return fmt.Errorf("refusing to flash: %s and %s overlap", v.name, w.name)
			}
		}
		ok := end <= ptOff || (w.offset == ptOff && w.size <= partTableBytes)
		for _, p := range parts {
			if p.typ == partTypeApp && start >= p.offset && end <= p.end() {
				ok = true
			}
		}
		if !ok {
			return fmt.Errorf("refusing to flash: %s at %#x is not in the bootloader area, the partition table or an app partition", w.name, w.offset)
		}
	}
	return nil
}

// checkEsptoolArgs is the last check before esptool runs: never an erase
// or a merged image, and every write-flash file clear of the NVS
// partitions nvs.
func checkEsptoolArgs(args []string, nvs []partition) error {
	if len(nvs) == 0 {
		return errors.New("refusing to run esptool: NVS partition unknown")
	}
	cmd := -1
	for i, a := range args {
		switch strings.ReplaceAll(a, "_", "-") {
		case "erase-flash", "erase-region", "--erase-all", "-e", "merge-bin", "write-mem", "write-efuse", "burn-efuse":
			return fmt.Errorf("refusing to run esptool %s", a)
		case "write-flash":
			if cmd < 0 {
				cmd = i
			}
		}
	}
	if cmd < 0 {
		return nil
	}
	rest := args[cmd+1:]
	for i := 0; i < len(rest); i++ {
		a := rest[i]
		if strings.HasPrefix(a, "-") {
			switch a {
			case "--flash-mode", "--flash-freq", "--flash-size", "-fm", "-ff", "-fs":
				i++
			case "--compress", "-z", "--no-progress":
			default:
				return fmt.Errorf("refusing to run esptool: unexpected write-flash option %s", a)
			}
			continue
		}
		if i+1 >= len(rest) {
			return fmt.Errorf("refusing to run esptool: offset %s without a file", a)
		}
		off, err := parseOffset(a)
		if err != nil {
			return fmt.Errorf("refusing to run esptool: %v", err)
		}
		size := uint32(flashSector)
		if fi, err := os.Stat(rest[i+1]); err == nil {
			size = uint32(fi.Size())
		}
		start, end := sectorSpan(off, size)
		for _, p := range nvs {
			if overlaps(start, end, p.offset, p.end()) {
				return fmt.Errorf("refusing to run esptool: the write at %#x would overwrite the NVS partition", off)
			}
		}
		i++
	}
	return nil
}

// chipInfo is what esptool reports about the chip.
type chipInfo struct {
	chip  string // e.g. "ESP32-S3-PICO-1 (LGA56) (revision v0.2)"
	psram bool
}

var (
	chipTypeRe   = regexp.MustCompile(`(?m)^Chip type:\s*(.+?)\s*$`)
	chipIsRe     = regexp.MustCompile(`(?m)^Chip is (.+?)\s*$`)
	chipDetectRe = regexp.MustCompile(`Detecting chip type\.*\s*(\S+)`)
	featuresRe   = regexp.MustCompile(`(?m)^Features:\s*(.+?)\s*$`)
)

// parseChip reads esptool's connection banner (printed by every command
// that talks to the chip, in esptool v4 and v5 wording).
func parseChip(out string) (chipInfo, error) {
	var c chipInfo
	for _, re := range []*regexp.Regexp{chipTypeRe, chipIsRe, chipDetectRe} {
		if m := re.FindStringSubmatch(out); m != nil {
			c.chip = m[1]
			break
		}
	}
	if c.chip == "" {
		return c, errors.New("esptool did not report the chip type")
	}
	if m := featuresRe.FindStringSubmatch(out); m != nil {
		c.psram = strings.Contains(m[1], "PSRAM")
	}
	return c, nil
}

// isS3 reports whether the chip is an ESP32-S3.
func (c chipInfo) isS3() bool { return strings.HasPrefix(strings.ToUpper(c.chip), "ESP32-S3") }

// guessBoard: of the two supported boards, the StickS3 (ESP32-S3-PICO-1-N8R8)
// has embedded PSRAM and the Cardputer ADV (ESP32-S3FN8) has none.
func (c chipInfo) guessBoard() string {
	if c.psram {
		return "sticks3"
	}
	return "cardputer"
}
