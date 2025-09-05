package version

import (
	"os"
	"strings"
)

// Version represents the current application version
var Version = "v1.1.0"

// GetVersion returns the version string
// It first checks for a VERSION file, then falls back to the hardcoded version
func GetVersion() string {
	// Try to read from VERSION file first
	if data, err := os.ReadFile("VERSION"); err == nil {
		return strings.TrimSpace(string(data))
	}

	// Fallback to hardcoded version
	return Version
}
