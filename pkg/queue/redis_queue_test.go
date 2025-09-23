package queue

import "testing"

func TestBuildKeysAndRegion(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "us-west")

	if rq.GetRegion() != "us-west" {
		t.Fatalf("expected region 'us-west', got %q", rq.GetRegion())
	}

	if got := rq.BuildQueueKey("snapshot", "channel1"); got != "egress:snapshot:channel1" {
		t.Fatalf("unexpected queue key: %q", got)
	}

	if got := rq.BuildTaskKey("abc123"); got != "egress:task:abc123" {
		t.Fatalf("unexpected task key: %q", got)
	}

	if got := rq.BuildProcessingQueueKey("w1"); got != "egress:processing:w1" {
		t.Fatalf("unexpected processing queue key: %q", got)
	}

	// Regional queue key behavior
	if got := rq.BuildRegionalQueueKey("", "record", "chan"); got != "egress:record:chan" {
		t.Fatalf("unexpected regional key (global): %q", got)
	}
	if got := rq.BuildRegionalQueueKey("eu", "record", "chan"); got != "egress:eu:record:chan" {
		t.Fatalf("unexpected regional key (eu): %q", got)
	}
}
