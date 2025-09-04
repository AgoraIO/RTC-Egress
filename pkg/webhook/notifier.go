package webhook

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/redis/go-redis/v9"
)

// NotificationState represents the state of a notification
type NotificationState struct {
	TaskID        string    `json:"task_id"`
	LastState     string    `json:"last_state"`
	LastNotified  time.Time `json:"last_notified"`
	RetryCount    int       `json:"retry_count"`
	LastError     string    `json:"last_error,omitempty"`
	NextRetryTime time.Time `json:"next_retry_time,omitempty"`
}

// WebhookPayload represents the payload sent to webhook endpoints
type WebhookPayload struct {
	TaskID      string                 `json:"task_id"`
	RequestID   string                 `json:"request_id"`
	Cmd         string                 `json:"cmd"`     // "snapshot", "record", "web:record"
	Action      string                 `json:"action"`  // "start", "stop", "status"
	Channel     string                 `json:"channel"` // channel name
	State       string                 `json:"state"`   // ENQUEUED, PROCESSING, SUCCESS, FAILED, TIMEOUT
	Error       string                 `json:"error,omitempty"`
	WorkerID    int                    `json:"worker_id,omitempty"`
	Payload     map[string]interface{} `json:"payload,omitempty"`
	CreatedAt   time.Time              `json:"created_at"`
	ProcessedAt *time.Time             `json:"processed_at,omitempty"`
	CompletedAt *time.Time             `json:"completed_at,omitempty"`
	RetryCount  int                    `json:"retry_count,omitempty"`
	Timestamp   time.Time              `json:"timestamp"` // When notification was sent
}

// WebhookConfig holds configuration for webhook notifications
type WebhookConfig struct {
	URL               string        `json:"url"`                  // Webhook endpoint URL
	Timeout           time.Duration `json:"timeout"`              // HTTP request timeout
	MaxRetries        int           `json:"max_retries"`          // Maximum retry attempts (default: 5)
	BaseRetryInterval time.Duration `json:"base_retry_interval"`  // Base interval for retries
	MaxRetryInterval  time.Duration `json:"max_retry_interval"`   // Maximum retry interval
	AuthToken         string        `json:"auth_token,omitempty"` // Optional auth token
	NotifyStates      []string      `json:"notify_states"`        // States to notify about
	IncludePayload    bool          `json:"include_payload"`      // Include task payload in notification
	BatchSize         int           `json:"batch_size"`           // Batch notifications (0 = disabled)
	BatchTimeout      time.Duration `json:"batch_timeout"`        // Batch timeout
}

// DefaultWebhookConfig returns default webhook configuration
func DefaultWebhookConfig() WebhookConfig {
	return WebhookConfig{
		Timeout:           30 * time.Second,
		MaxRetries:        5,
		BaseRetryInterval: queue.LeaseRenewalInterval, // 15 seconds
		MaxRetryInterval:  5 * time.Minute,
		NotifyStates:      []string{queue.TaskStateProcessing, queue.TaskStateSuccess, queue.TaskStateFailed, queue.TaskStateTimeout},
		IncludePayload:    false,
		BatchSize:         0, // Disabled by default
		BatchTimeout:      5 * time.Second,
	}
}

// WebhookNotifier handles webhook notifications for task state changes
type WebhookNotifier struct {
	config        WebhookConfig
	httpClient    *http.Client
	redisClient   *redis.Client
	notifications map[string]*NotificationState // Maps task_id -> notification state
	notifMutex    sync.RWMutex
	stopChan      chan struct{}
	wg            sync.WaitGroup

	// Batch processing
	batchQueue     []WebhookPayload
	batchMutex     sync.Mutex
	lastBatchFlush time.Time
}

