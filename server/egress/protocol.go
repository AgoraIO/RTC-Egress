package egress

import (
	"fmt"
	"regexp"
	"strings"

	"github.com/agora-build/rtc-egress/server/queue"
)

// UDSMessage defines the communication protocol between Go (egress) and C++ (eg_worker).
// All fields are serialized in JSON format.
type UDSMessage struct {
	TaskID             string   `json:"task_id"`            // Task ID for tracking completion
	Cmd                string   `json:"cmd"`                // "snapshot", "record", "rtmp", or "whip"
	Action             string   `json:"action"`             // "start", "stop", "status"
	Layout             string   `json:"layout"`             // "grid", "flat", "spotlight", or "freestyle"
	FreestyleCanvasUrl string   `json:"freestyleCanvasUrl"` // URL for custom canvas, used if layout is "freestyle"
	Uid                []string `json:"uid"`                // User IDs, if empty, all users will be included
	Channel            string   `json:"channel"`            // Channel Name
	AccessToken        string   `json:"access_token"`       // Access token for authentication
	WorkerUid          int      `json:"workerUid"`          // Worker UID
	IntervalInMs       int      `json:"interval_in_ms"`     // Interval in milliseconds
}

// UDSCompletionMessage defines the completion response from C++ worker to Go manager
type UDSCompletionMessage struct {
	TaskID  string `json:"task_id"` // Task ID that completed
	Status  string `json:"status"`  // "success" or "failed"
	Error   string `json:"error"`   // Error message if status is "failed"
	Message string `json:"message"` // Additional completion message
}

// FlattenPayloadToUDSMessage converts a WorkerCommand with nested payload to a flattened UDSMessage
// that matches the C++ UDSMessage structure exactly
func FlattenPayloadToUDSMessage(cmd string, action string, payload map[string]interface{}) (*UDSMessage, error) {
	udsMsg := &UDSMessage{
		Cmd:    cmd,
		Action: action,
		Layout: "flat", // default
		Uid:    []string{},
	}

	// Extract and validate each field from payload
	if payload == nil {
		return nil, fmt.Errorf("payload cannot be nil")
	}

	// Layout (optional, defaults to "flat")
	if layoutVal, ok := payload["layout"]; ok {
		if layoutStr, isStr := layoutVal.(string); isStr && layoutStr != "" {
			udsMsg.Layout = layoutStr
		}
	}

	// FreestyleCanvasUrl (optional)
	if canvasVal, ok := payload["freestyleCanvasUrl"]; ok {
		if canvasStr, isStr := canvasVal.(string); isStr {
			udsMsg.FreestyleCanvasUrl = strings.TrimSpace(canvasStr)
		}
	}

	// Uid (optional)
	if uidVal, ok := payload["uid"]; ok && uidVal != nil {
		switch v := uidVal.(type) {
		case []string:
			udsMsg.Uid = v
		case []interface{}:
			// Convert []interface{} to []string
			for _, item := range v {
				if str, isStr := item.(string); isStr {
					udsMsg.Uid = append(udsMsg.Uid, str)
				}
			}
		}
	}

	// Channel (required only for start actions)
	if channelVal, ok := payload["channel"]; ok {
		if channelStr, isStr := channelVal.(string); isStr {
			udsMsg.Channel = channelStr
		} else {
			return nil, fmt.Errorf("channel must be a string")
		}
	} else if action == "start" {
		return nil, fmt.Errorf("channel is required for start actions")
	}

	// AccessToken (required only for start actions)
	if tokenVal, ok := payload["access_token"]; ok {
		if tokenStr, isStr := tokenVal.(string); isStr {
			udsMsg.AccessToken = tokenStr
		} else {
			return nil, fmt.Errorf("access_token must be a string")
		}
	} else if action == "start" {
		return nil, fmt.Errorf("access_token is required for start actions")
	}

	// WorkerUid (required only for start actions)
	if workerUidVal, ok := payload["workerUid"]; ok {
		switch v := workerUidVal.(type) {
		case float64:
			udsMsg.WorkerUid = int(v)
		case int:
			udsMsg.WorkerUid = v
		case int64:
			udsMsg.WorkerUid = int(v)
		default:
			return nil, fmt.Errorf("workerUid must be a number")
		}
	} else if action == "start" {
		return nil, fmt.Errorf("workerUid is required for start actions")
	}

	// IntervalInMs (optional)
	if intervalVal, ok := payload["interval_in_ms"]; ok {
		switch v := intervalVal.(type) {
		case float64:
			udsMsg.IntervalInMs = int(v)
		case int:
			udsMsg.IntervalInMs = v
		case int64:
			udsMsg.IntervalInMs = int(v)
		default:
			return nil, fmt.Errorf("interval_in_ms must be a number")
		}
	} else {
		// Default to 20 seconds if not provided
		udsMsg.IntervalInMs = 20000
	}

	// TaskID (extract from payload if present)
	if taskIDVal, ok := payload["task_id"]; ok {
		if taskIDStr, isStr := taskIDVal.(string); isStr {
			udsMsg.TaskID = taskIDStr
		}
	}

	// Validate the flattened result
	if err := ValidateUDSMessage(udsMsg); err != nil {
		return nil, fmt.Errorf("validation failed after flattening: %v", err)
	}

	return udsMsg, nil
}

