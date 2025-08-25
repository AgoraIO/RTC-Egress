package egress

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/health"
	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/gin-gonic/gin"
)

var egressConfigPath string
var requestIdRegexp = regexp.MustCompile(`^[0-9a-zA-Z]{1,32}$`)

// parseRegionFromIP extracts region from IP address (fake implementation for now)
func parseRegionFromIP(ip string) string {
	// TODO: Implement real IP-to-region mapping
	// For now, return empty region (global)
	return ""
}

// Worker states
const (
	WORKER_IDLE = iota
	WORKER_RUNNING
	WORKER_NOT_AVAILABLE
)

type WorkerCommand struct {
	Cmd     string                 `json:"cmd"`
	Action  string                 `json:"action"`
	Payload map[string]interface{} `json:"payload,omitempty"`
}

type WorkerStatus struct {
	ID      int    `json:"id"`
	State   int    `json:"state"` // 0: IDLE, 1: RUNNING, 2: NOT_AVAILABLE
	Pid     int    `json:"pid"`
	LastErr string `json:"last_err,omitempty"`
	TaskID  string `json:"task_id,omitempty"`
}

type Worker struct {
	ID         int
	Cmd        *exec.Cmd
	SocketPath string
	Conn       net.Conn
	Status     WorkerStatus
	Mutex      sync.Mutex
	TaskID     string // Current task ID being processed
}

type TaskRequest struct {
	// RequestID is a mandatory passed by calller,
	// 32 digits(including 0-9, a-z, A-Z) long at most, can not be empty
	RequestID string                 `json:"request_id,omitempty"`
	Cmd       string                 `json:"cmd"`
	Action    string                 `json:"action"`
	TaskID    string                 `json:"task_id,omitempty"`
	Payload   map[string]interface{} `json:"payload,omitempty"`
}

type TaskResponse struct {
	RequestID string `json:"request_id"`
	TaskID    string `json:"task_id"`
	Status    string `json:"status"`
	Error     string `json:"error,omitempty"`
}

type WorkerManager struct {
	Workers        []*Worker
	NumWorkers     int
	BinPath        string
	Mutex          sync.Mutex
	ChannelWorkers map[string]int                 // Maps channel name to worker ID (0,1,2,3)
	WorkerChannels map[int]string                 // Maps worker ID to channel name
	ChannelTasks   map[string]map[string][]string // Maps channel -> task type -> []task_ids
	RedisQueue     *queue.RedisQueue              // Redis queue for task management
	PodID          string                         // Unique pod identifier
	ActiveTasks    map[string]string              // Maps task_id -> pod_id for lease renewal
	TasksMutex     sync.RWMutex                   // Protects ActiveTasks map
	HealthManager  *health.HealthManager          // Health monitoring (optional)
}

// Global worker manager instance for cleanup
var globalWorkerManager *WorkerManager

func NewWorkerManagerWithHealth(binPath string, num int, redisQueue *queue.RedisQueue, podID string, healthManager *health.HealthManager) *WorkerManager {
	wm := &WorkerManager{
		Workers:        make([]*Worker, num),
		NumWorkers:     num,
		BinPath:        binPath,
		ChannelWorkers: make(map[string]int),
		WorkerChannels: make(map[int]string),
		ChannelTasks:   make(map[string]map[string][]string),
		RedisQueue:     redisQueue,
		PodID:          podID,
		ActiveTasks:    make(map[string]string),
		HealthManager:  healthManager,
	}

	// Start health heartbeat if HealthManager is available
	if healthManager != nil {
		healthManager.StartHeartbeat(num, wm.getActiveTaskCount)
	}

	return wm
}

func (wm *WorkerManager) StartAll() {
	for i := 0; i < wm.NumWorkers; i++ {
		wm.startWorker(i)
	}
	// Start monitoring goroutine
	go wm.monitorWorkers()

	// Start Redis task subscriber if Redis is available
	if wm.RedisQueue != nil {
		go wm.startRedisSubscriber()
		// Start cleanup process for automatic failover
		ctx := context.Background()
		wm.RedisQueue.StartCleanupProcess(ctx)
		// Start lease renewal process
		go wm.startLeaseRenewal()
	}
}

func (wm *WorkerManager) startWorker(i int) {
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	// Clean up old socket if exists
	sockPath := fmt.Sprintf("/tmp/agora/eg_worker_%d.sock", i)
	os.Remove(sockPath)
	os.MkdirAll("/tmp/agora", 0755)

	// Manager creates the Unix socket server and listens
	ln, err := net.Listen("unix", sockPath)
	if err != nil {
		log.Printf("Failed to listen on socket %s: %v", sockPath, err)
		wm.Workers[i] = &Worker{
			ID:  i,
			Cmd: nil,
			Status: WorkerStatus{
				ID:      i,
				State:   WORKER_NOT_AVAILABLE,
				LastErr: err.Error(),
			},
		}
		return
	}

	cmd := exec.Command(wm.BinPath, "--config", egressConfigPath, "--sock", sockPath)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	err = cmd.Start()
	if err != nil {
		log.Printf("Failed to start worker %d: %v", i, err)
		ln.Close()
		wm.Workers[i] = &Worker{
			ID:  i,
			Cmd: nil,
			Status: WorkerStatus{
				ID:      i,
				State:   WORKER_NOT_AVAILABLE,
				LastErr: err.Error(),
			},
		}
		return
	}

	// Accept the connection from the worker
	var conn net.Conn
	acceptErr := make(chan error, 1)
	go func() {
		var err error
		conn, err = ln.Accept()
		acceptErr <- err
	}()

	select {
	case err := <-acceptErr:
		ln.Close()
		if err != nil {
			log.Printf("Failed to accept connection from worker %d: %v", i, err)
			cmd.Process.Kill()
			wm.Workers[i] = &Worker{
				ID:  i,
				Cmd: nil,
				Status: WorkerStatus{
					ID:      i,
					State:   WORKER_NOT_AVAILABLE,
					LastErr: "accept failed",
				},
			}
			return
		}
	case <-time.After(5 * time.Second):
		ln.Close()
		log.Printf("Timeout waiting for worker %d to connect to socket", i)
		cmd.Process.Kill()
		wm.Workers[i] = &Worker{
			ID:  i,
			Cmd: nil,
			Status: WorkerStatus{
				ID:      i,
				State:   WORKER_NOT_AVAILABLE,
				LastErr: "timeout waiting for connection",
			},
		}
		return
	}

	wm.Workers[i] = &Worker{
		ID:         i,
		Cmd:        cmd,
		SocketPath: sockPath,
		Conn:       conn,
		Status: WorkerStatus{
			ID:    i,
			State: WORKER_IDLE,
			Pid:   cmd.Process.Pid,
		},
	}

	// Start completion message listener for this worker
	go wm.listenForCompletionMessages(wm.Workers[i])

	log.Printf("Started worker %d (pid %d)", i, cmd.Process.Pid)
}

func (wm *WorkerManager) monitorWorkers() {
	for {
		time.Sleep(2 * time.Second)
		for i, w := range wm.Workers {
			if w == nil || w.Cmd == nil || w.Cmd.Process == nil {
				wm.startWorker(i)
				continue
			}
			if err := w.Cmd.Process.Signal(syscall.Signal(0)); err != nil {
				log.Printf("Worker %d (pid %d) appears dead, will check again in 5s", i, w.Cmd.Process.Pid)

				// Schedule delayed recovery check
				go wm.scheduleWorkerRecovery(i, w)

				if w.Conn != nil {
					w.Conn.Close()
				}
				wm.startWorker(i)
			}
		}
	}
}

