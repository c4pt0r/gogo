//go:build !darwin && !linux

package main

import "errors"

// Windows and the rest: no USB reset (it would need SetupAPI); the hint
// says to replug the board.

func portReady(port string) bool { return true }

var errNoUSBReset = errors.New("gogo cannot reset a USB device on this OS: " + replugHint)

func usbResetPort(port string) (string, error) { return "", errNoUSBReset }
func usbProbe(port string) (bool, error)       { return false, errNoUSBReset }
func portHolders(port string) ([]int, error)   { return nil, nil }