// ValidateUDSMessage validates a UDSMessage to ensure it's correct
func ValidateUDSMessage(msg *UDSMessage) error {
	// Validate cmd
	validCmds := map[string]bool{"snapshot": true, "record": true, "rtmp": true, "whip": true}
	if !validCmds[msg.Cmd] {
		return fmt.Errorf("cmd %s is not supported, only snapshot, record, rtmp, and whip are supported", msg.Cmd)
	}

	// Validate action
	validActions := map[string]bool{"start": true, "stop": true, "status": true}
	if !validActions[msg.Action] {
		return fmt.Errorf("action %s is not supported, only start, stop, and status are supported", msg.Action)
	}

	// Validate layout
	validLayouts := map[string]bool{"flat": true, "grid": true, "spotlight": true, "freestyle": true}
	if !validLayouts[msg.Layout] {
		return fmt.Errorf("layout %s is not supported, only flat, grid, spotlight, and freestyle are supported", msg.Layout)
	}

	// Validate freestyleCanvasUrl if present
	if msg.FreestyleCanvasUrl != "" {
		if len(msg.FreestyleCanvasUrl) > 1024 {
			return fmt.Errorf("freestyleCanvasUrl must be less than 1024 characters")
		}
		if strings.Contains(msg.FreestyleCanvasUrl, " ") {
			return fmt.Errorf("freestyleCanvasUrl must not contain whitespaces")
		}
		if !strings.HasPrefix(msg.FreestyleCanvasUrl, "http") && !strings.HasPrefix(msg.FreestyleCanvasUrl, "https") {
			return fmt.Errorf("freestyleCanvasUrl must start with http or https")
		}
	}

	// Validate uid count
	if len(msg.Uid) > 32 {
		return fmt.Errorf("uid count must be at most 32")
	}

	// Validate required fields based on action
	if msg.Action == "start" {
		// Start actions require channel, access_token, and workerUid
		if msg.Channel == "" {
			return fmt.Errorf("channel is required")
		}
		if msg.AccessToken == "" {
			return fmt.Errorf("access_token is required")
		}
		if msg.WorkerUid == 0 {
			return fmt.Errorf("workerUid is required and must be non-zero")
		}
	}
	// Stop and status actions don't require these fields - they use task_id for identification

	return nil
}

