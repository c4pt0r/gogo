//go:build !darwin && !linux

package main

import (
	"errors"
	"os"
)

func openSerial(port string) (*os.File, error) {
	return nil, errors.New("gogo monitor: not supported on this OS")
}

func resetChip(f *os.File) error { return errors.New("gogo monitor: not supported on this OS") }
