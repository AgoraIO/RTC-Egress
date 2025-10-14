package version

import "fmt"

// Version metadata. Values are intended to be overridden at build time using
// -ldflags "-X github.com/AgoraIO/RTC-Egress/pkg/version.Version=v1.2.3".
var (
	Version   = "v1.2.38"
	GitCommit = "unknown"
	BuildTime = "unknown"
)

// GetVersion returns the semantic version string.
func GetVersion() string {
	return Version
}

// FullVersion returns a detailed version string including build metadata.
func FullVersion() string {
	switch {
	case GitCommit == "unknown" && BuildTime == "unknown":
		return Version
	case GitCommit == "unknown":
		return fmt.Sprintf("%s (%s)", Version, BuildTime)
	case BuildTime == "unknown":
		return fmt.Sprintf("%s (%s)", Version, GitCommit)
	default:
		return fmt.Sprintf("%s (%s, %s)", Version, GitCommit, BuildTime)
	}
}
