package utils

import (
	"fmt"
	"os"
	"strings"
)

// IsRunningInContainer detects if the application is running inside a container
func IsRunningInContainer() bool {
	// Check for common container indicators
	// 1. Check if we're running as PID 1 (common in containers)
	if os.Getpid() == 1 {
		return true
	}

	// 2. Check for Docker-specific environment variables
	if os.Getenv("DOCKER_CONTAINER") != "" {
		return true
	}

	// 3. Check if /.dockerenv file exists (Docker containers)
	if _, err := os.Stat("/.dockerenv"); err == nil {
		return true
	}

	// 4. Check for Kubernetes environment
	if os.Getenv("KUBERNETES_SERVICE_HOST") != "" {
		return true
	}

	return false
}

// ResolveRedisAddr prefers an explicit addr, otherwise combines REDIS_HOST/REDIS_PORT.
func ResolveRedisAddr(current string) string {
	current = strings.TrimSpace(current)
	if current != "" {
		return current
	}

	if addr := strings.TrimSpace(os.Getenv("REDIS_ADDR")); addr != "" {
		return addr
	}

	host := strings.TrimSpace(os.Getenv("REDIS_HOST"))
	if host == "" {
		return ""
	}

	port := strings.TrimSpace(os.Getenv("REDIS_PORT"))
	if port == "" {
		return ""
	}

	return fmt.Sprintf("%s:%s", host, port)
}