func (wm *WorkerManager) findFreeWorker() (*Worker, error) {
	for _, w := range wm.Workers {
		if w != nil && w.Status.State == WORKER_IDLE {
			return w, nil
		}
	}
	return nil, fmt.Errorf("no free workers available")
}

// Cleanup terminates all worker processes gracefully
func (wm *WorkerManager) Cleanup() {
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	log.Printf("Cleaning up %d worker processes...", wm.NumWorkers)

	for i, worker := range wm.Workers {
		if worker != nil && worker.Cmd != nil && worker.Cmd.Process != nil {
			log.Printf("Terminating worker %d (pid %d)", i, worker.Cmd.Process.Pid)

			// Try graceful termination first
			if err := worker.Cmd.Process.Signal(syscall.SIGTERM); err != nil {
				log.Printf("Failed to send SIGTERM to worker %d: %v", i, err)
			} else {
				// Wait a bit for graceful shutdown
				done := make(chan error, 1)
				go func() {
					done <- worker.Cmd.Wait()
				}()

				select {
				case <-done:
					log.Printf("Worker %d terminated gracefully", i)
				case <-time.After(2 * time.Second):
					// Force kill if graceful shutdown takes too long
					log.Printf("Force killing worker %d", i)
					if err := worker.Cmd.Process.Kill(); err != nil {
						log.Printf("Failed to kill worker %d: %v", i, err)
					}
				}
			}

			// Close connection if it exists
			if worker.Conn != nil {
				worker.Conn.Close()
			}
		}
	}

	log.Printf("Worker cleanup completed")
}

// CleanupWorkers provides a global cleanup function
func CleanupWorkers() {
	if globalWorkerManager != nil {
		globalWorkerManager.Cleanup()
	}
}

// listenForCompletionMessages listens for completion messages from C++ worker
func (wm *WorkerManager) listenForCompletionMessages(worker *Worker) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("Completion listener for worker %d panicked: %v", worker.ID, r)
		}
	}()

	for {
		if worker.Conn == nil {
			time.Sleep(1 * time.Second)
			continue
		}

		// Set read timeout to avoid blocking forever
		worker.Conn.SetReadDeadline(time.Now().Add(5 * time.Second))

		// Read completion message from C++ worker
		buffer := make([]byte, 4096)
		n, err := worker.Conn.Read(buffer)
		if err != nil {
			// Check if it's a timeout - not necessarily an error
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				continue
			}
			log.Printf("Error reading from worker %d: %v", worker.ID, err)
			time.Sleep(1 * time.Second)
			continue
		}

		if n > 0 {
			// Handle multiple JSON messages separated by newlines
			data := string(buffer[:n])
			lines := strings.Split(strings.TrimSpace(data), "\n")

			for _, line := range lines {
				line = strings.TrimSpace(line)
				if line == "" {
					continue
				}

				// Try to parse each line as a completion message
				var completion UDSCompletionMessage
				if err := json.Unmarshal([]byte(line), &completion); err != nil {
					log.Printf("Failed to parse completion message from worker %d: %v (line: %s)", worker.ID, err, line)
					continue
				}

				// Handle the completion message
				wm.handleTaskCompletion(worker, &completion)
			}
		}
	}
}

// handleTaskCompletion processes task completion from C++ worker
func (wm *WorkerManager) handleTaskCompletion(worker *Worker, completion *UDSCompletionMessage) {
	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	log.Printf("Worker %d completed task %s with status: %s", worker.ID, completion.TaskID, completion.Status)

	// Validate that this worker was actually processing this task
	if worker.TaskID != completion.TaskID {
		log.Printf("Warning: Worker %d reported completion for task %s but was processing task %s",
			worker.ID, completion.TaskID, worker.TaskID)
		return
	}

	// Check for unrecoverable errors and auto-stop FAILED tasks
	if completion.Status != "success" {
		errorMsg := completion.Error
		if errorMsg == "" {
			errorMsg = completion.Message
		}

		// Detect unrecoverable errors that should mark task as FAILED
		isUnrecoverable := wm.isUnrecoverableError(errorMsg)

		if isUnrecoverable {
			// Get task from Redis to pass to failTask
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()

			task, err := wm.RedisQueue.GetTaskStatus(ctx, completion.TaskID)
			if err != nil {
				log.Printf("Failed to get task for FAILED handling: %v", err)
			} else {
				log.Printf("Detected unrecoverable error for task %s: %s", completion.TaskID, errorMsg)
				// Auto-stop the FAILED task
				if err := wm.failTask(task, errorMsg); err != nil {
					log.Printf("Failed to auto-stop FAILED task %s: %v", completion.TaskID, err)
				}
				return // Exit early as failTask handles Redis update and cleanup
			}
		}
	}

	// Update Redis task status (normal success/failed processing)
	if wm.RedisQueue != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		status := queue.TaskStateSuccess
		errorMsg := ""
		if completion.Status != "success" {
			status = queue.TaskStateFailed
			errorMsg = completion.Error
			if errorMsg == "" {
				errorMsg = completion.Message
			}
		}

		if err := wm.RedisQueue.UpdateTaskResult(ctx, completion.TaskID, status, errorMsg); err != nil {
			log.Printf("Failed to update Redis status for task %s: %v", completion.TaskID, err)
		} else {
			log.Printf("Updated Redis status for task %s: %s", completion.TaskID, status)
		}
	}

	// Update worker status - mark as idle and clear task ID
	worker.Status.State = WORKER_IDLE
	worker.Status.TaskID = ""
	worker.TaskID = ""

	// Remove from active tasks tracking for lease renewal
	wm.TasksMutex.Lock()
	delete(wm.ActiveTasks, completion.TaskID)
	wm.TasksMutex.Unlock()

	// Clean up channel mappings - find and remove this specific task
	for channel, workerID := range wm.ChannelWorkers {
		if workerID == worker.ID {
			// Find the task type for this task ID and remove it
			wm.Mutex.Lock()
			if channelTasks, ok := wm.ChannelTasks[channel]; ok {
				for taskType, taskIDs := range channelTasks {
					for _, taskID := range taskIDs {
						if taskID == completion.TaskID {
							wm.Mutex.Unlock()
							wm.removeTaskFromChannel(channel, taskType, completion.TaskID)
							return
						}
					}
				}
			}
			wm.Mutex.Unlock()
			break
		}
	}
}

// startLeaseRenewal periodically renews leases for active tasks
func (wm *WorkerManager) startLeaseRenewal() {
	ticker := time.NewTicker(queue.LeaseRenewalInterval)
	defer ticker.Stop()

	log.Printf("Started lease renewal process for pod %s", wm.PodID)

	for range ticker.C {
		wm.renewAllLeases()
	}
}

