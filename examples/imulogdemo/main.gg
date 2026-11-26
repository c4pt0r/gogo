// imulogdemo: records the motion sensor to /flash/imu with gogo/imulog and
// reads the log back. On unix the events come from the stdin script (`imu
// sample ...` and `imu gesture ...` lines; run with --fs-root). The log is capped at 80 bytes, so the oldest
// records rotate away. The golden
// script keeps every timing margin at 100 ms or more: each group of sample
// lines is followed by a `sleep` at least 100 ms longer than BatchMs.
package main

import (
	"gogo/actor"
	"gogo/imu"
	"gogo/imulog"
)

func main() {
	l, ok := imulog.Start(imu.Config{RateHz: 100, Gestures: imu.Tap | imu.Step, BatchMs: 200}, "/flash/imu", 80)
	if !ok {
		println("no imu")
		return
	}
	actor.Sleep(1500) // the script feeds the sensor meanwhile
	actor.Kill(l, actor.Shutdown)
	for _, r := range imulog.Read("/flash/imu", 10) {
		if r.Kind == imulog.ChunkRec {
			println("chunk", r.A, "dropped", r.B, "bytes", r.Data.Len(), "first byte", int(r.Data.At(0)))
		} else {
			println("gesture", r.A, "value", r.B)
		}
	}
	println("last 2:", len(imulog.Read("/flash/imu", 2)))
}