// NewWebhookNotifier creates a new webhook notifier
func NewWebhookNotifier(config WebhookConfig, redisClient *redis.Client) *WebhookNotifier {
	// Apply defaults
	if config.MaxRetries == 0 {
		config.MaxRetries = 5
	}
	if config.BaseRetryInterval == 0 {
		config.BaseRetryInterval = queue.LeaseRenewalInterval
	}
	if config.MaxRetryInterval == 0 {
		config.MaxRetryInterval = 5 * time.Minute
	}
	if config.Timeout == 0 {
		config.Timeout = 30 * time.Second
	}
	if len(config.NotifyStates) == 0 {
		config.NotifyStates = []string{queue.TaskStateProcessing, queue.TaskStateSuccess, queue.TaskStateFailed, queue.TaskStateTimeout}
	}

	return &WebhookNotifier{
		config:         config,
		httpClient:     &http.Client{Timeout: config.Timeout},
		redisClient:    redisClient,
		notifications:  make(map[string]*NotificationState),
		stopChan:       make(chan struct{}),
		batchQueue:     make([]WebhookPayload, 0),
		lastBatchFlush: time.Now(),
	}
}

// Start begins listening for Redis task updates and processing notifications
func (wn *WebhookNotifier) Start(ctx context.Context) error {
	log.Printf("Starting webhook notifier for URL: %s", wn.config.URL)

	// Enable Redis keyspace notifications if not already enabled
	if err := wn.enableRedisKeyspaceNotifications(ctx); err != nil {
		log.Printf("Warning: Failed to enable Redis keyspace notifications: %v", err)
	}

	// Start Redis subscriber
	wn.wg.Add(1)
	go wn.startRedisSubscriber(ctx)

	// Start retry processor
	wn.wg.Add(1)
	go wn.startRetryProcessor()

	// Start batch processor if batching is enabled
	if wn.config.BatchSize > 0 {
		wn.wg.Add(1)
		go wn.startBatchProcessor()
	}

	log.Printf("Webhook notifier started successfully")
	return nil
}

// Stop gracefully stops the webhook notifier
func (wn *WebhookNotifier) Stop() {
	log.Printf("Stopping webhook notifier")
	close(wn.stopChan)
	wn.wg.Wait()
	log.Printf("Webhook notifier stopped")
}

// enableRedisKeyspaceNotifications enables keyspace notifications for task updates
func (wn *WebhookNotifier) enableRedisKeyspaceNotifications(ctx context.Context) error {
	// Enable keyspace notifications for SET operations on egress:task:* keys
	_, err := wn.redisClient.ConfigSet(ctx, "notify-keyspace-events", "Kg$").Result()
	if err != nil {
		return fmt.Errorf("failed to enable keyspace notifications: %v", err)
	}
	log.Printf("Enabled Redis keyspace notifications for task updates")
	return nil
}

// startRedisSubscriber listens for Redis keyspace notifications on task updates
func (wn *WebhookNotifier) startRedisSubscriber(ctx context.Context) {
	defer wn.wg.Done()

	// Subscribe to keyspace notifications for egress:task:* keys
	pubsub := wn.redisClient.PSubscribe(ctx, "__keyspace@*__:egress:task:*")
	defer pubsub.Close()

	log.Printf("Subscribed to Redis keyspace notifications for task updates")

	for {
		select {
		case <-wn.stopChan:
			log.Printf("Redis subscriber stopping")
			return
		case <-ctx.Done():
			log.Printf("Redis subscriber context cancelled")
			return
		default:
			msg, err := pubsub.ReceiveTimeout(ctx, 1*time.Second)
			if err != nil {
				if err == redis.Nil {
					continue // Timeout, continue waiting for events
				}
				// Handle I/O timeouts silently (normal when no events)
				if strings.Contains(err.Error(), "timeout") || strings.Contains(err.Error(), "i/o timeout") {
					continue
				}
				log.Printf("Error receiving Redis message: %v", err)
				time.Sleep(1 * time.Second)
				continue
			}

			if pmsg, ok := msg.(*redis.Message); ok {
				wn.handleRedisKeyspaceNotification(ctx, pmsg)
			}
		}
	}
}