// renewAllLeases renews leases for all active tasks
func (wm *WorkerManager) renewAllLeases() {
	wm.TasksMutex.RLock()
	activeTasks := make(map[string]string)
	for taskID, workerID := range wm.ActiveTasks {
		activeTasks[taskID] = workerID
	}
	wm.TasksMutex.RUnlock()

	if len(activeTasks) == 0 {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	renewedCount := 0
	for taskID, workerID := range activeTasks {
		if err := wm.RedisQueue.RenewLease(ctx, taskID, workerID); err != nil {
			log.Printf("Failed to renew lease for task %s: %v", taskID, err)
			// Remove from active tasks if lease renewal fails
			wm.TasksMutex.Lock()
			delete(wm.ActiveTasks, taskID)
			wm.TasksMutex.Unlock()
		} else {
			renewedCount++
		}
	}

	if renewedCount > 0 {
		log.Printf("Renewed leases for %d tasks", renewedCount)
	}
}

// scheduleWorkerRecovery waits 5 seconds then recovers tasks from dead worker
func (wm *WorkerManager) scheduleWorkerRecovery(workerID int, deadWorker *Worker) {
	// Wait 5 seconds to confirm worker is really dead
	time.Sleep(5 * time.Second)

	// Double-check the worker is still dead and hasn't been replaced
	wm.Mutex.Lock()
	currentWorker := wm.Workers[workerID]
	stillDead := (currentWorker == nil ||
		currentWorker.Cmd == nil ||
		currentWorker.Cmd.Process == nil ||
		currentWorker.Cmd.Process.Pid != deadWorker.Cmd.Process.Pid)
	wm.Mutex.Unlock()

	if stillDead {
		log.Printf("Confirmed worker %d (pid %d) is dead after 5s, recovering tasks",
			workerID, deadWorker.Cmd.Process.Pid)
		wm.recoverTasksFromDeadWorker(deadWorker)
	} else {
		log.Printf("Worker %d recovered on its own, no task recovery needed", workerID)
	}
}

// recoverTasksFromDeadWorker moves tasks from dead worker back to Redis main queues
func (wm *WorkerManager) recoverTasksFromDeadWorker(deadWorker *Worker) {
	if wm.RedisQueue == nil {
		log.Printf("No Redis queue configured, cannot recover tasks from worker %d", deadWorker.ID)
		return
	}

	// Find all tasks assigned to this worker
	var tasksToRecover []string
	wm.TasksMutex.Lock()
	for taskID, workerManagerID := range wm.ActiveTasks {
		if workerManagerID == wm.PodID {
			// This task belongs to our manager, check if it was on the dead worker
			wm.Mutex.Lock()
			for channel, workerID := range wm.ChannelWorkers {
				if workerID == deadWorker.ID {
					// Check if this task is on this channel
					if channelTasks, ok := wm.ChannelTasks[channel]; ok {
						for _, taskIDs := range channelTasks {
							for _, id := range taskIDs {
								if id == taskID {
									tasksToRecover = append(tasksToRecover, taskID)
								}
							}
						}
					}
				}
			}
			wm.Mutex.Unlock()
		}
	}

	// Remove recovered tasks from tracking
	for _, taskID := range tasksToRecover {
		delete(wm.ActiveTasks, taskID)
	}
	wm.TasksMutex.Unlock()

	if len(tasksToRecover) == 0 {
		log.Printf("No tasks to recover from worker %d", deadWorker.ID)
		return
	}

	// Move tasks back to main queues for immediate re-assignment
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	recoveredCount := 0
	for _, taskID := range tasksToRecover {
		if err := wm.moveTaskBackToMainQueue(ctx, taskID); err != nil {
			log.Printf("Failed to recover task %s from dead worker %d: %v", taskID, deadWorker.ID, err)
		} else {
			recoveredCount++
		}
	}

	log.Printf("Recovered %d/%d tasks from dead worker %d", recoveredCount, len(tasksToRecover), deadWorker.ID)
}

// moveTaskBackToMainQueue moves a task from processing back to main queue
func (wm *WorkerManager) moveTaskBackToMainQueue(ctx context.Context, taskID string) error {
	// Get task details
	task, err := wm.RedisQueue.GetTaskStatus(ctx, taskID)
	if err != nil {
		return fmt.Errorf("failed to get task status: %v", err)
	}

	// Reset task state
	task.State = queue.TaskStateEnqueued
	task.WorkerID = 0 // Reset worker ID to 0
	task.LeaseExpiry = nil
	task.ProcessedAt = nil
	task.RetryCount++

	// Update task and move back to main queue
	originalQueue := wm.RedisQueue.BuildQueueKey(task.Type, task.Channel)
	processingQueue := wm.RedisQueue.BuildProcessingQueueKey(wm.PodID)
	taskKey := wm.RedisQueue.BuildTaskKey(taskID)
	leaseKey := wm.RedisQueue.BuildLeaseKey(taskID)

	taskData, err := json.Marshal(task)
	if err != nil {
		return fmt.Errorf("failed to marshal task: %v", err)
	}

	// Atomic operation: update task + move to main queue + cleanup
	pipe := wm.RedisQueue.Client().TxPipeline()
	pipe.Set(ctx, taskKey, taskData, wm.RedisQueue.TTL())
	pipe.LPush(ctx, originalQueue, taskID)
	pipe.LRem(ctx, processingQueue, 1, taskID)
	pipe.Del(ctx, leaseKey)

	_, err = pipe.Exec(ctx)
	if err != nil {
		return fmt.Errorf("failed to move task back to main queue: %v", err)
	}

	log.Printf("Moved task %s back to queue %s (retry %d)", taskID, originalQueue, task.RetryCount)
	return nil
}

// startRedisSubscriber continuously polls Redis for tasks and processes them
func (wm *WorkerManager) startRedisSubscriber() {
	log.Printf("Starting Redis task subscriber for pod %s", wm.PodID)

	for {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)

		// Use regional queue patterns that match our new structure
		patterns := []string{"egress:snapshot:channel:*", "egress:record:channel:*"}
		if wm.RedisQueue != nil {
			// Include web:record pattern for compatibility
			patterns = []string{"egress:snapshot:channel:*", "egress:record:channel:*", "egress:web:record:channel:*"}
		}

		task, err := wm.fetchTaskFromRedis(ctx, patterns)
		cancel()

		if err != nil {
			log.Printf("Error fetching task from Redis: %v", err)
			time.Sleep(1 * time.Second)
			continue
		}

		if task != nil {
			wm.processRedisTask(task)
		} else {
			time.Sleep(100 * time.Millisecond)
		}
	}
}

// fetchTaskFromRedis polls Redis queues for available tasks using regional priority
func (wm *WorkerManager) fetchTaskFromRedis(ctx context.Context, patterns []string) (*queue.Task, error) {
	if wm.RedisQueue == nil {
		return nil, nil
	}

	// Get prioritized queues (regional first, then global)
	queues := wm.getPrioritizedQueues(ctx, patterns)
	if len(queues) == 0 {
		return nil, nil
	}

	processingQueue := wm.RedisQueue.BuildProcessingQueueKey(wm.PodID)

	// Try BRPOPLPUSH for each queue in priority order
	for _, queue := range queues {
		taskID, err := wm.RedisQueue.Client().BRPopLPush(ctx, queue, processingQueue, 1*time.Second).Result()
		if err != nil {
			if err.Error() == "redis: nil" {
				continue // Try next queue
			}
			return nil, fmt.Errorf("failed to pop from queue %s: %v", queue, err)
		}

		// Use proper lease mechanism instead of manual state update
		task, err := wm.RedisQueue.ClaimTaskWithLease(ctx, taskID, wm.PodID)
		if err != nil {
			// If we can't claim the task, push it back to the original queue
			wm.RedisQueue.Client().LRem(ctx, processingQueue, 1, taskID)
			wm.RedisQueue.Client().LPush(ctx, queue, taskID)
			return nil, fmt.Errorf("failed to claim task %s with lease: %v", taskID, err)
		}

		log.Printf("Pod %s fetched task %s from queue %s", wm.PodID, taskID, queue)
		return task, nil
	}

	return nil, nil // No tasks available
}

