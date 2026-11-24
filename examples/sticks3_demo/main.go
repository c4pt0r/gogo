// sticks3_demo: M5StickS3 上的"崩溃后被监督者重启"演示。
//
//   KEY1 (G11)  让 worker 崩溃（除零），监督者把它重启
//   KEY2 (G12)  打印重启次数和 worker 的内存占用
//   5 秒内崩溃超过 3 次，监督者放弃，演示停止
//
// 所有 println 同时出现在 USB 串口和屏幕上；崩溃报告（stderr）在屏幕上是红色。
package main

import (
	"gogo/actor"
	"gogo/port"
	"gogo/supervisor"
)

const (
	key1 = 11
	key2 = 12
)

// Hello 是每个新 worker 实例发给 main 的第一条消息。
type Hello struct{ From actor.PID }

// Welcome 告诉 worker 它是第几个实例。
type Welcome struct{ N int }

func worker(boss actor.PID) {
	actor.Send(boss, Hello{actor.Self()})
	n := 0
	switch m := actor.Receive().(type) {
	case Welcome:
		n = m.N
	}

	// 上一个实例退出时，它打开的端口已被自动关闭，这里可以重新打开。
	_, ok := port.Open("gpio", port.GPIO{
		Pin:  key1,
		Mode: port.InputPullUp,
		Edge: port.Falling,
	})
	if !ok {
		println("worker: 打不开 KEY1")
		return
	}

	beat := 0
	for {
		msg, ok := actor.ReceiveTimeout(1000)
		if !ok { // 1 秒内没有消息：心跳
			beat++
			println("worker #", n, "心跳", beat)
			continue
		}
		switch msg.(type) {
		case port.Edge: // KEY1 按下
			println("worker #", n, "要崩溃了!")
			zero := 0
			println(1 / zero)
		}
	}
}

func main() {
	me := actor.Self()
	println("gogo demo: KEY1 崩溃, KEY2 状态")
	// Supervise 会把 main 和监督者链接起来；捕获退出信号，
	// 监督者放弃时 main 收到 ExitMsg 而不是跟着死掉。
	actor.TrapExit(true)

	sup := supervisor.Supervise(func() { worker(me) }, supervisor.Spec{
		Restart:     supervisor.Permanent,
		MaxRestarts: 3,
		PeriodMs:    5000,
	})
	_, ok := port.Open("gpio", port.GPIO{
		Pin:  key2,
		Mode: port.InputPullUp,
		Edge: port.Falling,
	})
	if !ok {
		println("main: 打不开 KEY2")
	}

	instances := 0
	var cur actor.PID
	for {
		switch m := actor.Receive().(type) {
		case Hello:
			instances++
			cur = m.From
			actor.Send(cur, Welcome{instances})
			if instances > 1 {
				println("已重启, 第", instances, "个实例")
			}
		case port.Edge: // KEY2 按下
			println("重启次数:", instances-1, "worker 内存:", actor.Info(cur).Memory, "B")
		case actor.ExitMsg:
			if m.From == sup {
				println("重启太频繁, 监督者放弃了。演示停止。")
				return
			}
		}
	}
}
