package queue

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"log"
	"time"

	"github.com/redis/go-redis/v9"
)

const (
	TaskStateEnqueued   = "ENQUEUED"   // Task waiting in queue (can timeout after TaskTimeout)
	TaskStateProcessing = "PROCESSING" // Worker actively working on task
	TaskStateStopping   = "STOPPING"   // Stop requested; worker is stopping the task
	TaskStateStopped    = "STOPPED"    // Task stopped (by user or completed normally)
	TaskStateFailed     = "FAILED"     // Fatal error(Can not be retried/recovered), like wrong access_token, no more disk space, etc. no user stop call needed
	TaskStateTimeout    = "TIMEOUT"    // Task aged out while ENQUEUED (business staleness). no user stop call needed

	// Business timeout settings
	TaskTimeout = 30 * time.Second // Max time from creation to start processing (business staleness)

	// Lease settings for automatic failover
	LeaseRenewalInterval = 15 * time.Second // How often workers renew lease
	LeaseTimeout         = 45 * time.Second // When to consider worker dead
	CleanupInterval      = 5 * time.Second  // How often to check for expired leases and aged tasks
)

type Task struct {
	ID          string                 `json:"id"`
	Cmd         string                 `json:"cmd"`     // "snapshot", "record", "web:record"
	Action      string                 `json:"action"`  // "start", "stop", "status"
	Channel     string                 `json:"channel"` // channel name
	RequestID   string                 `json:"request_id"`
	Payload     map[string]interface{} `json:"payload"`
	State       string                 `json:"state"`
	CreatedAt   time.Time              `json:"created_at"`
	EnqueuedAt  *time.Time             `json:"enqueued_at,omitempty"` // When task was last set to ENQUEUED (for timeout calculation)
	ProcessedAt *time.Time             `json:"processed_at,omitempty"`
	CompletedAt *time.Time             `json:"completed_at,omitempty"`
	Error       string                 `json:"error,omitempty"`
	WorkerID    int                    `json:"worker_id,omitempty"`
	LeaseExpiry *time.Time             `json:"lease_expiry,omitempty"` // When worker lease expires
	RetryCount  int                    `json:"retry_count,omitempty"`  // Number of retry attempts
	// SourceQueue indicates from which Redis queue this task was claimed.
	// Not persisted to Redis; used to determine web vs native routing.
	SourceQueue string `json:"-"`
}

type RedisQueue struct {
	client *redis.Client
	ttl    time.Duration
	region string // Current pod's region
}

func NewRedisQueue(addr, password string, db int, ttl int, region string) *RedisQueue {
	rdb := redis.NewClient(&redis.Options{
		Addr:     addr,
		Password: password,
		DB:       db,
	})

	return &RedisQueue{
		client: rdb,
		ttl:    time.Duration(ttl) * time.Second,
		region: region,
	}
}

func (rq *RedisQueue) generateTaskID(cmd, channel, requestID string) string {
	data := fmt.Sprintf("%s:%s:%s:%d", cmd, channel, requestID, time.Now().UnixNano())
	hash := sha256.Sum256([]byte(data))
	return fmt.Sprintf("%x", hash[:8])
}

func (rq *RedisQueue) buildQueueKey(cmd, channel string) string {
	return fmt.Sprintf("egress:%s:%s", cmd, channel)
}

// buildRegionalQueueKey builds a region-specific queue key
func (rq *RedisQueue) buildRegionalQueueKey(region, cmd, channel string) string {
	if region == "" {
		return fmt.Sprintf("egress:%s:%s", cmd, channel)
	}
	return fmt.Sprintf("egress:%s:%s:%s", region, cmd, channel)
}

// BuildRegionalQueueKey builds a region-specific queue key (public)
func (rq *RedisQueue) BuildRegionalQueueKey(region, cmd, channel string) string {
	return rq.buildRegionalQueueKey(region, cmd, channel)
}

