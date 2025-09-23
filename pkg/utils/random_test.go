package utils

import "testing"

func TestGenerateRandomID_LengthAndCharset(t *testing.T) {
	id := GenerateRandomID(16)
	if len(id) != 16 {
		t.Fatalf("expected length 16, got %d", len(id))
	}
	const charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	for i, c := range id {
		if !containsRune(charset, c) {
			t.Fatalf("character %q at %d not in allowed charset", c, i)
		}
	}
}

func containsRune(s string, r rune) bool {
	for _, c := range s {
		if c == r {
			return true
		}
	}
	return false
}
