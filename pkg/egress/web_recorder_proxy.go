package egress

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

// WebRecorderConfig holds configuration for the web recorder service
type WebRecorderConfig struct {
	BaseURL    string `json:"base_url"`    // Base URL of the web recorder service
	Timeout    int    `json:"timeout"`     // HTTP timeout in seconds
	AuthToken  string `json:"auth_token"`  // Optional auth token for web recorder
	MaxRetries int    `json:"max_retries"` // Max retry attempts for HTTP requests
}

// WebRecorderRequest represents a request to the web recorder service
type WebRecorderRequest struct {
	RequestID string                 `json:"request_id"`
	Action    string                 `json:"action"` // "start", "stop", "status"
	TaskID    string                 `json:"task_id,omitempty"`
	URL       string                 `json:"url,omitempty"`
	Options   map[string]interface{} `json:"options,omitempty"`
}

// WebRecorderResponse represents a response from the web recorder service
type WebRecorderResponse struct {
	RequestID string `json:"request_id"`
	TaskID    string `json:"task_id,omitempty"`
	Status    string `json:"status"`
	Error     string `json:"error,omitempty"`
	Message   string `json:"message,omitempty"`
}

// WorkerManagerWebRecorderProxy implements the worker manager interface but delegates to a web recorder service
type WorkerManagerWebRecorderProxy struct {
	config     WebRecorderConfig
	httpClient *http.Client
	podID      string
}

// NewWorkerManagerWebRecorderProxy creates a new web recorder proxy
func NewWorkerManagerWebRecorderProxy(config WebRecorderConfig, podID string) *WorkerManagerWebRecorderProxy {
	timeout := time.Duration(config.Timeout) * time.Second
	if timeout == 0 {
		timeout = 30 * time.Second // Default timeout
	}

	maxRetries := config.MaxRetries
	if maxRetries == 0 {
		maxRetries = 3 // Default max retries
	}

	return &WorkerManagerWebRecorderProxy{
		config: config,
		httpClient: &http.Client{
			Timeout: timeout,
		},
		podID: podID,
	}
}

// makeHTTPRequest makes an HTTP request to the web recorder service with retries
func (proxy *WorkerManagerWebRecorderProxy) makeHTTPRequest(ctx context.Context, endpoint string, request interface{}) (*WebRecorderResponse, error) {
	reqBytes, err := json.Marshal(request)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal request: %v", err)
	}

	url := fmt.Sprintf("%s%s", proxy.config.BaseURL, endpoint)

	var lastErr error
	for attempt := 1; attempt <= proxy.config.MaxRetries; attempt++ {
		// Create HTTP request with context
		httpReq, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewBuffer(reqBytes))
		if err != nil {
			lastErr = fmt.Errorf("failed to create HTTP request: %v", err)
			continue
		}

		httpReq.Header.Set("Content-Type", "application/json")
		if proxy.config.AuthToken != "" {
			httpReq.Header.Set("Authorization", fmt.Sprintf("Bearer %s", proxy.config.AuthToken))
		}

		resp, err := proxy.httpClient.Do(httpReq)
		if err != nil {
			lastErr = fmt.Errorf("HTTP request failed (attempt %d/%d): %v", attempt, proxy.config.MaxRetries, err)
			if attempt < proxy.config.MaxRetries {
				time.Sleep(time.Duration(attempt) * time.Second) // Exponential backoff
			}
			continue
		}

		defer resp.Body.Close()
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			lastErr = fmt.Errorf("failed to read response body: %v", err)
			continue
		}

		// Check HTTP status code
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			lastErr = fmt.Errorf("HTTP request failed with status %d: %s", resp.StatusCode, string(body))
			if attempt < proxy.config.MaxRetries {
				time.Sleep(time.Duration(attempt) * time.Second) // Exponential backoff
			}
			continue
		}

		var webResp WebRecorderResponse
		if err := json.Unmarshal(body, &webResp); err != nil {
			lastErr = fmt.Errorf("failed to unmarshal response: %v", err)
			continue
		}

		return &webResp, nil
	}

	return nil, fmt.Errorf("all HTTP request attempts failed: %v", lastErr)
}