func (rq *RedisQueue) BuildQueueKey(cmd, channel string) string {
	return rq.buildQueueKey(cmd, channel)
}

func (rq *RedisQueue) buildTaskKey(taskID string) string {
	return fmt.Sprintf("egress:task:%s", taskID)
}

func (rq *RedisQueue) BuildTaskKey(taskID string) string {
	return rq.buildTaskKey(taskID)
}

func (rq *RedisQueue) buildDedupeKey(cmd, channel, requestID string) string {
	return fmt.Sprintf("egress:dedupe:%s:%s:%s", cmd, channel, requestID)
}

func (rq *RedisQueue) buildProcessingQueueKey(workerID string) string {
	return fmt.Sprintf("egress:processing:%s", workerID)
}

func (rq *RedisQueue) BuildProcessingQueueKey(workerID string) string {
	return rq.buildProcessingQueueKey(workerID)
}

func (rq *RedisQueue) buildLeaseKey(taskID string) string {
	return fmt.Sprintf("egress:lease:%s", taskID)
}

func (rq *RedisQueue) BuildLeaseKey(taskID string) string {
	return rq.buildLeaseKey(taskID)
}

// GetRegion returns the region for this RedisQueue
func (rq *RedisQueue) GetRegion() string {
	return rq.region
}

// PublishStopTaskFor publishes a stop task for an existing task ID.
// It derives routing (global vs regional, web vs native) from the original task,
// without requiring layout in the stop payload.
func (rq *RedisQueue) PublishStopTaskFor(ctx context.Context, originalTaskID, requestID, region string) (*Task, error) {
	// Load original task
	origTaskKey := rq.buildTaskKey(originalTaskID)
	origData, err := rq.client.Get(ctx, origTaskKey).Result()
	if err != nil {
		return nil, fmt.Errorf("failed to load original task %s: %v", originalTaskID, err)
	}

	var orig Task
	if err := json.Unmarshal([]byte(origData), &orig); err != nil {
		return nil, fmt.Errorf("failed to unmarshal original task %s: %v", originalTaskID, err)
	}

	// Derive layout from original payload for routing only
	layout := "flat"
	if orig.Payload != nil {
		if lv, ok := orig.Payload["layout"]; ok {
			if ls, ok2 := lv.(string); ok2 && ls != "" {
				layout = ls
			}
		}
	}

	// Build queue key based on original cmd/channel and derived layout
	queueKey := rq.getQueueKey(layout, orig.Cmd, orig.Channel, region)

	// Dedupe by (cmd, channel, requestID)
	dedupeKey := rq.buildDedupeKey(orig.Cmd, orig.Channel, requestID)
	exists, err := rq.client.Exists(ctx, dedupeKey).Result()
	if err != nil {
		return nil, fmt.Errorf("failed to check duplicate: %v", err)
	}
	if exists > 0 {
		return nil, fmt.Errorf("duplicate stop request for cmd=%s channel=%s request_id=%s", orig.Cmd, orig.Channel, requestID)
	}

	// Create stop task with minimal payload (no layout needed)
	stopID := rq.generateTaskID(orig.Cmd, orig.Channel, requestID)
	now := time.Now()
	stopTask := &Task{
		ID:         stopID,
		Cmd:        orig.Cmd,
		Action:     "stop",
		Channel:    orig.Channel,
		RequestID:  requestID,
		Payload:    map[string]interface{}{"task_id": originalTaskID},
		State:      TaskStateEnqueued,
		CreatedAt:  now,
		EnqueuedAt: &now,
	}

	data, err := json.Marshal(stopTask)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal stop task: %v", err)
	}

	pipe := rq.client.TxPipeline()
	pipe.LPush(ctx, queueKey, stopID)
	pipe.Expire(ctx, queueKey, rq.ttl)
	pipe.Set(ctx, rq.buildTaskKey(stopID), data, rq.ttl)
	pipe.Set(ctx, dedupeKey, stopID, rq.ttl)
	if _, err := pipe.Exec(ctx); err != nil {
		return nil, fmt.Errorf("failed to publish stop task: %v", err)
	}

	log.Printf("Published stop task %s for original %s to queue %s", stopID, originalTaskID, queueKey)
	return stopTask, nil
}