// ValidateAndFlattenTaskRequest validates a TaskRequest and returns a flattened UDSMessage
// This combines validation and flattening into a single operation that can be called before AssignTask
func ValidateAndFlattenTaskRequest(taskReq *TaskRequest) (*UDSMessage, error) {
	// First validate the TaskRequest
	if err := validateTaskRequest(taskReq); err != nil {
		return nil, fmt.Errorf("validation failed: %v", err)
	}

	// Then flatten the validated payload to UDSMessage
	udsMsg, err := FlattenPayloadToUDSMessage(taskReq.Cmd, taskReq.Action, taskReq.Payload)
	if err != nil {
		return nil, fmt.Errorf("flattening failed: %v", err)
	}

	return udsMsg, nil
}

// ValidateStartTaskRequest validates TaskRequest specifically for Redis publishing
// This includes additional validation for HTTP endpoint parameters
func ValidateStartTaskRequest(taskReq *TaskRequest) error {
	// Basic TaskRequest validation first
	if err := validateTaskRequest(taskReq); err != nil {
		return err
	}

	// Additional validation for Redis publishing
	payload := taskReq.Payload
	if payload == nil {
		return fmt.Errorf("payload cannot be nil for Redis tasks")
	}

	// Validate channel name format for Redis queues
	if channelVal, ok := payload["channel"]; ok {
		if channelStr, isStr := channelVal.(string); isStr {
			// Channel name validation for Redis key compatibility
			if len(channelStr) == 0 {
				return fmt.Errorf("channel name cannot be empty")
			}
			if len(channelStr) > 64 {
				return fmt.Errorf("channel name must be at most 64 characters")
			}
			// Redis key safe characters
			validChannelName := regexp.MustCompile(`^[a-zA-Z0-9_\-.:]+$`)
			if !validChannelName.MatchString(channelStr) {
				return fmt.Errorf("channel name contains invalid characters (only a-z, A-Z, 0-9, _, -, ., : allowed)")
			}
		}
	}

	// Validate access_token format
	if tokenVal, ok := payload["access_token"]; ok {
		if tokenStr, isStr := tokenVal.(string); isStr && tokenStr != "" {
			if len(tokenStr) < 10 {
				return fmt.Errorf("access_token too short (minimum 10 characters)")
			}
			if len(tokenStr) > 512 {
				return fmt.Errorf("access_token too long (maximum 512 characters)")
			}
		}
	}

	// Validate workerUid range
	if workerUidVal, ok := payload["workerUid"]; ok {
		var workerUid int
		switch v := workerUidVal.(type) {
		case float64:
			workerUid = int(v)
		case int:
			workerUid = v
		case int64:
			workerUid = int(v)
		}
		if workerUid < 1 || workerUid > 999999999 {
			return fmt.Errorf("workerUid must be between 1 and 999999999")
		}
	}

	// Validate interval_in_ms range
	if intervalVal, ok := payload["interval_in_ms"]; ok {
		var interval int
		switch v := intervalVal.(type) {
		case float64:
			interval = int(v)
		case int:
			interval = v
		case int64:
			interval = int(v)
		}
		if interval < 1000 {
			return fmt.Errorf("interval_in_ms must be at least 1000ms (1 second)")
		}
		if interval > 300000 {
			return fmt.Errorf("interval_in_ms must be at most 300000ms (5 minutes)")
		}
	}

	// Validate uid array elements
	if uidVal, ok := payload["uid"]; ok && uidVal != nil {
		switch v := uidVal.(type) {
		case []string:
			for i, uid := range v {
				if uid == "" {
					return fmt.Errorf("uid[%d] cannot be empty", i)
				}
				if len(uid) > 32 {
					return fmt.Errorf("uid[%d] must be at most 32 characters", i)
				}
				// Validate uid format (numbers and common characters)
				validUID := regexp.MustCompile(`^[a-zA-Z0-9_\-]+$`)
				if !validUID.MatchString(uid) {
					return fmt.Errorf("uid[%d] contains invalid characters", i)
				}
			}
		case []interface{}:
			for i, item := range v {
				if str, isStr := item.(string); isStr {
					if str == "" {
						return fmt.Errorf("uid[%d] cannot be empty", i)
					}
					if len(str) > 32 {
						return fmt.Errorf("uid[%d] must be at most 32 characters", i)
					}
					validUID := regexp.MustCompile(`^[a-zA-Z0-9_\-]+$`)
					if !validUID.MatchString(str) {
						return fmt.Errorf("uid[%d] contains invalid characters", i)
					}
				} else {
					return fmt.Errorf("uid[%d] must be a string", i)
				}
			}
		}
	}

	return nil
}

