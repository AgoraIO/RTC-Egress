package logger

import (
	"bytes"
	"os"
	"strings"
	"testing"
)

func captureOutput(t *testing.T, fn func()) string {
	t.Helper()

	var buf bytes.Buffer
	SetOutput(&buf)
	defer SetOutput(os.Stdout)

	fn()
	return buf.String()
}

func TestInfoIncludesFields(t *testing.T) {
	out := captureOutput(t, func() {
		Init("egress", String("version", "v1.2.3"))
		Info("connected", String("redis_addr", "127.0.0.1:6379"))
	})

	if !strings.Contains(out, "][INFO]connected") {
		t.Fatalf("expected INFO level with message, got %q", out)
	}
	if !strings.Contains(out, "version=v1.2.3") {
		t.Fatalf("expected default field, got %q", out)
	}
	if !strings.Contains(out, "redis_addr=127.0.0.1:6379") {
		t.Fatalf("expected redis field, got %q", out)
	}
}

func TestQuotesAddedWhenNeeded(t *testing.T) {
	out := captureOutput(t, func() {
		Init("api")
		Info("redis connection established", String("status", "connection ok"))
	})

	if !strings.Contains(out, "][INFO]redis connection established") {
		t.Fatalf("expected message text, got %q", out)
	}
	if !strings.Contains(out, `status="connection ok"`) {
		t.Fatalf("expected quoted field, got %q", out)
	}
}

func TestWithAddsDefaultField(t *testing.T) {
	out := captureOutput(t, func() {
		Init("webhook")
		With(String("pod", "pod-123"))
		Info("notifier ready")
	})

	if !strings.Contains(out, "pod=pod-123") {
		t.Fatalf("expected pod field, got %q", out)
	}
}
