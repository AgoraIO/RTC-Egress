package utils

import "testing"

func TestSplitString(t *testing.T) {
	parts := SplitString("a,b,c", ",")
	if len(parts) != 3 || parts[0] != "a" || parts[1] != "b" || parts[2] != "c" {
		t.Fatalf("unexpected split result: %#v", parts)
	}
}

func TestHasPrefix(t *testing.T) {
	if !HasPrefix("foobar", "foo") {
		t.Fatalf("expected prefix to match")
	}
	if HasPrefix("barfoo", "foo") {
		t.Fatalf("did not expect prefix to match")
	}
}

func TestTrimSpace(t *testing.T) {
	got := TrimSpace("  hello \t\n")
	if got != "hello" {
		t.Fatalf("expected 'hello', got %q", got)
	}
}