// ValidateStopTaskRequest validates parameters for stop task requests
func ValidateStopTaskRequest(requestID, taskID string) error {
	if err := ValidateRequestID(requestID); err != nil {
		return err
	}
	if err := ValidateTaskID(taskID); err != nil {
		return err
	}
	return nil
}

// ValidateStatusTaskRequest validates parameters for task status requests
func ValidateStatusTaskRequest(requestID, taskID string) error {
	// Same validation as stop task request
	return ValidateStopTaskRequest(requestID, taskID)
}

// ValidateRedisTask validates a queue.Task retrieved from Redis
func ValidateRedisTask(task *queue.Task) error {
	if task == nil {
		return fmt.Errorf("task cannot be nil")
	}

	// Validate task ID
	if err := ValidateTaskID(task.ID); err != nil {
		return fmt.Errorf("invalid task ID: %v", err)
	}

	// Validate basic task fields
	validTypes := map[string]bool{"snapshot": true, "record": true, "rtmp": true, "whip": true}
	if !validTypes[task.Type] {
		return fmt.Errorf("task type %s is not supported", task.Type)
	}

	validActions := map[string]bool{"start": true, "stop": true, "status": true}
	if !validActions[task.Action] {
		return fmt.Errorf("task action %s is not supported", task.Action)
	}

	// Validate channel name for Redis key safety
	if task.Channel != "" {
		if len(task.Channel) > 64 {
			return fmt.Errorf("channel name must be at most 64 characters")
		}
		validChannelName := regexp.MustCompile(`^[a-zA-Z0-9_\-.:]+$`)
		if !validChannelName.MatchString(task.Channel) {
			return fmt.Errorf("channel name contains invalid characters")
		}
	}

	// Validate request ID
	if task.RequestID != "" {
		if err := ValidateRequestID(task.RequestID); err != nil {
			return fmt.Errorf("invalid request ID: %v", err)
		}
	}

	// Validate payload for start actions
	if task.Action == "start" && task.Payload != nil {
		// Convert task.Payload to TaskRequest format for reusing existing validation
		taskReq := &TaskRequest{
			RequestID: task.RequestID,
			Cmd:       task.Type,
			Action:    task.Action,
			TaskID:    task.ID,
			Payload:   task.Payload,
		}
		if err := ValidateStartTaskRequest(taskReq); err != nil {
			return fmt.Errorf("payload validation failed: %v", err)
		}
	}

	return nil
}

// ValidateTaskID validates task ID format
func ValidateTaskID(taskID string) error {
	if taskID == "" {
		return fmt.Errorf("task_id is required")
	}
	if len(taskID) > 64 {
		return fmt.Errorf("task_id must be at most 64 characters")
	}
	// Task ID should be hexadecimal (from generateTaskID function)
	validTaskID := regexp.MustCompile(`^[a-f0-9]+$`)
	if !validTaskID.MatchString(taskID) {
		return fmt.Errorf("task_id format is invalid")
	}
	return nil
}

// ValidateRequestID validates request ID format
func ValidateRequestID(requestID string) error {
	if requestID == "" {
		return fmt.Errorf("request_id is required")
	}
	if len(requestID) > 32 {
		return fmt.Errorf("request_id must be at most 32 characters")
	}
	requestIdRegexp := regexp.MustCompile(`^[0-9a-zA-Z]{1,32}$`)
	if !requestIdRegexp.MatchString(requestID) {
		return fmt.Errorf("request_id must only contain 0-9, a-z, A-Z")
	}
	return nil
}

