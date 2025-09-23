package version

import "testing"

func TestGetVersion_NotEmpty(t *testing.T) {
	if v := GetVersion(); v == "" {
		t.Fatalf("version should not be empty")
	}
}