// handleRedisKeyspaceNotification processes Redis keyspace notifications
func (wn *WebhookNotifier) handleRedisKeyspaceNotification(ctx context.Context, msg *redis.Message) {
	// Extract task ID from the key: __keyspace@0__:egress:task:TASK_ID
	if msg.Payload != "set" {
		return // Only interested in SET operations (task updates)
	}

	// Parse task ID from channel name
	taskKey := msg.Channel[len("__keyspace@0__:"):]
	if !isValidTaskKey(taskKey) {
		return
	}
	taskID := extractTaskIDFromKey(taskKey)

	// Fetch the updated task
	task, err := wn.getTaskFromRedis(ctx, taskID)
	if err != nil {
		log.Printf("Failed to fetch updated task %s: %v", taskID, err)
		return
	}

	// Check if we should notify about this state
	if !wn.shouldNotifyState(task.State) {
		return
	}

	wn.processTaskNotification(task)
}

// processTaskNotification processes a task for webhook notification
func (wn *WebhookNotifier) processTaskNotification(task *queue.Task) {
	wn.notifMutex.Lock()
	defer wn.notifMutex.Unlock()

	notifState, exists := wn.notifications[task.ID]
	if !exists {
		notifState = &NotificationState{
			TaskID: task.ID,
		}
		wn.notifications[task.ID] = notifState
	}

	// Check if we already notified about this state successfully
	if notifState.LastState == task.State && notifState.RetryCount == 0 {
		return // Already successfully notified
	}

	// Check if we're in retry backoff period
	if notifState.RetryCount > 0 && time.Now().Before(notifState.NextRetryTime) {
		return // Still in backoff period
	}

	// Create webhook payload
	payload := WebhookPayload{
		TaskID:      task.ID,
		RequestID:   task.RequestID,
		Cmd:         task.Cmd,
		Action:      task.Action,
		Channel:     task.Channel,
		State:       task.State,
		Error:       task.Error,
		WorkerID:    task.WorkerID,
		CreatedAt:   task.CreatedAt,
		ProcessedAt: task.ProcessedAt,
		CompletedAt: task.CompletedAt,
		RetryCount:  task.RetryCount,
		Timestamp:   time.Now(),
	}

	if wn.config.IncludePayload {
		payload.Payload = task.Payload
	}

	// Send notification (batched or immediate)
	if wn.config.BatchSize > 0 {
		wn.addToBatch(payload)
	} else {
		wn.sendNotification(payload, notifState)
	}
}

// addToBatch adds a notification to the batch queue
func (wn *WebhookNotifier) addToBatch(payload WebhookPayload) {
	wn.batchMutex.Lock()
	defer wn.batchMutex.Unlock()

	wn.batchQueue = append(wn.batchQueue, payload)

	// Flush if batch is full
	if len(wn.batchQueue) >= wn.config.BatchSize {
		wn.flushBatch()
	}
}

// flushBatch sends all queued notifications
func (wn *WebhookNotifier) flushBatch() {
	if len(wn.batchQueue) == 0 {
		return
	}

	batch := make([]WebhookPayload, len(wn.batchQueue))
	copy(batch, wn.batchQueue)
	wn.batchQueue = wn.batchQueue[:0] // Clear the batch
	wn.lastBatchFlush = time.Now()

	// Send batch in background
	go wn.sendBatchNotification(batch)
}

// startBatchProcessor processes batch notifications with timeout
func (wn *WebhookNotifier) startBatchProcessor() {
	defer wn.wg.Done()

	ticker := time.NewTicker(wn.config.BatchTimeout)
	defer ticker.Stop()

	for {
		select {
		case <-wn.stopChan:
			// Flush remaining batch before stopping
			wn.batchMutex.Lock()
			wn.flushBatch()
			wn.batchMutex.Unlock()
			return
		case <-ticker.C:
			wn.batchMutex.Lock()
			if len(wn.batchQueue) > 0 && time.Since(wn.lastBatchFlush) >= wn.config.BatchTimeout {
				wn.flushBatch()
			}
			wn.batchMutex.Unlock()
		}
	}
}