// Validate TaskRequest including request_id, cmd, and required payload fields
func validateTaskRequest(taskReq *TaskRequest) error {
	// Defensive: copy and update the payload map to ensure persistence
	payload := taskReq.Payload
	if payload == nil {
		payload = make(map[string]interface{})
	}
	// Validate request_id using reusable function
	if err := ValidateRequestID(taskReq.RequestID); err != nil {
		return err
	}
	// Validate mainCmd
	validCmds := map[string]bool{"snapshot": true, "record": true, "rtmp": true, "whip": true}
	if !validCmds[taskReq.Cmd] {
		return fmt.Errorf("%s unsupported, only snapshot, record, rtmp, and whip are supported", taskReq.Cmd)
	}
	// Validate action
	validActions := map[string]bool{"start": true, "stop": true, "status": true}
	if !validActions[taskReq.Action] {
		return fmt.Errorf("action %s not supported, only start, stop, and status are supported", taskReq.Action)
	}
	// Validate layout (optional, default to 'flat' if missing or empty)
	layoutVal, ok := payload["layout"]
	layoutStr := "flat"
	if ok {
		if s, isStr := layoutVal.(string); isStr && s != "" {
			layoutStr = s
		}
	}
	payload["layout"] = layoutStr
	validLayouts := map[string]bool{"flat": true, "grid": true, "spotlight": true, "freestyle": true}
	if !validLayouts[layoutStr] {
		return fmt.Errorf("layout %s not supported, only flat, grid, spotlight, and freestyle are supported", layoutStr)
	}
	// Validate freestyleCanvasUrl (optional, only if present and non-empty)
	canvasVal, ok := payload["freestyleCanvasUrl"]
	canvasStr := ""
	if ok {
		canvasStr, _ = canvasVal.(string)
		canvasStr = strings.TrimSpace(canvasStr)
		if canvasStr != "" {
			if len(canvasStr) > 1024 {
				return fmt.Errorf("freestyleCanvasUrl must be less than 1024 characters")
			}
			if strings.Contains(canvasStr, " ") {
				return fmt.Errorf("freestyleCanvasUrl must not contain whitespaces")
			}
			if !strings.HasPrefix(canvasStr, "http") && !strings.HasPrefix(canvasStr, "https") {
				return fmt.Errorf("freestyleCanvasUrl must start with http or https")
			}
		}
	}
	// Validate user count (optional, only if present and non-empty)
	uidVal, ok := payload["uid"]
	if ok && uidVal != nil {
		switch v := uidVal.(type) {
		case []string:
			if len(v) > 32 {
				return fmt.Errorf("users must be at most 32")
			}
		case []interface{}:
			if len(v) > 32 {
				return fmt.Errorf("users must be at most 32")
			}
		}
	}
	// Validate required fields based on action
	if taskReq.Action == "start" {
		// Start actions need channel, access_token, and workerUid
		requiredFields := []string{"channel", "access_token", "workerUid"}
		for _, field := range requiredFields {
			v, ok := payload[field]
			if !ok || v == nil || (field != "workerUid" && v == "") {
				return fmt.Errorf("%s is required", field)
			}
			if field == "workerUid" {
				if n, ok := v.(float64); !ok || int(n) == 0 {
					return fmt.Errorf("workerUid is required and must be non-zero")
				}
			}
		}
	} else if taskReq.Action == "stop" || taskReq.Action == "status" {
		// Stop and status actions need task_id to identify the specific task
		if taskReq.TaskID == "" {
			return fmt.Errorf("task_id is required for %s actions", taskReq.Action)
		}
	}
	taskReq.Payload = payload // persistently update the payload
	return nil
}

// Snapshot JSON:
// {
//   "cmd": "snapshot",
//   "action": "start",
//   "uid": ["1", "2", "3"], // user ids, if empty, all users will be included, we can support flexible user ids in the future
//   "layout": "flat",
//   "freestyleCanvasUrl": "http://your_customized_canvas.good.example",
//   "channel": "your_channel_id", // channel name
//   "access_token": "your_access_token",
//   "workerUid": 42,
//   "interval_in_ms": 20000
// }
