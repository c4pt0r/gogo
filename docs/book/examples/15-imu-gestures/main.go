// Gestures only (RateHz 0: no samples): a double tap starts and stops a
// "recording", a change of orientation is reported, and the program ends
// after the step counter passes 10. On the host the gestures come from the
// "imu gesture <kind> [value]" lines of the stdin script.
package main

import (
	"gogo/actor"
	"gogo/imu"
	"gogo/port"
)

// side names the orientation number's low two bits.
func side(v int) string {
	if v == 0 {
		return "upright"
	}
	if v == 1 {
		return "on its left side"
	}
	if v == 2 {
		return "upside down"
	}
	return "on its right side"
}

func main() {
	_, ok := port.Open("imu", imu.Config{Gestures: imu.DoubleTap | imu.Orientation | imu.Step})
	if !ok {
		println("no motion sensor on this board")
		return
	}
	recording := false
	for {
		switch g := actor.Receive().(type) {
		case imu.Gesture:
			if g.Kind == imu.DoubleTap {
				recording = !recording
				if recording {
					println("double tap: recording")
				} else {
					println("double tap: stopped")
				}
			} else if g.Kind == imu.Orientation {
				face := "face up"
				if g.Value&4 != 0 {
					face = "face down"
				}
				println("orientation:", side(g.Value&3)+",", face)
			} else if g.Kind == imu.Step {
				println("steps:", g.Value)
				if g.Value > 10 {
					return
				}
			}
		}
	}
}