// getPrioritizedQueues returns queues with regional priority
func (wm *WorkerManager) getPrioritizedQueues(ctx context.Context, patterns []string) []string {
	if wm.RedisQueue == nil {
		return nil
	}

	var regionalQueues []string
	var globalQueues []string

	// Get Redis client and pod region
	client := wm.RedisQueue.Client()
	podRegion := wm.getRedisQueueRegion()

	for _, basePattern := range patterns {
		// Extract task type from pattern like "egress:record:channel:*"
		parts := strings.Split(basePattern, ":")
		if len(parts) < 3 {
			continue
		}
		taskType := parts[1] // "record", "snapshot", etc.

		// If pod has a region, prioritize regional queues first
		if podRegion != "" {
			regionalPattern := fmt.Sprintf("egress:%s:%s:channel:*", podRegion, taskType)
			keys, err := client.Keys(ctx, regionalPattern).Result()
			if err != nil {
				log.Printf("Error getting regional keys for pattern %s: %v", regionalPattern, err)
			} else {
				regionalQueues = append(regionalQueues, keys...)
			}
		}

		// Always check global/no-region queues as fallback
		globalPattern := fmt.Sprintf("egress:%s:channel:*", taskType)
		keys, err := client.Keys(ctx, globalPattern).Result()
		if err != nil {
			log.Printf("Error getting global keys for pattern %s: %v", globalPattern, err)
		} else {
			globalQueues = append(globalQueues, keys...)
		}

		// Also check other regions if pod has no specific region
		if podRegion == "" {
			otherRegionalPattern := fmt.Sprintf("egress:*:%s:channel:*", taskType)
			keys, err := client.Keys(ctx, otherRegionalPattern).Result()
			if err != nil {
				log.Printf("Error getting other regional keys for pattern %s: %v", otherRegionalPattern, err)
			} else {
				// Filter out the basic pattern we already got
				for _, key := range keys {
					if !strings.HasPrefix(key, fmt.Sprintf("egress:%s:channel:", taskType)) {
						regionalQueues = append(regionalQueues, key)
					}
				}
			}
		}
	}

	// Combine with priority: regional first, then global
	var result []string
	uniqueQueues := make(map[string]bool)

	// Add regional queues first (higher priority)
	for _, queue := range regionalQueues {
		if !uniqueQueues[queue] {
			uniqueQueues[queue] = true
			result = append(result, queue)
		}
	}

	// Add global queues second (lower priority)
	for _, queue := range globalQueues {
		if !uniqueQueues[queue] {
			uniqueQueues[queue] = true
			result = append(result, queue)
		}
	}

	return result
}

// getRedisQueueRegion returns the region for this pod from RedisQueue config
func (wm *WorkerManager) getRedisQueueRegion() string {
	if wm.RedisQueue == nil {
		return ""
	}
	return wm.RedisQueue.GetRegion()
}

// processRedisTask processes Redis task directly using queue.Task
func (wm *WorkerManager) processRedisTask(task *queue.Task) {
	log.Printf("Processing Redis task %s (type: %s, channel: %s)", task.ID, task.Type, task.Channel)

	// Validate Redis task before processing
	if err := ValidateRedisTask(task); err != nil {
		log.Printf("Pod %s - Redis task %s validation failed: %v", wm.PodID, task.ID, err)
		// Update task status to failed
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateFailed, fmt.Sprintf("Validation failed: %v", err))
		cancel()
		return
	}

	// Use direct queue.Task processing methods
	var err error

	switch task.Action {
	case "start":
		err = wm.startTask(task)
	case "stop":
		err = wm.stopTask(task)
	case "status":
		err = wm.getTaskStatus(task)
	default:
		err = fmt.Errorf("unknown action: %s", task.Action)
	}

	if task.Action == "stop" {
		// Don't update Redis state or add to ActiveTasks - stopTask already handled this
		return
	}

	// Update task status in Redis
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err != nil {
		log.Printf("Failed to process Redis task %s: %v", task.ID, err)
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateFailed, err.Error())
	} else {
		log.Printf("Successfully processed Redis task %s", task.ID)
		// Task is now being processed - completion will be handled by the completion listener
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateProcessing, "")

		// Track task for lease renewal
		wm.TasksMutex.Lock()
		wm.ActiveTasks[task.ID] = wm.PodID
		wm.TasksMutex.Unlock()
	}
}

// startTask starts a task from Redis queue directly
func (wm *WorkerManager) startTask(task *queue.Task) error {
	// Create UDS message directly from task
	udsMsg := &UDSMessage{
		TaskID:  task.ID,
		Cmd:     task.Type,
		Action:  task.Action,
		Channel: task.Channel,
		Uid:     []string{}, // Initialize as empty slice to avoid null JSON serialization
	}

	// Extract fields from task payload
	if task.Payload != nil {
		if layout, ok := task.Payload["layout"].(string); ok {
			udsMsg.Layout = layout
		}
		if uid, ok := task.Payload["uid"].([]interface{}); ok {
			for _, u := range uid {
				if s, ok := u.(string); ok {
					udsMsg.Uid = append(udsMsg.Uid, s)
				}
			}
		}
		if accessToken, ok := task.Payload["access_token"].(string); ok {
			udsMsg.AccessToken = accessToken
		}
		if workerUid, ok := task.Payload["workerUid"].(float64); ok {
			udsMsg.WorkerUid = int(workerUid)
		}
		if interval, ok := task.Payload["interval_in_ms"].(float64); ok {
			udsMsg.IntervalInMs = int(interval)
		}
	}

	channel := task.Channel
	taskType := task.Type

	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	// Find worker for this channel (reuse if exists, find free if new)
	worker, err, _ := wm.findWorkerForChannel(channel, taskType)
	if err != nil {
		return err
	}

	// Update channel mappings
	wm.ChannelWorkers[channel] = worker.ID
	wm.WorkerChannels[worker.ID] = channel
	if wm.ChannelTasks[channel] == nil {
		wm.ChannelTasks[channel] = make(map[string][]string)
	}
	wm.ChannelTasks[channel][taskType] = append(wm.ChannelTasks[channel][taskType], task.ID)

	// Send task to worker
	worker.Mutex.Lock()
	err = wm.sendUDSMessageToWorker(worker, task.ID, udsMsg)
	if err != nil {
		worker.Mutex.Unlock()
		// Clean up on failure
		wm.removeTaskFromChannel(channel, taskType, task.ID)
		return fmt.Errorf("failed to send task to worker: %v", err)
	}

	// Update worker state
	worker.Status.State = WORKER_RUNNING
	worker.Status.TaskID = task.ID
	worker.TaskID = task.ID
	worker.Mutex.Unlock()

	log.Printf("Pod %s assigned task %s (%s:%s) to worker %d", wm.PodID, task.ID, channel, taskType, worker.ID)

	// Update worker ID in Redis for this task
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	wm.RedisQueue.UpdateTaskWorker(ctx, task.ID, worker.ID)

	return nil
}

