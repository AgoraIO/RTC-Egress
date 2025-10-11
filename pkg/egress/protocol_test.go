package egress

import (
	"strings"
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func TestBuildUDSMessageFromQueueTaskStart(t *testing.T) {
	task := &queue.Task{
		ID:        "start-1",
		Cmd:       "record",
		Action:    "start",
		Channel:   "demo",
		RequestID: "req-1",
		Payload: map[string]interface{}{
			"layout":         "flat",
			"channel":        "demo",
			"access_token":   "token-value-12345",
			"uid":            []string{"user1", "user2"},
			"workerUid":      float64(101),
			"interval_in_ms": float64(15000),
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.TaskID != "start-1" {
		t.Fatalf("expected task id %q, got %q", "start-1", msg.TaskID)
	}
	if msg.Channel != "demo" {
		t.Fatalf("expected channel %q, got %q", "demo", msg.Channel)
	}
	if msg.WorkerUid != 101 {
		t.Fatalf("expected worker UID 101, got %d", msg.WorkerUid)
	}
	if msg.IntervalInMs != 15000 {
		t.Fatalf("expected interval 15000, got %d", msg.IntervalInMs)
	}
	if msg.Action != "start" {
		t.Fatalf("expected action start, got %s", msg.Action)
	}
	if msg.Cmd != "record" {
		t.Fatalf("expected cmd record, got %s", msg.Cmd)
	}
	if msg.Layout != "flat" {
		t.Fatalf("expected layout flat, got %s", msg.Layout)
	}
	if msg.AccessToken != "token-value-12345" {
		t.Fatalf("expected access token token-value-12345, got %s", msg.AccessToken)
	}
	if len(msg.Uid) != 2 || msg.Uid[0] != "user1" || msg.Uid[1] != "user2" {
		t.Fatalf("expected uid slice [user1 user2], got %#v", msg.Uid)
	}
}

func TestBuildUDSMessageFromQueueTaskStopUsesOriginalID(t *testing.T) {
	task := &queue.Task{
		ID:        "stop-1",
		Cmd:       "record",
		Action:    "stop",
		Channel:   "demo",
		RequestID: "req-stop",
		Payload: map[string]interface{}{
			"task_id": "orig-1",
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.TaskID != "orig-1" {
		t.Fatalf("expected task id %q, got %q", "orig-1", msg.TaskID)
	}
	if msg.Channel != "demo" {
		t.Fatalf("expected channel %q, got %q", "demo", msg.Channel)
	}
	if msg.Action != "stop" {
		t.Fatalf("expected action stop, got %s", msg.Action)
	}
	if msg.IntervalInMs != 0 {
		t.Fatalf("expected interval 0, got %d", msg.IntervalInMs)
	}
}

func TestBuildUDSMessageFromQueueTaskStopNilPayload(t *testing.T) {
	task := &queue.Task{
		ID:      "stop-2",
		Cmd:     "snapshot",
		Action:  "stop",
		Channel: "demo",
		Payload: nil,
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.TaskID != "stop-2" {
		t.Fatalf("expected task id %q, got %q", "stop-2", msg.TaskID)
	}
	if msg.Channel != "demo" {
		t.Fatalf("expected channel %q, got %q", "demo", msg.Channel)
	}
}

func TestBuildUDSMessageFromQueueTaskStartMissingAccessToken(t *testing.T) {
	task := &queue.Task{
		ID:      "start-missing-token",
		Cmd:     "record",
		Action:  "start",
		Channel: "demo",
		Payload: map[string]interface{}{
			"workerUid": float64(10),
		},
	}

	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for missing access_token, got nil")
	}
	if !strings.Contains(err.Error(), "access_token") {
		t.Fatalf("expected error to mention access_token, got %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskStartChannelFallback(t *testing.T) {
	task := &queue.Task{
		ID:      "start-channel-fallback",
		Cmd:     "snapshot",
		Action:  "start",
		Channel: "channel-from-task",
		Payload: map[string]interface{}{
			"access_token": "token-123456",
			"workerUid":    float64(77),
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.Channel != "channel-from-task" {
		t.Fatalf("expected channel to fallback to task.Channel, got %q", msg.Channel)
	}
	if msg.WorkerUid != 77 {
		t.Fatalf("expected worker UID 77, got %d", msg.WorkerUid)
	}
}

func TestBuildUDSMessageFromQueueTaskUIDConversion(t *testing.T) {
	task := &queue.Task{
		ID:      "start-with-uids",
		Cmd:     "record",
		Action:  "start",
		Channel: "demo",
		Payload: map[string]interface{}{
			"access_token": "token-123456",
			"workerUid":    float64(42),
			"uid":          []interface{}{"user1", "user2"},
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if len(msg.Uid) != 2 || msg.Uid[0] != "user1" || msg.Uid[1] != "user2" {
		t.Fatalf("expected uid slice [user1 user2], got %#v", msg.Uid)
	}
}

func TestBuildUDSMessageFromQueueTaskNilTask(t *testing.T) {
	_, err := buildUDSMessageFromQueueTask(nil)
	if err == nil {
		t.Fatal("expected error when task is nil")
	}
	if !strings.Contains(err.Error(), "task cannot be nil") {
		t.Fatalf("expected nil task error, got %v", err)
	}
}