// PublishTaskToRegion publishes a task to a specific region queue
func (rq *RedisQueue) PublishTaskToRegion(ctx context.Context, cmd, action, channel, requestID string, payload map[string]interface{}, region string) (*Task, error) {
	dedupeKey := rq.buildDedupeKey(cmd, channel, requestID)

	exists, err := rq.client.Exists(ctx, dedupeKey).Result()
	if err != nil {
		return nil, fmt.Errorf("failed to check duplicate: %v", err)
	}
	if exists > 0 {
		return nil, fmt.Errorf("duplicate task: %s for channel %s with request_id %s", cmd, channel, requestID)
	}

	taskID := rq.generateTaskID(cmd, channel, requestID)
	now := time.Now()
	task := &Task{
		ID:         taskID,
		Cmd:        cmd,
		Action:     action,
		Channel:    channel,
		RequestID:  requestID,
		Payload:    payload,
		State:      TaskStateEnqueued,
		CreatedAt:  now,
		EnqueuedAt: &now, // Set initial ENQUEUED timestamp
	}

	taskData, err := json.Marshal(task)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal task: %v", err)
	}

	pipe := rq.client.TxPipeline()

	// Determine queue routing based on layout in payload

	layout := "flat" // default
	if payload != nil {
		if layoutVal, ok := payload["layout"]; ok {
			if layoutStr, isStr := layoutVal.(string); isStr && layoutStr != "" {
				layout = layoutStr
			}
		}
	}

	queueKey := rq.getQueueKey(layout, cmd, channel, region)

	taskKey := rq.buildTaskKey(taskID)

	pipe.LPush(ctx, queueKey, taskID)
	pipe.Expire(ctx, queueKey, rq.ttl)

	pipe.Set(ctx, taskKey, taskData, rq.ttl)
	pipe.Set(ctx, dedupeKey, taskID, rq.ttl)

	_, err = pipe.Exec(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to publish task: %v", err)
	}

	log.Printf("Published task %s to queue %s", taskID, queueKey)
	return task, nil
}

func (rq *RedisQueue) getQueueKey(layout, cmd, channel, region string) string {
	if layout == "freestyle" {
		// Route to flexible-recorder pods - matches patterns "web:*" and "*:web:*"
		if region != "" {
			return fmt.Sprintf("egress:%s:web:%s:%s", region, cmd, channel)
		} else {
			return fmt.Sprintf("egress:web:%s:%s", cmd, channel)
		}
	} else {
		// Route to native egress pods - matches patterns "egress:snapshot:*" etc.
		if region != "" {
			return fmt.Sprintf("egress:%s:%s:%s", region, cmd, channel)
		} else {
			return fmt.Sprintf("egress:%s:%s", cmd, channel)
		}
	}
}

// ClaimTaskWithLease updates task state and creates a lease
func (rq *RedisQueue) ClaimTaskWithLease(ctx context.Context, taskID, workerID string) (*Task, error) {
	taskKey := rq.buildTaskKey(taskID)
	leaseKey := rq.buildLeaseKey(taskID)

	taskData, err := rq.client.Get(ctx, taskKey).Result()
	if err != nil {
		return nil, fmt.Errorf("failed to get task data: %v", err)
	}

	var task Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return nil, fmt.Errorf("failed to unmarshal task: %v", err)
	}

	// Update task with lease information
	now := time.Now()
	leaseExpiry := now.Add(LeaseTimeout)
	task.State = TaskStateProcessing
	task.ProcessedAt = &now
	// WorkerID remains 0 for now - will be updated when task is assigned to specific worker
	task.WorkerID = 0
	task.LeaseExpiry = &leaseExpiry

	updatedData, err := json.Marshal(task)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal updated task: %v", err)
	}

	// Atomic update: task data + lease creation
	pipe := rq.client.TxPipeline()
	pipe.Set(ctx, taskKey, updatedData, rq.ttl)
	pipe.Set(ctx, leaseKey, workerID, LeaseTimeout) // Lease expires automatically (workerID here is manager ID string)
	_, err = pipe.Exec(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to update task and create lease: %v", err)
	}

	return &task, nil
}

