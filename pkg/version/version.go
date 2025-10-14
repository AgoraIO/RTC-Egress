package version

import (
	"fmt"
	"strings"
)

// Version metadata. Values are intended to be overridden at build time using
// -ldflags "-X github.com/AgoraIO/RTC-Egress/pkg/version.Version=v1.2.3".
var (
	Version   = "v1.2.39"
	GitCommit = "unknown"
	BuildTime = "unknown"
)

// GetVersion returns the semantic version string.
func GetVersion() string {
	return Version
}

// FullVersion returns a detailed version string including build metadata.
func FullVersion() string {
	hasCommit := GitCommit != "unknown"
	hasBuildTime := BuildTime != "unknown"
	hasBuildMetadata := strings.Contains(Version, "+")

	switch {
	case !hasCommit && !hasBuildTime:
		return Version
	case hasBuildMetadata:
		if hasBuildTime {
			return fmt.Sprintf("%s (%s)", Version, BuildTime)
		}
		return Version
	case hasCommit && hasBuildTime:
		return fmt.Sprintf("%s (%s, %s)", Version, GitCommit, BuildTime)
	case hasCommit:
		return fmt.Sprintf("%s (%s)", Version, GitCommit)
	case hasBuildTime:
		return fmt.Sprintf("%s (%s)", Version, BuildTime)
	default:
		return Version
	}
}
