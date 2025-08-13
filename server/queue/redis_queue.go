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
	TaskStatePending    = "PENDING"
	TaskStateProcessing = "PROCESSING"
	TaskStateSuccess    = "SUCCESS"
	TaskStateFailed     = "FAILED"

	// Lease settings for automatic failover
	LeaseRenewalInterval = 15 * time.Second // How often workers renew lease
	LeaseTimeout         = 45 * time.Second // When to consider worker dead
	CleanupInterval      = 5 * time.Second  // How often to check for expired leases
)

type Task struct {
	ID          string                 `json:"id"`
	Type        string                 `json:"type"`    // "snapshot", "record", "web:record"
	Action      string                 `json:"action"`  // "start", "stop", "status"
	Channel     string                 `json:"channel"` // channel name
	RequestID   string                 `json:"request_id"`
	Payload     map[string]interface{} `json:"payload"`
	State       string                 `json:"state"`
	CreatedAt   time.Time              `json:"created_at"`
	ProcessedAt *time.Time             `json:"processed_at,omitempty"`
	CompletedAt *time.Time             `json:"completed_at,omitempty"`
	Error       string                 `json:"error,omitempty"`
	WorkerID    string                 `json:"worker_id,omitempty"`
	LeaseExpiry *time.Time             `json:"lease_expiry,omitempty"` // When worker lease expires
	RetryCount  int                    `json:"retry_count,omitempty"`  // Number of retry attempts
}

type RedisQueue struct {
	client   *redis.Client
	ttl      time.Duration
	patterns []string
}

func NewRedisQueue(addr, password string, db int, ttl int, patterns []string) *RedisQueue {
	rdb := redis.NewClient(&redis.Options{
		Addr:     addr,
		Password: password,
		DB:       db,
	})

	return &RedisQueue{
		client:   rdb,
		ttl:      time.Duration(ttl) * time.Second,
		patterns: patterns,
	}
}

func (rq *RedisQueue) generateTaskID(taskType, channel, requestID string) string {
	data := fmt.Sprintf("%s:%s:%s:%d", taskType, channel, requestID, time.Now().UnixNano())
	hash := sha256.Sum256([]byte(data))
	return fmt.Sprintf("%x", hash[:8])
}

func (rq *RedisQueue) buildQueueKey(taskType, channel string) string {
	return fmt.Sprintf("egress:%s:channel:%s", taskType, channel)
}

func (rq *RedisQueue) BuildQueueKey(taskType, channel string) string {
	return rq.buildQueueKey(taskType, channel)
}

func (rq *RedisQueue) buildTaskKey(taskID string) string {
	return fmt.Sprintf("egress:task:%s", taskID)
}

func (rq *RedisQueue) BuildTaskKey(taskID string) string {
	return rq.buildTaskKey(taskID)
}

func (rq *RedisQueue) buildDedupeKey(taskType, channel, requestID string) string {
	return fmt.Sprintf("egress:dedupe:%s:%s:%s", taskType, channel, requestID)
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

func (rq *RedisQueue) PublishTask(ctx context.Context, taskType, action, channel, requestID string, payload map[string]interface{}) (*Task, error) {
	dedupeKey := rq.buildDedupeKey(taskType, channel, requestID)

	exists, err := rq.client.Exists(ctx, dedupeKey).Result()
	if err != nil {
		return nil, fmt.Errorf("failed to check duplicate: %v", err)
	}
	if exists > 0 {
		return nil, fmt.Errorf("duplicate task: %s for channel %s with request_id %s", taskType, channel, requestID)
	}

	taskID := rq.generateTaskID(taskType, channel, requestID)
	task := &Task{
		ID:        taskID,
		Type:      taskType,
		Action:    action,
		Channel:   channel,
		RequestID: requestID,
		Payload:   payload,
		State:     TaskStatePending,
		CreatedAt: time.Now(),
	}

	taskData, err := json.Marshal(task)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal task: %v", err)
	}

	pipe := rq.client.TxPipeline()

	queueKey := rq.buildQueueKey(taskType, channel)
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

func (rq *RedisQueue) SubscribeToTasks(ctx context.Context, workerID string, patterns []string, taskChan chan<- *Task) error {
	if len(patterns) == 0 {
		patterns = rq.patterns
	}

	log.Printf("Worker %s subscribing to patterns: %v", workerID, patterns)

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
			task, err := rq.fetchNextTask(ctx, workerID, patterns)
			if err != nil {
				log.Printf("Error fetching task for worker %s: %v", workerID, err)
				time.Sleep(1 * time.Second)
				continue
			}
			if task != nil {
				taskChan <- task
			} else {
				time.Sleep(100 * time.Millisecond)
			}
		}
	}
}

func (rq *RedisQueue) fetchNextTask(ctx context.Context, workerID string, patterns []string) (*Task, error) {
	queues := rq.getMatchingQueues(ctx, patterns)
	if len(queues) == 0 {
		return nil, nil
	}

	processingQueue := rq.buildProcessingQueueKey(workerID)

	// Try BRPOPLPUSH for each queue to atomically move task to processing queue
	for _, queue := range queues {
		taskID, err := rq.client.BRPopLPush(ctx, queue, processingQueue, 1*time.Second).Result()
		if err != nil {
			if err == redis.Nil {
				continue // Try next queue
			}
			return nil, fmt.Errorf("failed to pop from queue %s: %v", queue, err)
		}

		// Successfully got a task, now update its state and create lease
		task, err := rq.claimTaskWithLease(ctx, taskID, workerID)
		if err != nil {
			// If we can't claim the task, push it back to the original queue
			rq.client.LRem(ctx, processingQueue, 1, taskID)
			rq.client.LPush(ctx, queue, taskID)
			return nil, fmt.Errorf("failed to claim task %s: %v", taskID, err)
		}

		log.Printf("Worker %s fetched task %s from queue %s", workerID, taskID, queue)
		return task, nil
	}

	return nil, nil // No tasks available
}

// claimTaskWithLease updates task state and creates a lease
func (rq *RedisQueue) claimTaskWithLease(ctx context.Context, taskID, workerID string) (*Task, error) {
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
	task.WorkerID = workerID
	task.LeaseExpiry = &leaseExpiry

	updatedData, err := json.Marshal(task)
	if err != nil {
		return nil, fmt.Errorf("failed to marshal updated task: %v", err)
	}

	// Atomic update: task data + lease creation
	pipe := rq.client.TxPipeline()
	pipe.Set(ctx, taskKey, updatedData, rq.ttl)
	pipe.Set(ctx, leaseKey, workerID, LeaseTimeout) // Lease expires automatically
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
	task.CompletedAt = &now
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

	if state == TaskStateSuccess || state == TaskStateFailed {
		dedupeKey := rq.buildDedupeKey(task.Type, task.Channel, task.RequestID)
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

	// Increment retry count and reset state
	task.RetryCount++
	task.State = TaskStatePending
	task.WorkerID = ""
	task.LeaseExpiry = nil
	task.ProcessedAt = nil

	// Move back to original queue
	originalQueue := rq.buildQueueKey(task.Type, task.Channel)

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
				if err := rq.CleanupExpiredLeases(ctx); err != nil {
					log.Printf("Cleanup error: %v", err)
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