func (rq *RedisQueue) getMatchingQueues(ctx context.Context, patterns []string) []string {
	var allQueues []string

	for _, pattern := range patterns {
		queuePattern := fmt.Sprintf("egress:%s", pattern)
		keys, err := rq.client.Keys(ctx, queuePattern).Result()
		if err != nil {
			log.Printf("Error getting keys for pattern %s: %v", pattern, err)
			continue
		}
		allQueues = append(allQueues, keys...)
	}

	uniqueQueues := make(map[string]bool)
	var result []string
	for _, queue := range allQueues {
		if !uniqueQueues[queue] {
			uniqueQueues[queue] = true
			result = append(result, queue)
		}
	}

	return result
}

func (rq *RedisQueue) UpdateTaskWorker(ctx context.Context, taskID string, workerID int) error {
	taskKey := rq.buildTaskKey(taskID)

	taskData, err := rq.client.Get(ctx, taskKey).Result()
	if err != nil {
		return fmt.Errorf("failed to get task data: %v", err)
	}

	var task Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return fmt.Errorf("failed to unmarshal task: %v", err)
	}

	task.WorkerID = workerID

	updatedData, err := json.Marshal(task)
	if err != nil {
		return fmt.Errorf("failed to marshal updated task: %v", err)
	}

	err = rq.client.Set(ctx, taskKey, updatedData, rq.ttl).Err()
	if err != nil {
		return fmt.Errorf("failed to update task worker: %v", err)
	}

	log.Printf("Updated task %s worker to %d", taskID, workerID)
	return nil
}

func (rq *RedisQueue) UpdateTaskResult(ctx context.Context, taskID, state, errorMsg string) error {
	taskKey := rq.buildTaskKey(taskID)

	taskData, err := rq.client.Get(ctx, taskKey).Result()
	if err != nil {
		return fmt.Errorf("failed to get task data: %v", err)
	}

	var task Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return fmt.Errorf("failed to unmarshal task: %v", err)
	}

	now := time.Now()
	task.State = state
	// Only set CompletedAt for terminal states
	if state == TaskStateStopped || state == TaskStateFailed || state == TaskStateTimeout {
		task.CompletedAt = &now
	}
	if errorMsg != "" {
		task.Error = errorMsg
	}

	updatedData, err := json.Marshal(task)
	if err != nil {
		return fmt.Errorf("failed to marshal updated task: %v", err)
	}

	err = rq.client.Set(ctx, taskKey, updatedData, rq.ttl).Err()
	if err != nil {
		return fmt.Errorf("failed to update task result: %v", err)
	}

	if state == TaskStateStopped || state == TaskStateFailed || state == TaskStateTimeout {
		dedupeKey := rq.buildDedupeKey(task.Cmd, task.Channel, task.RequestID)
		rq.client.Del(ctx, dedupeKey)
	}

	log.Printf("Updated task %s state to %s", taskID, state)
	return nil
}

func (rq *RedisQueue) GetTaskStatus(ctx context.Context, taskID string) (*Task, error) {
	taskKey := rq.buildTaskKey(taskID)

	taskData, err := rq.client.Get(ctx, taskKey).Result()
	if err != nil {
		if err == redis.Nil {
			return nil, fmt.Errorf("task %s not found", taskID)
		}
		return nil, fmt.Errorf("failed to get task data: %v", err)
	}

	var task Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return nil, fmt.Errorf("failed to unmarshal task: %v", err)
	}

	return &task, nil
}

