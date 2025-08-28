package utils

import "strings"

// SplitString splits a string by a delimiter and returns the parts
func SplitString(s, delimiter string) []string {
	return strings.Split(s, delimiter)
}

// HasPrefix checks if a string has a specific prefix
func HasPrefix(s, prefix string) bool {
	return strings.HasPrefix(s, prefix)
}

// TrimSpace removes leading and trailing whitespace from a string
func TrimSpace(s string) string {
	return strings.TrimSpace(s)
}