// stopTask stops a task from Redis queue directly - always succeeds
// Returns a special error to prevent post-processing
func (wm *WorkerManager) stopTask(task *queue.Task) error {
	// 1. Extract original task ID from stop task payload
	var originalTaskID string
	if task.Payload != nil {
		if taskID, ok := task.Payload["task_id"].(string); ok {
			originalTaskID = taskID
		}
	}

	if originalTaskID == "" {
		return fmt.Errorf("stop task %s missing original task_id in payload", task.ID)
	}

	// Always mark stop task as SUCCESS at the end
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		// Mark stop task as SUCCESS - stop operation always succeeds
		err := wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateSuccess, "Stop command executed")
		if err != nil {
			log.Printf("Failed to update stop task %s state to SUCCESS: %v", task.ID, err)
		}

		// Remove stop task from lease renewal
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, task.ID)
		wm.TasksMutex.Unlock()
	}()

	// 2. Find which worker is running the original task
	var targetWorker *Worker
	var targetWorkerID int
	wm.Mutex.Lock()
	for workerID, worker := range wm.Workers {
		if worker != nil && worker.TaskID == originalTaskID {
			targetWorker = worker
			targetWorkerID = workerID
			break
		}
	}
	wm.Mutex.Unlock()

	if targetWorker == nil {
		// Original task may have already completed or not found
		log.Printf("Original task %s not found on any worker for stop task %s", originalTaskID, task.ID)

		// Clean up original task from lease renewal anyway
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, originalTaskID)
		wm.TasksMutex.Unlock()

		// Mark original task as SUCCESS (may already be completed)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		err := wm.RedisQueue.UpdateTaskResult(ctx, originalTaskID, queue.TaskStateSuccess, "Task already completed or not found")
		if err != nil {
			log.Printf("Failed to update original task %s state: %v", originalTaskID, err)
		}

		return nil
	}

	// 3. Try to stop original task gracefully with retries
	stopped := wm.stopOriginalTaskWithRetries(targetWorker, targetWorkerID, originalTaskID, task)

	if stopped {
		log.Printf("Pod %s successfully stopped task %s on worker %d (via stop task %s)", wm.PodID, originalTaskID, targetWorkerID, task.ID)
	} else {
		log.Printf("Pod %s force killed worker %d for task %s (via stop task %s)", wm.PodID, targetWorkerID, originalTaskID, task.ID)
	}

	return nil
}

// stopOriginalTaskWithRetries attempts graceful stop with retries, then force kill
func (wm *WorkerManager) stopOriginalTaskWithRetries(targetWorker *Worker, targetWorkerID int, originalTaskID string, stopTask *queue.Task) bool {
	// Create UDS message to stop the original task
	udsMsg := &UDSMessage{
		TaskID:  originalTaskID,
		Cmd:     stopTask.Type,
		Action:  "stop",
		Channel: stopTask.Channel,
		Uid:     []string{},
	}

	// Try graceful stop with retries (3 attempts with exponential backoff)
	maxRetries := 3
	baseDelay := 1 * time.Second

	for attempt := 1; attempt <= maxRetries; attempt++ {
		targetWorker.Mutex.Lock()
		err := wm.sendUDSMessageToWorker(targetWorker, originalTaskID, udsMsg)
		targetWorker.Mutex.Unlock()

		if err == nil {
			// Success - clean up and mark as completed
			wm.cleanupStoppedTask(stopTask.Channel, stopTask.Type, originalTaskID)
			return true
		}

		log.Printf("Stop attempt %d/%d failed for task %s on worker %d: %v", attempt, maxRetries, originalTaskID, targetWorkerID, err)

		if attempt < maxRetries {
			// Exponential backoff: 1s, 2s, 4s
			delay := time.Duration(attempt) * baseDelay
			time.Sleep(delay)
		}
	}

	// All graceful attempts failed - force kill worker
	log.Printf("Graceful stop failed after %d attempts, force killing worker %d for task %s", maxRetries, targetWorkerID, originalTaskID)
	wm.forceKillWorker(targetWorkerID, originalTaskID, stopTask.Channel, stopTask.Type)

	return false
}

// cleanupStoppedTask removes task from all tracking and marks as SUCCESS
func (wm *WorkerManager) cleanupStoppedTask(channel, taskType, taskID string) {
	// Remove from channel tracking
	wm.removeTaskFromChannel(channel, taskType, taskID)

	// Remove from active tasks tracking (stop lease renewal)
	wm.TasksMutex.Lock()
	delete(wm.ActiveTasks, taskID)
	wm.TasksMutex.Unlock()

	// Mark original task as SUCCESS and remove from Redis completely
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	err := wm.RedisQueue.UpdateTaskResult(ctx, taskID, queue.TaskStateSuccess, "Stopped by user")
	if err != nil {
		log.Printf("Failed to update task %s state to SUCCESS: %v", taskID, err)
	}

}

// forceKillWorker kills worker process and cleans up, WorkerManager handles recovery
func (wm *WorkerManager) forceKillWorker(workerID int, taskID, channel, taskType string) {
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	worker := wm.Workers[workerID]
	if worker == nil {
		log.Printf("Worker %d not found for force kill", workerID)
		return
	}

	// Gracefully terminate worker process first, then force kill if needed
	if worker.Cmd != nil && worker.Cmd.Process != nil {
		// Try SIGTERM first (graceful shutdown)
		err := worker.Cmd.Process.Signal(os.Interrupt)
		if err != nil {
			log.Printf("Failed to send SIGTERM to worker %d: %v", workerID, err)
		} else {
			log.Printf("Sent SIGTERM to worker %d, waiting 3 seconds...", workerID)

			// Wait 3 seconds for graceful shutdown
			time.Sleep(3 * time.Second)

			// Check if process is still running
			if worker.Cmd.ProcessState == nil {
				// Still running, force kill with SIGKILL
				log.Printf("Worker %d still running after SIGTERM, force killing...", workerID)
				err = worker.Cmd.Process.Kill()
				if err != nil {
					log.Printf("Failed to force kill worker %d process: %v", workerID, err)
				} else {
					log.Printf("Force killed worker %d process", workerID)
				}
			} else {
				log.Printf("Worker %d terminated gracefully", workerID)
			}
		}
	}

	// Clean up the stopped task
	wm.cleanupStoppedTask(channel, taskType, taskID)

	// Mark worker as dead - WorkerManager will handle respawning
	worker.Status.State = WORKER_NOT_AVAILABLE
	worker.TaskID = ""
	worker.Cmd = nil

	// Remove from channel mappings
	delete(wm.ChannelWorkers, channel)
	delete(wm.WorkerChannels, workerID)

	log.Printf("Worker %d marked as NOT_AVAILABLE, WorkerManager will handle recovery", workerID)
}