func (rq *RedisQueue) Close() error {
	return rq.client.Close()
}

func (rq *RedisQueue) Ping(ctx context.Context) error {
	return rq.client.Ping(ctx).Err()
}

// RenewLease extends the lease for a task being processed by a worker
func (rq *RedisQueue) RenewLease(ctx context.Context, taskID, workerID string) error {
	leaseKey := rq.buildLeaseKey(taskID)
	taskKey := rq.buildTaskKey(taskID)

	// Check if this worker still owns the lease
	currentWorker, err := rq.client.Get(ctx, leaseKey).Result()
	if err != nil {
		if err == redis.Nil {
			return fmt.Errorf("lease for task %s has expired", taskID)
		}
		return fmt.Errorf("failed to check lease: %v", err)
	}

	if currentWorker != workerID {
		return fmt.Errorf("lease for task %s is owned by worker %s, not %s", taskID, currentWorker, workerID)
	}

	// Update task lease expiry and renew Redis lease
	taskData, err := rq.client.Get(ctx, taskKey).Result()
	if err != nil {
		return fmt.Errorf("failed to get task data: %v", err)
	}

	var task Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return fmt.Errorf("failed to unmarshal task: %v", err)
	}

	// Update lease expiry
	newExpiry := time.Now().Add(LeaseTimeout)
	task.LeaseExpiry = &newExpiry

	updatedData, err := json.Marshal(task)
	if err != nil {
		return fmt.Errorf("failed to marshal updated task: %v", err)
	}

	// Atomic renewal of both task data and Redis lease
	pipe := rq.client.TxPipeline()
	pipe.Set(ctx, taskKey, updatedData, rq.ttl)
	pipe.Expire(ctx, leaseKey, LeaseTimeout)
	_, err = pipe.Exec(ctx)
	if err != nil {
		return fmt.Errorf("failed to renew lease: %v", err)
	}

	log.Printf("Renewed lease for task %s by worker %s", taskID, workerID)
	return nil
}

// CleanupExpiredLeases moves tasks from dead workers back to main queues
func (rq *RedisQueue) CleanupExpiredLeases(ctx context.Context) error {
	// Find all processing queues
	processingQueues, err := rq.client.Keys(ctx, "egress:processing:*").Result()
	if err != nil {
		return fmt.Errorf("failed to get processing queues: %v", err)
	}

	for _, processingQueue := range processingQueues {
		// Get all tasks in this processing queue
		taskIDs, err := rq.client.LRange(ctx, processingQueue, 0, -1).Result()
		if err != nil {
			log.Printf("Failed to get tasks from %s: %v", processingQueue, err)
			continue
		}

		for _, taskID := range taskIDs {
			if err := rq.checkAndRecoverTask(ctx, taskID, processingQueue); err != nil {
				log.Printf("Failed to check task %s: %v", taskID, err)
			}
		}
	}

	return nil
}

