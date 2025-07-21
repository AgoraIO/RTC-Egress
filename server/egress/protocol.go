package egress

import (
	"fmt"
	"strings"
)

// UDSMessage defines the communication protocol between Go (egress) and C++ (eg_worker).
// All fields are serialized in JSON format.
type UDSMessage struct {
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

	// Channel (required)
	if channelVal, ok := payload["channel"]; ok {
		if channelStr, isStr := channelVal.(string); isStr {
			udsMsg.Channel = channelStr
		} else {
			return nil, fmt.Errorf("channel must be a string")
		}
	} else {
		return nil, fmt.Errorf("channel is required")
	}

	// AccessToken (required)
	if tokenVal, ok := payload["access_token"]; ok {
		if tokenStr, isStr := tokenVal.(string); isStr {
			udsMsg.AccessToken = tokenStr
		} else {
			return nil, fmt.Errorf("access_token must be a string")
		}
	} else {
		return nil, fmt.Errorf("access_token is required")
	}

	// WorkerUid (required)
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
	} else {
		return nil, fmt.Errorf("workerUid is required")
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
	validActions := map[string]bool{"start": true, "release": true, "status": true}
	if !validActions[msg.Action] {
		return fmt.Errorf("action %s is not supported, only start, release, and status are supported", msg.Action)
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

	// Validate required fields
	if msg.Channel == "" {
		return fmt.Errorf("channel is required")
	}
	if msg.AccessToken == "" {
		return fmt.Errorf("access_token is required")
	}
	if msg.WorkerUid == 0 {
		return fmt.Errorf("workerUid is required and must be non-zero")
	}

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

// Validate TaskRequest including request_id, cmd, and required payload fields
func validateTaskRequest(taskReq *TaskRequest) error {
	// Defensive: copy and update the payload map to ensure persistence
	payload := taskReq.Payload
	if payload == nil {
		payload = make(map[string]interface{})
	}
	// Validate request_id
	if taskReq.RequestID == "" {
		return fmt.Errorf("request_id is required")
	}
	if len(taskReq.RequestID) > 32 {
		return fmt.Errorf("request_id must be at most 32 characters")
	}
	if !requestIdRegexp.MatchString(taskReq.RequestID) {
		return fmt.Errorf("request_id must only contain 0-9, a-z, A-Z")
	}
	// Validate mainCmd
	validCmds := map[string]bool{"snapshot": true, "record": true, "rtmp": true, "whip": true}
	if !validCmds[taskReq.Cmd] {
		return fmt.Errorf("%s unsupported, only snapshot, record, rtmp, and whip are supported", taskReq.Cmd)
	}
	// Validate action
	validActions := map[string]bool{"start": true, "release": true, "status": true}
	if !validActions[taskReq.Action] {
		return fmt.Errorf("action %s not supported, only start, release, and status are supported", taskReq.Action)
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
	// Validate required fields in payload
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