// failTask stops a task due to unrecoverable error and marks it as FAILED
func (wm *WorkerManager) failTask(task *queue.Task, reason string) error {
	// Protection against multiple concurrent failTask calls for the same task
	failKey := fmt.Sprintf("failing:%s", task.ID)
	wm.TasksMutex.Lock()
	if _, exists := wm.ActiveTasks[failKey]; exists {
		wm.TasksMutex.Unlock()
		log.Printf("Task %s is already being failed by another process, skipping", task.ID)
		return nil
	}
	wm.ActiveTasks[failKey] = "failing" // Mark as being failed
	wm.TasksMutex.Unlock()

	// Cleanup when function exits
	defer func() {
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, failKey)
		wm.TasksMutex.Unlock()
	}()

	// Find worker for this task
	workerID := task.WorkerID
	if workerID < 0 || workerID >= len(wm.Workers) {
		// Task not assigned to worker, just mark as FAILED
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateFailed, reason)

		// Remove from active tasks tracking for lease renewal
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, task.ID)
		wm.TasksMutex.Unlock()

		log.Printf("Marked unassigned task %s as FAILED: %s", task.ID, reason)
		return nil
	}

	worker := wm.Workers[workerID]
	if worker == nil {
		// Worker gone, just mark as FAILED
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateFailed, reason)

		// Remove from active tasks tracking for lease renewal
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, task.ID)
		wm.TasksMutex.Unlock()

		log.Printf("Marked task %s as FAILED (worker gone): %s", task.ID, reason)
		return nil
	}

	// Send stop command to worker first
	udsMsg := &UDSMessage{
		TaskID:  task.ID,
		Cmd:     task.Type,
		Action:  "stop",
		Channel: task.Channel,
		Uid:     []string{}, // Initialize as empty slice to avoid null JSON serialization
	}

	// NOTE: Worker mutex is already locked by the caller (handleTaskCompletion)
	// Do not lock again to avoid deadlock
	// Try to send stop command to worker (best effort)
	log.Printf("Sending stop command to worker %d for FAILED task %s (channel: %s, type: %s)", workerID, task.ID, task.Channel, task.Type)
	err := wm.sendUDSMessageToWorker(worker, task.ID, udsMsg)
	if err != nil {
		log.Printf("Failed to send stop command to worker %d for FAILED task %s: %v", workerID, task.ID, err)
		// Continue anyway - mark as FAILED even if stop failed
	} else {
		log.Printf("Successfully sent stop command to worker %d for FAILED task %s", workerID, task.ID)
	}

	// Clean up worker state
	worker.Status.TaskID = ""

	// Clean up task from channel tracking
	wm.removeTaskFromChannel(task.Channel, task.Type, task.ID)

	// Mark task as FAILED in Redis and clean up
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Use existing queue method to mark task as FAILED
	err = wm.RedisQueue.UpdateTaskResult(ctx, task.ID, "FAILED", reason)
	if err != nil {
		log.Printf("Warning: Failed to mark task %s as FAILED in Redis: %v", task.ID, err)
	} else {
		log.Printf("Marked task %s as FAILED in Redis", task.ID)
	}

	// Remove task from processing queue to prevent recovery
	processingQueue := wm.RedisQueue.BuildProcessingQueueKey(wm.PodID)
	removed, err := wm.RedisQueue.Client().LRem(ctx, processingQueue, -1, task.ID).Result()
	if err != nil {
		log.Printf("Warning: Failed to remove FAILED task %s from processing queue: %v", task.ID, err)
	} else if removed > 0 {
		log.Printf("Removed %d instances of FAILED task %s from processing queue", removed, task.ID)
	}

	// Remove lease to prevent lease renewal
	leaseKey := wm.RedisQueue.BuildLeaseKey(task.ID)
	err = wm.RedisQueue.Client().Del(ctx, leaseKey).Err()
	if err != nil {
		log.Printf("Warning: Failed to remove lease for FAILED task %s: %v", task.ID, err)
	}

	// Remove from local active tasks tracking (always do this)
	wm.TasksMutex.Lock()
	delete(wm.ActiveTasks, task.ID)
	wm.TasksMutex.Unlock()

	log.Printf("Pod %s marked task %s (%s:%s) as FAILED and stopped worker %d: %s",
		wm.PodID, task.ID, task.Channel, task.Type, workerID, reason)
	return nil
}

// isUnrecoverableError detects error messages that indicate unrecoverable failures
// These are errors that cannot be resolved by retrying and should mark tasks as FAILED
func (wm *WorkerManager) isUnrecoverableError(errorMsg string) bool {
	if errorMsg == "" {
		return false
	}

	errorLower := strings.ToLower(errorMsg)

	// Authentication and permission errors (including SDK-specific messages)
	if strings.Contains(errorLower, "access_token") ||
		strings.Contains(errorLower, "authentication") ||
		strings.Contains(errorLower, "unauthorized") ||
		strings.Contains(errorLower, "permission denied") ||
		strings.Contains(errorLower, "token") && strings.Contains(errorLower, "invalid") ||
		strings.Contains(errorLower, "token") && strings.Contains(errorLower, "expired") ||
		strings.Contains(errorLower, "access_token_expired") ||
		strings.Contains(errorLower, "invalid token") ||
		strings.Contains(errorLower, "token expired") {
		return true
	}

	// RTC channel and connection errors (usually unrecoverable)
	if strings.Contains(errorLower, "channel") && strings.Contains(errorLower, "join") && strings.Contains(errorLower, "failed") ||
		strings.Contains(errorLower, "rtc") && strings.Contains(errorLower, "connect") && strings.Contains(errorLower, "failed") ||
		strings.Contains(errorLower, "invalid") && strings.Contains(errorLower, "channel") ||
		strings.Contains(errorLower, "connection_failure") ||
		strings.Contains(errorLower, "connection_lost") ||
		strings.Contains(errorLower, "rtc connection") && strings.Contains(errorLower, "failed") ||
		strings.Contains(errorLower, "rtc connection lost") ||
		strings.Contains(errorLower, "banned by server") ||
		strings.Contains(errorLower, "rejected by server") ||
		strings.Contains(errorLower, "invalid app id") ||
		strings.Contains(errorLower, "invalid channel name") ||
		strings.Contains(errorLower, "too many broadcasters") ||
		strings.Contains(errorLower, "same uid login") {
		return true
	}

	// Resource exhaustion errors
	if strings.Contains(errorLower, "no space left") ||
		strings.Contains(errorLower, "disk full") ||
		strings.Contains(errorLower, "storage") && strings.Contains(errorLower, "full") ||
		strings.Contains(errorLower, "out of disk") ||
		strings.Contains(errorLower, "insufficient") && strings.Contains(errorLower, "space") {
		return true
	}

	// Configuration and initialization errors
	if strings.Contains(errorLower, "config") && strings.Contains(errorLower, "invalid") ||
		strings.Contains(errorLower, "initialization") && strings.Contains(errorLower, "failed") ||
		strings.Contains(errorLower, "codec") && strings.Contains(errorLower, "not supported") {
		return true
	}

	// SDK and library errors
	if strings.Contains(errorLower, "sdk") && strings.Contains(errorLower, "error") ||
		strings.Contains(errorLower, "agora") && strings.Contains(errorLower, "error") ||
		strings.Contains(errorLower, "sdk error:") ||
		strings.Contains(errorLower, "task failed due to unrecoverable sdk error") ||
		strings.Contains(errorLower, "failed to create agora service") ||
		strings.Contains(errorLower, "failed to create media node factory") ||
		strings.Contains(errorLower, "failed to create video mixer") {
		return true
	}

	return false
}

// getTaskStatus gets status of a task from Redis queue directly
func (wm *WorkerManager) getTaskStatus(task *queue.Task) error {
	udsMsg := &UDSMessage{
		TaskID:  task.ID,
		Cmd:     task.Type,
		Action:  task.Action,
		Channel: task.Channel,
		Uid:     []string{}, // Initialize as empty slice to avoid null JSON serialization
	}

	// Find worker for this task
	workerID := task.WorkerID
	if workerID < 0 || workerID >= len(wm.Workers) {
		return fmt.Errorf("invalid worker ID %d", workerID)
	}

	worker := wm.Workers[workerID]
	if worker == nil {
		return fmt.Errorf("worker %d not found", workerID)
	}

	if worker.Status.TaskID != task.ID {
		return fmt.Errorf("task %s not found on worker %d", task.ID, workerID)
	}

	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	err := wm.sendUDSMessageToWorker(worker, task.ID, udsMsg)
	if err != nil {
		return fmt.Errorf("failed to send status command to worker %d: %v", workerID, err)
	}

	log.Printf("Pod %s get status of task %s on worker %d", wm.PodID, task.ID, workerID)
	return nil
}