// checkAndRecoverTask checks if a task lease has expired and recovers it
func (rq *RedisQueue) checkAndRecoverTask(ctx context.Context, taskID, processingQueue string) error {
	leaseKey := rq.buildLeaseKey(taskID)
	taskKey := rq.buildTaskKey(taskID)

	// Check if lease still exists
	_, err := rq.client.Get(ctx, leaseKey).Result()
	if err == nil {
		return nil // Lease still active, worker is alive
	}
	if err != redis.Nil {
		return fmt.Errorf("failed to check lease: %v", err)
	}

	// Lease expired, recover the task
	taskData, err := rq.client.Get(ctx, taskKey).Result()
	if err != nil {
		if err == redis.Nil {
			// Task data gone, just remove from processing queue
			rq.client.LRem(ctx, processingQueue, 1, taskID)
			return nil
		}
		return fmt.Errorf("failed to get task data: %v", err)
	}

	var task Task
	if err := json.Unmarshal([]byte(taskData), &task); err != nil {
		return fmt.Errorf("failed to unmarshal task: %v", err)
	}

	// Only recover PROCESSING or ENQUEUED tasks, not completed ones
	if task.State == TaskStateStopped || task.State == TaskStateFailed || task.State == TaskStateTimeout {
		// Task is already completed, just remove from processing queue
		rq.client.LRem(ctx, processingQueue, 1, taskID)
		log.Printf("Skipped recovery of completed task %s (state: %s)", taskID, task.State)
		return nil
	}

	// Increment retry count and reset state
	task.RetryCount++
	task.State = TaskStateEnqueued
	task.WorkerID = 0 // Reset worker ID to 0
	task.LeaseExpiry = nil
	task.ProcessedAt = nil
	// Reset ENQUEUED timestamp for fresh timeout window
	now := time.Now()
	task.EnqueuedAt = &now

	// Move back to original queue
	originalQueue := rq.buildQueueKey(task.Cmd, task.Channel)

	// Atomic operation: update task + move to original queue + remove from processing
	pipe := rq.client.TxPipeline()

	updatedData, err := json.Marshal(task)
	if err != nil {
		return fmt.Errorf("failed to marshal task: %v", err)
	}

	pipe.Set(ctx, taskKey, updatedData, rq.ttl)
	pipe.LPush(ctx, originalQueue, taskID)
	pipe.LRem(ctx, processingQueue, 1, taskID)
	pipe.Del(ctx, leaseKey) // Clean up expired lease key

	_, err = pipe.Exec(ctx)
	if err != nil {
		return fmt.Errorf("failed to recover task: %v", err)
	}

	log.Printf("Recovered expired task %s (retry %d) from worker, moved back to %s",
		taskID, task.RetryCount, originalQueue)
	return nil
}

// CleanupAgedTasks marks ENQUEUED tasks that have exceeded TaskTimeout as TIMEOUT
func (rq *RedisQueue) CleanupAgedTasks(ctx context.Context) error {
	// Find all main queues (not processing queues - only check ENQUEUED tasks)
	queuePattern := "egress:*:channel:*"
	queues, err := rq.client.Keys(ctx, queuePattern).Result()
	if err != nil {
		return fmt.Errorf("failed to get queues: %v", err)
	}

	for _, queue := range queues {
		// Get all tasks in this queue
		taskIDs, err := rq.client.LRange(ctx, queue, 0, -1).Result()
		if err != nil {
			log.Printf("Failed to get tasks from %s: %v", queue, err)
			continue
		}

		for _, taskID := range taskIDs {
			if err := rq.checkAndTimeoutEnqueuedTask(ctx, taskID, queue); err != nil {
				log.Printf("Failed to check task age %s: %v", taskID, err)
			}
		}
	}

	return nil
}

