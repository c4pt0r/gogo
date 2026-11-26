// On-device check of the LCD debug console (M5StickS3): mixed Chinese and
// English lines in white, one actor that crashes on purpose (the platform
// prints its crash report on stderr, so it shows in red), then a line
// showing the VM carried on. With the boot and exit lines it fills the 11
// console rows exactly, so nothing scrolls off. The final ✓ is not in
// GB2312, so it shows as a box glyph.
//
//	gogo build platform/esp32s3/examples/console -o console.gogo && gogo flash console.gogo
package main

import "gogo/actor"

func main() {
	println("你好，gogo! Hello from M5StickS3")
	println("中英文混排: actor 模型 on ESP32-S3")
	println("一行很长的文字会自动折行到下一行显示, long lines wrap.")
	zero := 0
	p := actor.Spawn(func() {
		println("worker 启动, 准备除以零")
		println(1 / zero) // crashes this actor only
	})
	actor.Monitor(p)
	actor.Receive() // the DOWN message
	println("崩溃之后 main 继续运行 ✓")
}
