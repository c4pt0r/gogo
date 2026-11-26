// imudemo: opens the motion sensor and prints what it reports: every
// gesture by name, and for every sample batch its Chunk numbers, size and
// the first and last sample's values. Exits on the Step gesture. With no
// IMU it says so. On unix the events come from the stdin script (`imu
// sample gx gy gz ax ay az` and `imu gesture <kind> [value]` lines); on a
// board, from the sensor. The golden script keeps every timing margin at
// 100 ms or more: each group of sample lines is followed by a `sleep` at
// least 100 ms longer than BatchMs.
package main

import (
	"gogo/actor"
	"gogo/imu"
	"gogo/port"
	"gogo/strconv"
)

// s16 is the little-endian int16 at byte i of b.
func s16(b []byte, i int) int {
	v := int(b[i]) | int(b[i+1])<<8
	if v >= 32768 {
		v = v - 65536
	}
	return v
}

func gestureName(kind int) string {
	if kind == imu.Tap {
		return "tap"
	}
	if kind == imu.DoubleTap {
		return "doubletap"
	}
	if kind == imu.TripleTap {
		return "tripletap"
	}
	if kind == imu.Orientation {
		return "orientation"
	}
	if kind == imu.Flat {
		return "flat"
	}
	if kind == imu.AnyMotion {
		return "anymotion"
	}
	if kind == imu.NoMotion {
		return "nomotion"
	}
	if kind == imu.Step {
		return "step"
	}
	return "kind " + strconv.Itoa(kind)
}

func main() {
	_, ok := port.Open("imu", imu.Config{RateHz: 100, Gestures: imu.Tap | imu.DoubleTap | imu.Orientation | imu.Step, BatchMs: 200})
	if !ok {
		println("no imu")
		return
	}
	for {
		switch m := actor.Receive().(type) {
		case port.Chunk:
			n := m.Data.Len()
			b := make([]byte, n)
			m.Data.CopyTo(b)
			println("chunk", m.Seq, "dropped", m.Dropped, "bytes", n, "first gx", s16(b, 0), "last az", s16(b, n-2))
		case imu.Gesture:
			println("gesture", gestureName(m.Kind), m.Value)
			if m.Kind == imu.Step {
				return
			}
		}
	}
}