// startTask implements the task start logic for web recorder
func (proxy *WorkerManagerWebRecorderProxy) startTask(task *queue.Task) error {
	// Convert task to web recorder format
	options := make(map[string]interface{})

	// Extract URL from payload (web recorder needs a URL to record)
	if task.Payload != nil {
		if url, ok := task.Payload["url"].(string); ok && url != "" {
			options["url"] = url
		} else {
			return fmt.Errorf("web recorder tasks require a 'url' field in payload")
		}

		// Copy other options from payload
		for key, value := range task.Payload {
			if key != "url" {
				options[key] = value
			}
		}
	} else {
		return fmt.Errorf("web recorder tasks require payload with 'url'")
	}

	request := WebRecorderRequest{
		RequestID: task.RequestID,
		Action:    "start",
		TaskID:    task.ID,
		URL:       options["url"].(string),
		Options:   options,
	}

	ctx, cancel := context.WithTimeout(context.Background(), proxy.httpClient.Timeout)
	defer cancel()

	response, err := proxy.makeHTTPRequest(ctx, "/v1/record/start", request)
	if err != nil {
		return fmt.Errorf("failed to start web recording: %v", err)
	}

	if response.Status != "success" && response.Status != "started" {
		return fmt.Errorf("web recorder start failed: %s", response.Error)
	}

	log.Printf("Pod %s started web recording task %s successfully", proxy.podID, task.ID)
	return nil
}

// stopTask implements the task stop logic for web recorder
func (proxy *WorkerManagerWebRecorderProxy) stopTask(task *queue.Task) error {
	// Extract original task ID from stop task payload
	var originalTaskID string
	if task.Payload != nil {
		if taskID, ok := task.Payload["task_id"].(string); ok {
			originalTaskID = taskID
		}
	}

	if originalTaskID == "" {
		return fmt.Errorf("stop task %s missing original task_id in payload", task.ID)
	}

	request := WebRecorderRequest{
		RequestID: task.RequestID,
		Action:    "stop",
		TaskID:    originalTaskID,
	}

	ctx, cancel := context.WithTimeout(context.Background(), proxy.httpClient.Timeout)
	defer cancel()

	response, err := proxy.makeHTTPRequest(ctx, "/v1/record/stop", request)
	if err != nil {
		return fmt.Errorf("failed to stop web recording: %v", err)
	}

	if response.Status != "success" && response.Status != "stopped" {
		return fmt.Errorf("web recorder stop failed: %s", response.Error)
	}

	log.Printf("Pod %s stopped web recording task %s successfully", proxy.podID, originalTaskID)
	return nil
}

// getTaskStatus implements the task status logic for web recorder
func (proxy *WorkerManagerWebRecorderProxy) getTaskStatus(task *queue.Task) error {
	request := WebRecorderRequest{
		RequestID: task.RequestID,
		Action:    "status",
		TaskID:    task.ID,
	}

	ctx, cancel := context.WithTimeout(context.Background(), proxy.httpClient.Timeout)
	defer cancel()

	response, err := proxy.makeHTTPRequest(ctx, "/v1/record/status", request)
	if err != nil {
		return fmt.Errorf("failed to get web recording status: %v", err)
	}

	if response.Status == "error" {
		return fmt.Errorf("web recorder status failed: %s", response.Error)
	}

	log.Printf("Pod %s got status for web recording task %s: %s", proxy.podID, task.ID, response.Status)
	return nil
}

// ProcessRedisTask processes a Redis task by delegating to the web recorder service
func (proxy *WorkerManagerWebRecorderProxy) ProcessRedisTask(task *queue.Task) error {
	log.Printf("Processing web recorder task %s (type: %s, channel: %s, action: %s)",
		task.ID, task.Type, task.Channel, task.Action)

	// Validate Redis task before processing
	if err := ValidateRedisTask(task); err != nil {
		log.Printf("Pod %s - Redis task %s validation failed: %v", proxy.podID, task.ID, err)
		return fmt.Errorf("validation failed: %v", err)
	}

	// Delegate to appropriate action
	var err error
	switch task.Action {
	case "start":
		err = proxy.startTask(task)
	case "stop":
		err = proxy.stopTask(task)
	case "status":
		err = proxy.getTaskStatus(task)
	default:
		err = fmt.Errorf("unknown action: %s", task.Action)
	}

	if err != nil {
		log.Printf("Failed to process web recorder task %s: %v", task.ID, err)
		return err
	}

	log.Printf("Successfully processed web recorder task %s", task.ID)
	return nil
}

// GetPodID returns the pod ID for this proxy
func (proxy *WorkerManagerWebRecorderProxy) GetPodID() string {
	return proxy.podID
}

// Health check method for the web recorder proxy
func (proxy *WorkerManagerWebRecorderProxy) HealthCheck() error {
	request := map[string]interface{}{
		"action": "health",
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	_, err := proxy.makeHTTPRequest(ctx, "/health", request)
	if err != nil {
		return fmt.Errorf("web recorder health check failed: %v", err)
	}

	return nil
}