// sendNotification sends a single webhook notification
func (wn *WebhookNotifier) sendNotification(payload WebhookPayload, notifState *NotificationState) {
	success := wn.makeWebhookRequest([]WebhookPayload{payload})

	if success {
		notifState.LastState = payload.State
		notifState.LastNotified = time.Now()
		notifState.RetryCount = 0
		notifState.LastError = ""
		notifState.NextRetryTime = time.Time{}
		log.Printf("Successfully sent webhook notification for task %s (state: %s)", payload.TaskID, payload.State)
	} else {
		notifState.RetryCount++
		if notifState.RetryCount <= wn.config.MaxRetries {
			// Calculate next retry time with exponential backoff
			backoff := wn.calculateBackoff(notifState.RetryCount)
			notifState.NextRetryTime = time.Now().Add(backoff)
			log.Printf("Webhook notification failed for task %s (state: %s), retry %d/%d in %v",
				payload.TaskID, payload.State, notifState.RetryCount, wn.config.MaxRetries, backoff)
		} else {
			log.Printf("Webhook notification permanently failed for task %s (state: %s) after %d retries",
				payload.TaskID, payload.State, wn.config.MaxRetries)
			// Reset for future state changes
			notifState.RetryCount = 0
		}
	}
}

// sendBatchNotification sends a batch of webhook notifications
func (wn *WebhookNotifier) sendBatchNotification(batch []WebhookPayload) {
	success := wn.makeWebhookRequest(batch)

	if success {
		log.Printf("Successfully sent webhook batch notification with %d items", len(batch))
		// Update all notification states in the batch
		wn.notifMutex.Lock()
		for _, payload := range batch {
			if notifState, exists := wn.notifications[payload.TaskID]; exists {
				notifState.LastState = payload.State
				notifState.LastNotified = time.Now()
				notifState.RetryCount = 0
				notifState.LastError = ""
				notifState.NextRetryTime = time.Time{}
			}
		}
		wn.notifMutex.Unlock()
	} else {
		log.Printf("Webhook batch notification failed with %d items", len(batch))
		// Handle batch retry - for simplicity, we'll retry the entire batch
		// In production, you might want to retry individual items
		wn.notifMutex.Lock()
		for _, payload := range batch {
			if notifState, exists := wn.notifications[payload.TaskID]; exists {
				notifState.RetryCount++
				if notifState.RetryCount <= wn.config.MaxRetries {
					backoff := wn.calculateBackoff(notifState.RetryCount)
					notifState.NextRetryTime = time.Now().Add(backoff)
				} else {
					notifState.RetryCount = 0 // Reset for future state changes
				}
			}
		}
		wn.notifMutex.Unlock()
	}
}

// makeWebhookRequest makes the actual HTTP request to the webhook endpoint
func (wn *WebhookNotifier) makeWebhookRequest(payloads []WebhookPayload) bool {
	var requestBody interface{}
	if len(payloads) == 1 {
		requestBody = payloads[0]
	} else {
		requestBody = map[string]interface{}{
			"notifications": payloads,
			"batch_size":    len(payloads),
		}
	}

	jsonData, err := json.Marshal(requestBody)
	if err != nil {
		log.Printf("Failed to marshal webhook payload: %v", err)
		return false
	}

	ctx, cancel := context.WithTimeout(context.Background(), wn.config.Timeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, "POST", wn.config.URL, bytes.NewBuffer(jsonData))
	if err != nil {
		log.Printf("Failed to create webhook request: %v", err)
		return false
	}

	req.Header.Set("Content-Type", "application/json")
	if wn.config.AuthToken != "" {
		req.Header.Set("Authorization", fmt.Sprintf("Bearer %s", wn.config.AuthToken))
	}

	resp, err := wn.httpClient.Do(req)
	if err != nil {
		log.Printf("Webhook request failed: %v", err)
		return false
	}
	defer resp.Body.Close()

	// Consider 2xx status codes as success
	if resp.StatusCode >= 200 && resp.StatusCode < 300 {
		return true
	}

	log.Printf("Webhook request returned non-success status: %d", resp.StatusCode)
	return false
}

