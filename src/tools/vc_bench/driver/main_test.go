package main

import (
	"strings"
	"testing"
)

func TestPayload(t *testing.T) {
	for _, width := range []int{1, 128, 1024, 16384} {
		a, b := payload(123, width), payload(124, width)
		if len(a) != width || a != payload(123, width) || a == b {
			t.Fatalf("invalid payload at width %d", width)
		}
		if strings.ContainsAny(a, "'\\\n\r") {
			t.Fatal("unsafe SQL literal")
		}
	}
}