// findWorkerForChannel finds worker for channel (reuse existing or find free)
func (wm *WorkerManager) findWorkerForChannel(channel, taskType string) (*Worker, error, bool) {
	// Check if channel already has a worker assigned
	if workerID, exists := wm.ChannelWorkers[channel]; exists {
		worker := wm.Workers[workerID]
		if worker != nil && worker.Status.State != WORKER_NOT_AVAILABLE {
			return worker, nil, false // Reuse existing worker
		}
		// Worker is dead, clean up
		delete(wm.ChannelWorkers, channel)
		delete(wm.WorkerChannels, workerID)
		delete(wm.ChannelTasks, channel)
	}

	// Find free worker
	worker, err := wm.findFreeWorker()
	return worker, err, false
}

// removeTaskFromChannel removes a specific task from channel tracking
func (wm *WorkerManager) removeTaskFromChannel(channel, taskType, taskId string) {
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	if channelTasks, ok := wm.ChannelTasks[channel]; ok {
		if taskIDs, typeExists := channelTasks[taskType]; typeExists {
			// Remove specific task ID
			for i, id := range taskIDs {
				if id == taskId {
					wm.ChannelTasks[channel][taskType] = append(taskIDs[:i], taskIDs[i+1:]...)
					break
				}
			}
			// Clean up empty task type
			if len(wm.ChannelTasks[channel][taskType]) == 0 {
				delete(wm.ChannelTasks[channel], taskType)
			}
		}
		// Clean up empty channel
		if len(channelTasks) == 0 {
			delete(wm.ChannelTasks, channel)
			delete(wm.ChannelWorkers, channel)
			for workerID, ch := range wm.WorkerChannels {
				if ch == channel {
					delete(wm.WorkerChannels, workerID)
					break
				}
			}
		}
	}
}

// Helper to generate 11 random digits as a string
// including 0-9, a-z, A-Z
func randomDigits(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	digits := make([]byte, n)
	for i := 0; i < n; i++ {
		digits[i] = '0' + (b[i] % 36)
		if digits[i] > '9' {
			digits[i] = 'a' + (digits[i] - '9' - 1)
		}
	}
	return string(digits)
}

// Helper to create WorkerCommand with all required protocol fields
func NewWorkerCommand(cmd string, action string, payload map[string]interface{}) WorkerCommand {
	// The payload should already have defaults applied by validateTaskRequest
	return WorkerCommand{
		Cmd:     cmd,
		Action:  action,
		Payload: payload,
	}
}

func (wm *WorkerManager) StartTaskOnWorker(taskReq TaskRequest) (*TaskResponse, error) {
	// 1. Validate and get channel info
	udsMsg, err := ValidateAndFlattenTaskRequest(&taskReq)
	if err != nil {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	channel := udsMsg.Channel
	taskType := taskReq.Cmd

	wm.Mutex.Lock()
	// 2. Find worker for this channel (reuse if exists, find free if new)
	worker, err, _ := wm.findWorkerForChannel(channel, taskType)
	if err != nil {
		wm.Mutex.Unlock()
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// 3. Use existing task ID or generate new one for HTTP requests
	var taskId string
	if taskReq.TaskID != "" {
		// Use existing taskID from request (recovery case)
		taskId = taskReq.TaskID
	} else {
		// Generate new taskID for new HTTP requests
		taskId = fmt.Sprintf("%s%d", randomDigits(11), worker.ID)
	}

	// 4. Update channel mappings (before sending to avoid race conditions)
	wm.ChannelWorkers[channel] = worker.ID
	wm.WorkerChannels[worker.ID] = channel
	if wm.ChannelTasks[channel] == nil {
		wm.ChannelTasks[channel] = make(map[string][]string)
	}
	wm.ChannelTasks[channel][taskType] = append(wm.ChannelTasks[channel][taskType], taskId)
	wm.Mutex.Unlock()

	// 5. Send task to worker
	worker.Mutex.Lock()
	err = wm.sendUDSMessageToWorker(worker, taskId, udsMsg)
	if err != nil {
		worker.Mutex.Unlock()
		// Clean up on failure
		wm.removeTaskFromChannel(channel, taskType, taskId)
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskId,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// 6. Update worker state
	worker.Status.State = WORKER_RUNNING
	worker.Status.TaskID = taskId
	worker.TaskID = taskId
	worker.Mutex.Unlock()

	log.Printf("Pod %s assigned task %s (%s:%s) to worker %d", wm.PodID, taskId, channel, taskType, worker.ID)
	return &TaskResponse{
		RequestID: taskReq.RequestID,
		TaskID:    taskId,
		Status:    "success",
	}, nil
}

func (wm *WorkerManager) StopTaskOnWorker(workerID int, taskReq TaskRequest) (*TaskResponse, error) {
	// 1. Find and validate worker
	wm.Mutex.Lock()
	worker := wm.Workers[workerID]
	wm.Mutex.Unlock()

	if worker == nil || worker.Status.TaskID != taskReq.TaskID {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     fmt.Sprintf("task %s not found on worker %d", taskReq.TaskID, workerID),
		}, nil
	}

	// 2. Validate and send stop command
	udsMsg, err := ValidateAndFlattenTaskRequest(&taskReq)
	if err != nil {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	worker.Mutex.Lock()
	err = wm.sendUDSMessageToWorker(worker, taskReq.TaskID, udsMsg)
	if err != nil {
		worker.Mutex.Unlock()
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// 3. Update worker state
	worker.Status.TaskID = ""
	worker.Mutex.Unlock()

	// 4. Clean up channel tracking
	wm.removeTaskFromChannel(udsMsg.Channel, taskReq.Cmd, taskReq.TaskID)

	log.Printf("Pod %s released task %s (%s:%s) from worker %d", wm.PodID, taskReq.TaskID, udsMsg.Channel, taskReq.Cmd, workerID)
	return &TaskResponse{
		RequestID: taskReq.RequestID,
		TaskID:    taskReq.TaskID,
		Status:    "success",
	}, nil
}

func (wm *WorkerManager) GetTaskStatusOnWorker(workerID int, taskReq TaskRequest) (*TaskResponse, error) {
	wm.Mutex.Lock()
	worker := wm.Workers[workerID]
	wm.Mutex.Unlock()

	if worker == nil {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     fmt.Sprintf("worker %d not found", workerID),
		}, fmt.Errorf("worker %d not found", workerID)
	}

	if worker.Status.TaskID != taskReq.TaskID {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     fmt.Sprintf("taskId %s not found on worker %d", taskReq.TaskID, workerID),
		}, fmt.Errorf("taskId %s not found on worker %d", taskReq.TaskID, workerID)
	}

	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	// Validate and flatten the task request
	udsMsg, err := ValidateAndFlattenTaskRequest(&taskReq)
	if err != nil {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     fmt.Sprintf("failed to validate and flatten task request: %v", err),
		}, fmt.Errorf("failed to validate and flatten task request: %v", err)
	}

	// Send the flattened UDSMessage to worker
	err = wm.sendUDSMessageToWorker(worker, taskReq.TaskID, udsMsg)
	if err != nil {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     fmt.Sprintf("failed to send status command to worker %d: %v", workerID, err),
		}, fmt.Errorf("failed to send status command to worker %d: %v", workerID, err)
	}

	log.Printf("Pod %s get status of task %s on worker %d", wm.PodID, taskReq.TaskID, workerID)

	return &TaskResponse{
		RequestID: taskReq.RequestID,
		TaskID:    taskReq.TaskID,
		Status:    "success",
	}, nil
}

