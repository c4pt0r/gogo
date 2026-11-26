// Upload a recording with http.Upload: the file is streamed from storage
// 4 KB at a time, never held in memory whole. The URL and the token come
// from the config store, never from the source:
//
//	gogo config set upload.url https://myhost.example.ts.net/upload
//	gogo config set upload.token -     (typed, not echoed)
//
// On the host there is no network, so this shows the error cases.
package main

import (
	"gogo/bytes"
	"gogo/config"
	"gogo/fs"
	"gogo/http"
)

func main() {
	url, _ := config.Get("upload.url")
	token, _ := config.Get("upload.token") // sent as "Authorization: Bearer <token>"; never print it

	// A 10 KB file to send.
	b := bytes.NewBuffer(10240)
	for i := 0; i < 10240; i++ {
		b.WriteByte(byte(i))
	}
	println("write:", int(fs.WriteFile("/flash/rec.bin", b.Freeze())))

	status, err := http.Upload(url, "/flash/missing.bin", "audio/wav", token)
	println("missing file:", status, int(err), err == fs.NotFound)

	status, err = http.Upload("ftp://myhost/upload", "/flash/rec.bin", "audio/wav", token)
	println("bad URL:", status, int(err), err == http.BadURL)

	// No network on the host: Failed. On a board, the server's status
	// (201, 401, ...) with fs.OK.
	status, err = http.Upload(url, "/flash/rec.bin", "audio/wav", token)
	println("upload:", status, int(err), err == http.Failed)
}
