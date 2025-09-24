package utils

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// ResolveConfigFile resolves a config file path using the following precedence:
// 1) CONFIG_FILE environment variable (absolute file path)
// 2) --config flag in args (absolute file path)
// 3) CONFIG_DIR + <baseFileName>
// Fallback search order if none of the above resolve:
//
//	./config, /opt/rtc_egress/config, /etc/rtc_egress
//
// Returns the resolved absolute/relative path, or an error if no file found.
func ResolveConfigFile(baseFileName string, args []string) (string, error) {
	// 1) CONFIG_FILE
	if cf := strings.TrimSpace(os.Getenv("CONFIG_FILE")); cf != "" {
		if st, err := os.Stat(cf); err == nil && !st.IsDir() {
			return cf, nil
		}
	}

	// 2) --config flag
	for i, a := range args {
		if a == "--config" && i+1 < len(args) {
			cf := strings.TrimSpace(args[i+1])
			if cf != "" {
				if st, err := os.Stat(cf); err == nil && !st.IsDir() {
					return cf, nil
				}
			}
			break
		}
	}

	// 3) CONFIG_DIR + baseFileName
	if cd := strings.TrimSpace(os.Getenv("CONFIG_DIR")); cd != "" {
		candidate := filepath.Join(cd, baseFileName)
		if st, err := os.Stat(candidate); err == nil && !st.IsDir() {
			return candidate, nil
		}
	}

	// Fallback search
	for _, dir := range []string{"./config", "/opt/rtc_egress/config", "/etc/rtc_egress"} {
		candidate := filepath.Join(dir, baseFileName)
		if st, err := os.Stat(candidate); err == nil && !st.IsDir() {
			return candidate, nil
		}
	}

	return "", fmt.Errorf("config path not provided or not found: set --config, CONFIG_FILE, or mount CONFIG_DIR/%s", baseFileName)
}
