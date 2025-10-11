package egress

import (
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func TestDeriveCompletionResultSuccessDefaultMessage(t *testing.T) {
	completion := &UDSCompletionMessage{
		TaskID:  "task-123",
		Status:  "success",
		Message: "",
	}

	status, message := deriveCompletionResult(completion)

	if status != queue.TaskStateStopped {
		t.Fatalf("expected status %q, got %q", queue.TaskStateStopped, status)
	}
	if message != "Task completed successfully" {
		t.Fatalf("expected default success message, got %q", message)
	}
}

func TestDeriveCompletionResultSuccessUsesProvidedMessage(t *testing.T) {
	completion := &UDSCompletionMessage{
		TaskID:  "task-456",
		Status:  "success",
		Message: "Export finished",
	}

	status, message := deriveCompletionResult(completion)

	if status != queue.TaskStateStopped {
		t.Fatalf("expected status %q, got %q", queue.TaskStateStopped, status)
	}
	if message != "Export finished" {
		t.Fatalf("expected message to be preserved, got %q", message)
	}
}

func TestDeriveCompletionResultFailureUsesErrorFallback(t *testing.T) {
	completion := &UDSCompletionMessage{
		TaskID: "task-789",
		Status: "failed",
		Error:  "worker crashed",
	}

	status, message := deriveCompletionResult(completion)

	if status != queue.TaskStateFailed {
		t.Fatalf("expected status %q, got %q", queue.TaskStateFailed, status)
	}
	if message != "worker crashed" {
		t.Fatalf("expected error message to be used, got %q", message)
	}
}