// sendUDSMessageToWorker sends a flattened UDSMessage directly to a worker
func (wm *WorkerManager) sendUDSMessageToWorker(worker *Worker, taskID string, udsMsg *UDSMessage) error {
	if worker == nil || worker.Conn == nil {
		return fmt.Errorf("worker not running")
	}

	// Set the task ID in the UDSMessage
	udsMsg.TaskID = taskID

	// Send the message to worker
	data, err := json.Marshal(udsMsg)
	if err != nil {
		return fmt.Errorf("failed to marshal UDSMessage for worker %v: %v", worker, err)
	}

	data = append(data, '\n')
	_, err = worker.Conn.Write(data)
	if err != nil {
		return fmt.Errorf("failed to send command to worker %v: %v", worker, err)
	}

	return nil
}

func ManagerMainWithRedisAndHealth(redisQueue *queue.RedisQueue, healthManager *health.HealthManager, numWorkers int) {
	binPath := "./bin/eg_worker" // Adjust if needed
	egressConfigPath = ""
	for i, arg := range os.Args {
		if arg == "--config" && i+1 < len(os.Args) {
			egressConfigPath = os.Args[i+1]
		}
	}
	if egressConfigPath == "" {
		log.Fatal("--config must be provided for egress processes")
	}

	// Generate unique 12-character pod ID
	podID := utils.GenerateRandomID(12)

	wm := NewWorkerManagerWithHealth(binPath, numWorkers, redisQueue, podID, healthManager)
	globalWorkerManager = wm // Store for cleanup
	wm.StartAll()

	r := gin.Default()

	// New API endpoint for automatic task start via Redis
	r.POST("/egress/v1/task/start", func(c *gin.Context) {
		var taskReq TaskRequest
		if err := c.ShouldBindJSON(&taskReq); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error(), "request_id": taskReq.RequestID})
			return
		}

		// Set action to "start" since this is the start endpoint
		taskReq.Action = "start"

		// Validate request parameters before Redis publishing
		if err := ValidateStartTaskRequest(&taskReq); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{
				"error":      fmt.Sprintf("validation failed: %v", err),
				"request_id": taskReq.RequestID,
			})
			return
		}

		// Redis is required for task management
		if wm.RedisQueue == nil {
			c.JSON(http.StatusInternalServerError, gin.H{
				"error":      "Redis queue not configured",
				"request_id": taskReq.RequestID,
			})
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		// Extract channel from payload
		channel := "default"
		if taskReq.Payload != nil {
			if ch, ok := taskReq.Payload["channel"].(string); ok && ch != "" {
				channel = ch
			}
		}

		// Parse region from client IP
		clientIP := c.ClientIP()
		region := parseRegionFromIP(clientIP)
		log.Printf("Client IP: %s, Parsed region: %s", clientIP, region)

		task, err := wm.RedisQueue.PublishTaskToRegion(ctx, taskReq.Cmd, "start", channel, taskReq.RequestID, taskReq.Payload, region)
		if err != nil {
			c.JSON(http.StatusInternalServerError, gin.H{
				"error":      err.Error(),
				"request_id": taskReq.RequestID,
			})
			return
		}

		c.JSON(http.StatusOK, gin.H{
			"request_id": taskReq.RequestID,
			"task_id":    task.ID,
			"status":     "enqueued",
		})
	})

	// API endpoint to stop a worker from a task
	r.POST("/egress/v1/task/stop", func(c *gin.Context) {
		var req struct {
			RequestID string `json:"request_id"`
			TaskID    string `json:"task_id"`
		}
		if err := c.ShouldBindJSON(&req); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}

		// Validate request parameters
		if err := ValidateStopTaskRequest(req.RequestID, req.TaskID); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{
				"error":      fmt.Sprintf("validation failed: %v", err),
				"request_id": req.RequestID,
			})
			return
		}

		// Redis is required for task management
		if wm.RedisQueue == nil {
			c.JSON(http.StatusInternalServerError, gin.H{
				"error":      "Redis queue not configured",
				"request_id": req.RequestID,
			})
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		// Get the existing task
		task, err := wm.RedisQueue.GetTaskStatus(ctx, req.TaskID)
		if err != nil {
			c.JSON(http.StatusOK, gin.H{
				"request_id": req.RequestID,
				"task_id":    req.TaskID,
				"status":     "rejected",
				"error":      fmt.Sprintf("Task %s not found", req.TaskID),
			})
			return
		}

		// Check if task is already completed
		if task.State == queue.TaskStateSuccess || task.State == queue.TaskStateFailed || task.State == queue.TaskStateTimeout {
			c.JSON(http.StatusOK, gin.H{
				"request_id": req.RequestID,
				"task_id":    req.TaskID,
				"status":     "completed",
				"message":    fmt.Sprintf("Task already in state: %s", task.State),
			})
			return
		}

		// Create stop task in Redis with original task_id in payload
		stopPayload := map[string]interface{}{
			"task_id": req.TaskID, // Include the original task_id so worker can find it
		}

		// Parse region from client IP for stop task as well
		clientIP := c.ClientIP()
		region := parseRegionFromIP(clientIP)
		log.Printf("Stop task - Client IP: %s, Parsed region: %s", clientIP, region)

		stopTask, err := wm.RedisQueue.PublishTaskToRegion(ctx, task.Type, "stop", task.Channel, req.RequestID, stopPayload, region)
		if err != nil {
			c.JSON(http.StatusInternalServerError, gin.H{
				"error":      fmt.Sprintf("Failed to create stop task: %v", err),
				"request_id": req.RequestID,
				"task_id":    req.TaskID,
			})
			return
		}

		log.Printf("Created stop task %s for original task %s", stopTask.ID, req.TaskID)

		c.JSON(http.StatusOK, gin.H{
			"request_id": req.RequestID,
			"task_id":    stopTask.ID,
			"status":     "enqueued",
		})
	})

	// API endpoint for task status (checks Redis first, then worker status)
	r.POST("/egress/v1/task/status", func(c *gin.Context) {
		var req struct {
			RequestID string `json:"request_id"`
			TaskID    string `json:"task_id"`
		}

		if err := c.ShouldBindJSON(&req); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}

		// Validate request parameters
		if err := ValidateStatusTaskRequest(req.RequestID, req.TaskID); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{
				"error":      fmt.Sprintf("validation failed: %v", err),
				"request_id": req.RequestID,
			})
			return
		}

		// Redis is required for task management
		if wm.RedisQueue == nil {
			c.JSON(http.StatusInternalServerError, gin.H{
				"error":      "Redis queue not configured",
				"request_id": req.RequestID,
			})
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		task, err := wm.RedisQueue.GetTaskStatus(ctx, req.TaskID)
		if err != nil {
			c.JSON(http.StatusNotFound, gin.H{
				"error":      err.Error(),
				"request_id": req.RequestID,
			})
			return
		}

		c.JSON(http.StatusOK, gin.H{
			"request_id": req.RequestID,
			"task_id":    task.ID,
			"state":      task.State,
			"action":     task.Action,
			"created_at": task.CreatedAt,
			"error":      task.Error,
			"worker_id":  task.WorkerID,
		})
	})

	log.Println("Manager API server running on :8090")
	r.Run(":8090")
}

// getActiveTaskCount returns the number of currently active tasks
func (wm *WorkerManager) getActiveTaskCount() int {
	wm.TasksMutex.RLock()
	defer wm.TasksMutex.RUnlock()
	return len(wm.ActiveTasks)
}