// checkAndTimeoutEnqueuedTask atomically checks if a ENQUEUED task has exceeded TaskTimeout and marks it as TIMEOUT
func (rq *RedisQueue) checkAndTimeoutEnqueuedTask(ctx context.Context, taskID, queueName string) error {
	taskKey := rq.buildTaskKey(taskID)

	// Atomic operation: check if task is still in queue AND still ENQUEUED, then timeout
	// This prevents race condition where worker picks up task while we're checking timeout
	return rq.client.Watch(ctx, func(tx *redis.Tx) error {
		// Get task data
		taskData, err := tx.Get(ctx, taskKey).Result()
		if err != nil {
			if err == redis.Nil {
				// Task data gone, try to remove from queue (cleanup)
				tx.LRem(ctx, queueName, 1, taskID)
				return nil
			}
			return fmt.Errorf("failed to get task data: %v", err)
		}

		var task Task
		if err := json.Unmarshal([]byte(taskData), &task); err != nil {
			return fmt.Errorf("failed to unmarshal task: %v", err)
		}

		// Only timeout ENQUEUED tasks (not PROCESSING, STOPPING, STOPPED, FAILED, or already TIMEOUT)
		if task.State != TaskStateEnqueued {
			return nil
		}

		// Check if ENQUEUED task has exceeded business timeout (30s since last ENQUEUED)
		var enqueuedTime time.Time
		if task.EnqueuedAt != nil {
			enqueuedTime = *task.EnqueuedAt
		} else {
			// Fallback for old tasks without EnqueuedAt
			enqueuedTime = task.CreatedAt
		}

		taskAge := time.Since(enqueuedTime)
		if taskAge <= TaskTimeout {
			return nil // Task is still fresh
		}

		// Check if task is still in the queue (hasn't been picked up by worker)
		isInQueue, err := tx.LPos(ctx, queueName, taskID, redis.LPosArgs{}).Result()
		if err != nil {
			if err.Error() == "redis: nil" {
				// Task no longer in queue (worker picked it up), don't timeout
				return nil
			}
			return fmt.Errorf("failed to check if task in queue: %v", err)
		}
		_ = isInQueue // Task is in queue

		// Mark task as TIMEOUT and remove from queue atomically
		task.State = TaskStateTimeout
		task.Error = fmt.Sprintf("Task timeout: ENQUEUED task exceeded %v limit (age: %v)", TaskTimeout, taskAge)

		updatedData, err := json.Marshal(task)
		if err != nil {
			return fmt.Errorf("failed to marshal timed out task: %v", err)
		}

		// Execute atomic transaction
		pipe := tx.TxPipeline()
		pipe.Set(ctx, taskKey, updatedData, rq.ttl)
		pipe.LRem(ctx, queueName, 1, taskID) // Remove from queue so it won't be picked up
		_, err = pipe.Exec(ctx)
		if err != nil {
			return fmt.Errorf("failed to timeout task: %v", err)
		}

		log.Printf("Marked ENQUEUED task %s as TIMEOUT (age: %v, limit: %v)", taskID, taskAge, TaskTimeout)
		return nil
	}, taskKey, queueName)
}

// StartCleanupProcess runs background cleanup of expired leases
func (rq *RedisQueue) StartCleanupProcess(ctx context.Context) {
	go func() {
		ticker := time.NewTicker(CleanupInterval)
		defer ticker.Stop()

		for {
			select {
			case <-ctx.Done():
				log.Printf("Stopping Redis cleanup process")
				return
			case <-ticker.C:
				// Handle both business timeouts and worker failure recovery
				if err := rq.CleanupExpiredLeases(ctx); err != nil {
					log.Printf("Lease cleanup error: %v", err)
				}
				if err := rq.CleanupAgedTasks(ctx); err != nil {
					log.Printf("Task timeout cleanup error: %v", err)
				}
			}
		}
	}()
	log.Printf("Started Redis cleanup process with %v interval", CleanupInterval)
}

func (rq *RedisQueue) Client() *redis.Client {
	return rq.client
}

func (rq *RedisQueue) TTL() time.Duration {
	return rq.ttl
}

// DeleteTask completely removes a task from Redis (task data and lease)
func (rq *RedisQueue) DeleteTask(ctx context.Context, taskID string) error {
	taskKey := rq.buildTaskKey(taskID)
	leaseKey := rq.buildLeaseKey(taskID)

	pipe := rq.client.Pipeline()
	pipe.Del(ctx, taskKey)  // Remove task data
	pipe.Del(ctx, leaseKey) // Remove lease data

	_, err := pipe.Exec(ctx)
	if err != nil {
		return fmt.Errorf("failed to delete task %s: %v", taskID, err)
	}

	log.Printf("Deleted task %s from Redis completely", taskID)
	return nil
}