// calculateBackoff calculates exponential backoff with jitter
func (wn *WebhookNotifier) calculateBackoff(retryCount int) time.Duration {
	// Exponential backoff: base * 2^(retry-1)
	backoff := wn.config.BaseRetryInterval * time.Duration(1<<uint(retryCount-1))

	// Cap at max retry interval
	if backoff > wn.config.MaxRetryInterval {
		backoff = wn.config.MaxRetryInterval
	}

	return backoff
}

// startRetryProcessor processes retry notifications
func (wn *WebhookNotifier) startRetryProcessor() {
	defer wn.wg.Done()

	ticker := time.NewTicker(wn.config.BaseRetryInterval)
	defer ticker.Stop()

	for {
		select {
		case <-wn.stopChan:
			return
		case <-ticker.C:
			wn.processRetries()
		}
	}
}

// processRetries processes pending retry notifications
func (wn *WebhookNotifier) processRetries() {
	wn.notifMutex.Lock()
	defer wn.notifMutex.Unlock()

	now := time.Now()
	for taskID, notifState := range wn.notifications {
		if notifState.RetryCount > 0 &&
			notifState.RetryCount <= wn.config.MaxRetries &&
			now.After(notifState.NextRetryTime) {

			// Fetch current task state
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			task, err := wn.getTaskFromRedis(ctx, taskID)
			cancel()

			if err != nil {
				log.Printf("Failed to fetch task %s for retry: %v", taskID, err)
				continue
			}

			// Create payload and retry
			payload := WebhookPayload{
				TaskID:      task.ID,
				RequestID:   task.RequestID,
				Cmd:         task.Cmd,
				Action:      task.Action,
				Channel:     task.Channel,
				State:       task.State,
				Error:       task.Error,
				WorkerID:    task.WorkerID,
				CreatedAt:   task.CreatedAt,
				ProcessedAt: task.ProcessedAt,
				CompletedAt: task.CompletedAt,
				RetryCount:  task.RetryCount,
				Timestamp:   time.Now(),
			}

			if wn.config.IncludePayload {
				payload.Payload = task.Payload
			}

			go wn.sendNotification(payload, notifState)
		}
	}
}

// Helper functions

// getTaskFromRedis fetches a task from Redis
func (wn *WebhookNotifier) getTaskFromRedis(ctx context.Context, taskID string) (*queue.Task, error) {
	taskKey := fmt.Sprintf("egress:task:%s", taskID)
	taskData, err := wn.redisClient.Get(ctx, taskKey).Result()
	if err != nil {
		return nil, err
	}

	var task queue.Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return nil, err
	}

	return &task, nil
}

// shouldNotifyState checks if we should send notifications for this state
func (wn *WebhookNotifier) shouldNotifyState(state string) bool {
	for _, notifyState := range wn.config.NotifyStates {
		if state == notifyState {
			return true
		}
	}
	return false
}

// isValidTaskKey checks if a Redis key is a valid task key
func isValidTaskKey(key string) bool {
	return len(key) > len("egress:task:") && key[:len("egress:task:")] == "egress:task:"
}

// extractTaskIDFromKey extracts task ID from Redis key
func extractTaskIDFromKey(key string) string {
	return key[len("egress:task:"):]
}

// GetNotificationHash returns a hash for deduplication (can be used for external storage)
func GetNotificationHash(payload WebhookPayload) string {
	hashInput := fmt.Sprintf("%s:%s:%d", payload.TaskID, payload.State, payload.Timestamp.Unix())
	hash := sha256.Sum256([]byte(hashInput))
	return fmt.Sprintf("%x", hash[:8])
}
