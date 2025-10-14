package version

import "testing"

func TestGetVersion_NotEmpty(t *testing.T) {
	if v := GetVersion(); v == "" {
		t.Fatalf("version should not be empty")
	}
}

func TestFullVersionIncludesMetadata(t *testing.T) {
	origVersion, origCommit, origTime := Version, GitCommit, BuildTime
	defer func() {
		Version, GitCommit, BuildTime = origVersion, origCommit, origTime
	}()

	Version = "v1.2.3-dev"
	GitCommit = "abc123"
	BuildTime = "2025-10-12T08:00:00Z"

	want := "v1.2.3-dev (abc123, 2025-10-12T08:00:00Z)"
	if got := FullVersion(); got != want {
		t.Fatalf("expected %q, got %q", want, got)
	}
}

func TestFullVersionFallbacks(t *testing.T) {
	origVersion, origCommit, origTime := Version, GitCommit, BuildTime
	defer func() {
		Version, GitCommit, BuildTime = origVersion, origCommit, origTime
	}()

	Version = "v1.2.3-dev"
	GitCommit = "unknown"
	BuildTime = "2025-10-12T08:00:00Z"
	if got := FullVersion(); got != "v1.2.3-dev (2025-10-12T08:00:00Z)" {
		t.Fatalf("unexpected full version with unknown commit: %s", got)
	}

	GitCommit = "abc123"
	BuildTime = "unknown"
	if got := FullVersion(); got != "v1.2.3-dev (abc123)" {
		t.Fatalf("unexpected full version with unknown build time: %s", got)
	}

	GitCommit = "unknown"
	BuildTime = "unknown"
	if got := FullVersion(); got != "v1.2.3-dev" {
		t.Fatalf("unexpected fallback full version: %s", got)
	}
}
