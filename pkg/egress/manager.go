package egress

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/health"
	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
)

var egressConfigPath string

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
}

type Worker struct {
	ID         int
	Cmd        *exec.Cmd
	SocketPath string
	Conn       net.Conn
	Status     WorkerStatus
	Mutex      sync.Mutex
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

// WorkerManagerMode defines the operational mode of the worker manager
type WorkerManagerMode int

const (
	ModeNative WorkerManagerMode = iota // Handles native recording tasks only
	ModeWeb                             // Handles web recording tasks only
)

type WorkerManager struct {
	Workers           []*Worker
	NumWorkers        int
	BinPath           string
	Mutex             sync.Mutex
	ChannelWorkers    map[string]int                 // Maps channel name to worker ID (0,1,2,3)
	WorkerChannels    map[int]string                 // Maps worker ID to channel name
	ChannelTasks      map[string]map[string][]string // Maps channel -> task type -> []task_ids
	RedisQueue        *queue.RedisQueue              // Redis queue for task management
	PodID             string                         // Unique pod identifier
	ActiveTasks       map[string]string              // Maps task_id -> pod_id for lease renewal
	TasksMutex        sync.RWMutex                   // Protects ActiveTasks map
	HealthManager     *health.HealthManager          // Health monitoring (optional)
	webRecorderProxy  *WorkerManagerWebRecorderProxy // Web recorder proxy for web tasks
	mode              WorkerManagerMode              // Operational mode of this manager
	webRecorderConfig *WebRecorderConfig             // Web recorder configuration
	patterns          []string                       // Task patterns this manager handles
}

// Global worker manager instance for cleanup
var globalWorkerManager *WorkerManager

func NewWorkerManagerWithMode(binPath string, num int, redisQueue *queue.RedisQueue, podID string, healthManager *health.HealthManager, mode WorkerManagerMode, webConfig *WebRecorderConfig, patterns []string) *WorkerManager {
	wm := &WorkerManager{
		Workers:           make([]*Worker, num),
		NumWorkers:        num,
		BinPath:           binPath,
		ChannelWorkers:    make(map[string]int),
		WorkerChannels:    make(map[int]string),
		ChannelTasks:      make(map[string]map[string][]string),
		RedisQueue:        redisQueue,
		PodID:             podID,
		ActiveTasks:       make(map[string]string),
		HealthManager:     healthManager,
		mode:              mode,
		webRecorderConfig: webConfig,
		patterns:          patterns,
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

	// In ModeWeb, we don't spawn eg_worker processes - web tasks are handled via HTTP API calls
	if wm.mode == ModeWeb {
		log.Printf("Worker %d: Web recording uses external API", i)
		// TODO: should check the web recorder status with probe asynchronously
		wm.Workers[i] = &Worker{
			ID:  i,
			Cmd: nil,
			Status: WorkerStatus{
				ID:    i,
				State: WORKER_RUNNING, // Mark as running since web mode doesn't need C++ workers
			},
		}
		return
	}

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
	// In ModeWeb, there are no eg_worker processes to monitor
	// WebRecorder is managerd by k8s or other external system
	if wm.mode == ModeWeb {
		log.Printf("Worker monitoring disabled in web/flexible recorder mode")
		return
	}

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

// handleTaskCompletion processes task completion from C++ worker(sendCompletionMessage)
func (wm *WorkerManager) handleTaskCompletion(worker *Worker, completion *UDSCompletionMessage) {
	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	log.Printf("Worker %d completed task %s with status: %s", worker.ID, completion.TaskID, completion.Status)

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

	// Update Redis task status (normal stopped/failed processing)
	if wm.RedisQueue != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		status := queue.TaskStateStopped
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
		}
	}

	// After handling redis and cleanup below, decide worker state based on remaining tasks

	// Remove from active tasks tracking for lease renewal
	wm.TasksMutex.Lock()
	delete(wm.ActiveTasks, completion.TaskID)
	wm.TasksMutex.Unlock()

	// Clean up channel mappings - find this worker's channel and remove this specific task
	var channelFound string
	var cmdFound string
	wm.Mutex.Lock()
	for ch, wid := range wm.ChannelWorkers {
		if wid == worker.ID {
			// Identify command type that contains this task ID
			if channelTasks, ok := wm.ChannelTasks[ch]; ok {
				for cmd, taskIDs := range channelTasks {
					for _, taskID := range taskIDs {
						if taskID == completion.TaskID {
							channelFound = ch
							cmdFound = cmd
							break
						}
					}
					if channelFound != "" {
						break
					}
				}
			}
			break
		}
	}
	// Remove mapping if found
	if channelFound != "" && cmdFound != "" {
		wm.Mutex.Unlock()
		wm.removeTaskFromChannel(channelFound, cmdFound, completion.TaskID)
	} else {
		wm.Mutex.Unlock()
		log.Printf("Warning: Could not locate task %s in channel/task tracking for worker %d", completion.TaskID, worker.ID)
		log.Printf("Warning: This is NORMAL if triggered by stop task from user")
	}

	// Determine if worker should remain RUNNING (other tasks left on its channel)
	wm.Mutex.Lock()
	if ch, ok := wm.WorkerChannels[worker.ID]; ok {
		if tasksByType, exists := wm.ChannelTasks[ch]; exists && len(tasksByType) > 0 {
			// Still tasks on this channel
			worker.Status.State = WORKER_RUNNING
		} else {
			worker.Status.State = WORKER_IDLE
		}
	} else {
		// No channel assigned; mark idle defensively
		worker.Status.State = WORKER_IDLE
	}
	wm.Mutex.Unlock()
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
	originalQueue := wm.RedisQueue.BuildQueueKey(task.Cmd, task.Channel)
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

	// Validate that patterns were provided during construction
	if len(wm.patterns) == 0 {
		log.Fatalf("Worker patterns not configured - cannot start Redis subscriber")
		return
	}

	log.Printf("Using configured patterns: %v", wm.patterns)

	for {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)

		task, err := wm.fetchTaskFromRedis(ctx, wm.patterns)
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
		// Check if this consumer should yield for regional tasks
		shouldYield := wm.shouldYieldForRegionalTask(queue)
		if shouldYield {
			// Yield 3 times (200ms each) to allow regional consumers priority
			for i := 0; i < 3; i++ {
				time.Sleep(200 * time.Millisecond)
				// Check if we have available workers during yield period
				if wm.getIdleWorkerCount() == 0 {
					break // Stop yielding if no workers available anyway
				}
			}
		}

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

		// Record source queue for web/native detection priority
		task.SourceQueue = queue

		if shouldYield {
			log.Printf("Pod %s (non-regional) fetched regional task %s from queue %s after yielding", wm.PodID, taskID, queue)
		} else {
			log.Printf("Pod %s fetched task %s from queue %s", wm.PodID, taskID, queue)
		}
		return task, nil
	}

	return nil, nil // No tasks available
}

// isValidQueueKey validates that a key follows expected queue structure
func isValidQueueKey(keyParts []string) bool {
	if len(keyParts) < 3 || keyParts[0] != "egress" {
		return false
	}

	validCmds := map[string]bool{"snapshot": true, "record": true, "rtmp": true, "whip": true}

	// Global queue: egress:cmd:channel OR egress:web:cmd:channel
	if len(keyParts) == 3 {
		return validCmds[keyParts[1]]
	} else if len(keyParts) == 4 && keyParts[1] == "web" {
		return validCmds[keyParts[2]]
	} else if len(keyParts) == 4 {
		// Regional queue: egress:region:cmd:channel
		return validCmds[keyParts[2]]
	} else if len(keyParts) == 5 && keyParts[2] == "web" {
		// Regional web queue: egress:region:web:cmd:channel
		return validCmds[keyParts[3]]
	}

	return false
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
		// Use the original pattern directly for Redis KEYS command
		keys, err := client.Keys(ctx, basePattern).Result()
		if err != nil {
			log.Printf("Error getting keys for pattern %s: %v", basePattern, err)
			continue
		}

		// Categorize queues based on regional matching priority
		for _, key := range keys {
			keyParts := strings.Split(key, ":")
			if len(keyParts) < 3 {
				continue
			}

			// Validate queue key structure - must match expected formats:
			// Global: egress:cmd:channel OR egress:web:cmd:channel
			// Regional: egress:region:cmd:channel OR egress:region:web:cmd:channel
			if !isValidQueueKey(keyParts) {
				continue
			}

			// Parse queue key format:
			// Global: egress:record:channelName, egress:web:record:channelName
			// Regional: egress:region:record:channelName, egress:region:web:record:channelName

			var taskRegion string
			isRegionalTask := false

			// Detect if this is a regional task by checking key structure
			if len(keyParts) >= 4 {
				// Could be: egress:region:record:channelName or egress:region:web:record:channelName
				possibleRegion := keyParts[1]
				if possibleRegion != "record" && possibleRegion != "snapshot" && possibleRegion != "web" && possibleRegion != "cmd" {
					taskRegion = possibleRegion
					isRegionalTask = true
				}
			}

			// Priority assignment:
			if isRegionalTask {
				if podRegion == taskRegion {
					// Highest priority: regional match
					regionalQueues = append(regionalQueues, key)
				} else {
					// Lower priority: different/no region (will yield before taking)
					globalQueues = append(globalQueues, key)
				}
			} else {
				// Global task: any consumer can take immediately
				globalQueues = append(globalQueues, key)
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

// shouldYieldForRegionalTask determines if this consumer should yield to regional consumers
func (wm *WorkerManager) shouldYieldForRegionalTask(queueKey string) bool {
	podRegion := wm.getRedisQueueRegion()

	// Parse queue key to determine if it's a regional task
	keyParts := strings.Split(queueKey, ":")
	if len(keyParts) < 4 {
		return false // Not a valid queue format
	}

	// Detect if this is a regional task
	var taskRegion string
	isRegionalTask := false

	if len(keyParts) >= 5 && keyParts[3] == "channel" {
		// Format: egress:region:record:channel:* or egress:region:snapshot:channel:*
		possibleRegion := keyParts[1]
		if possibleRegion != "record" && possibleRegion != "snapshot" && possibleRegion != "web" && possibleRegion != "cmd" {
			taskRegion = possibleRegion
			isRegionalTask = true
		}
	} else if len(keyParts) >= 6 && keyParts[4] == "channel" {
		// Format: egress:region:web:record:channel:* or egress:region:web:snapshot:channel:*
		possibleRegion := keyParts[1]
		if possibleRegion != "record" && possibleRegion != "snapshot" && possibleRegion != "web" && possibleRegion != "cmd" {
			taskRegion = possibleRegion
			isRegionalTask = true
		}
	}

	// Yield if: regional task AND (consumer has no region OR different region)
	return isRegionalTask && (podRegion == "" || podRegion != taskRegion)
}

// getIdleWorkerCount returns the number of idle workers
func (wm *WorkerManager) getIdleWorkerCount() int {
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	idleCount := 0
	for _, worker := range wm.Workers {
		if worker != nil && worker.Status.State == WORKER_IDLE {
			idleCount++
		}
	}
	return idleCount
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
	log.Printf("Processing Redis task %s (cmd: %s, channel: %s)", task.ID, task.Cmd, task.Channel)

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

// isWebTask determines if a task should be handled by the web recorder path
func (wm *WorkerManager) isWebTask(task *queue.Task) bool {
	// 1) Prefer routing by source queue (authoritative)
	if task != nil && task.SourceQueue != "" {
		parts := strings.Split(task.SourceQueue, ":")
		// egress:web:cmd:channel OR egress:region:web:cmd:channel
		if len(parts) >= 3 && parts[0] == "egress" {
			if parts[1] == "web" {
				return true
			}
			if len(parts) >= 4 && parts[2] == "web" {
				return true
			}
		}
	}

	// 2) Fallback to payload layout
	if task != nil && task.Payload != nil {
		if v, ok := task.Payload["layout"]; ok {
			if s, ok2 := v.(string); ok2 {
				if strings.EqualFold(s, "freestyle") {
					return true
				}
			}
		}
	}

	// 3) Fallback to manager mode if payload not informative
	return wm.mode == ModeWeb
}

// startTask starts a task from Redis queue directly
func (wm *WorkerManager) startTask(task *queue.Task) error {
	if wm.isWebTask(task) {
		return wm.startWebRecorderTask(task)
	}
	return wm.startNativeRecorderTask(task)
}

// startNativeRecorderTask starts a native recording task using eg_workers
func (wm *WorkerManager) startNativeRecorderTask(task *queue.Task) error {
	// Create UDS message directly from task for native tasks
	udsMsg := &UDSMessage{
		TaskID:  task.ID,
		Cmd:     task.Cmd,
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
	cmd := task.Cmd

	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	// Enforce only one task per type on a given channel/worker
	if channelTasks, ok := wm.ChannelTasks[channel]; ok {
		if existing, typeExists := channelTasks[cmd]; typeExists && len(existing) > 0 {
			return fmt.Errorf("task of type %s already running on channel %s", cmd, channel)
		}
	}

	// Find worker for this channel (reuse if exists, find free if new)
	worker, err, _ := wm.findWorkerForChannel(channel, cmd)
	if err != nil {
		return err
	}

	// Update channel mappings
	wm.ChannelWorkers[channel] = worker.ID
	wm.WorkerChannels[worker.ID] = channel
	if wm.ChannelTasks[channel] == nil {
		wm.ChannelTasks[channel] = make(map[string][]string)
	}
	wm.ChannelTasks[channel][cmd] = append(wm.ChannelTasks[channel][cmd], task.ID)

	// Send task to worker
	worker.Mutex.Lock()
	err = wm.sendUDSMessageToWorker(worker, task.ID, udsMsg)
	if err != nil {
		worker.Mutex.Unlock()
		// Clean up on failure
		wm.removeTaskFromChannel(channel, cmd, task.ID)
		return fmt.Errorf("failed to send task to worker: %v", err)
	}

	// Update worker state (may have multiple tasks of different types)
	worker.Status.State = WORKER_RUNNING
	worker.Mutex.Unlock()

	log.Printf("Pod %s assigned task %s (%s:%s) to worker %d", wm.PodID, task.ID, channel, cmd, worker.ID)

	// Update worker ID in Redis for this task
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	wm.RedisQueue.UpdateTaskWorker(ctx, task.ID, worker.ID)

	return nil
}

func (wm *WorkerManager) startWebRecorderTask(task *queue.Task) error {
	log.Printf("Starting web recording task %s (cmd: %s, channel: %s)", task.ID, task.Cmd, task.Channel)

	// Create web recorder proxy if not exists (lazy initialization)
	if wm.webRecorderProxy == nil {
		var webConfig WebRecorderConfig
		if wm.webRecorderConfig != nil {
			webConfig = *wm.webRecorderConfig
		} else {
			// Default web recorder configuration
			webConfig = WebRecorderConfig{
				BaseURL:    "http://localhost:8001",
				Timeout:    30,
				AuthToken:  "",
				MaxRetries: 3,
			}
		}
		wm.webRecorderProxy = NewWorkerManagerWebRecorderProxy(webConfig, wm.PodID)
	}

	// Process the web task using the proxy
	err := wm.webRecorderProxy.ProcessRedisTask(task)
	if err != nil {
		return fmt.Errorf("failed to start web recording task: %v", err)
	}

	log.Printf("Successfully started web recording task %s", task.ID)
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

	// Always mark stop task(not the origial task) as STOPPED at the end
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		// Mark stop task as STOPPED - stop operation acknowledged
		err := wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateStopped, "Stop command executed")
		if err != nil {
			log.Printf("Failed to update stop task %s state to STOPPED: %v", task.ID, err)
		}

		// Remove stop task from lease renewal
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, task.ID)
		wm.TasksMutex.Unlock()
	}()

	// 2. Mark original as STOPPING immediately, then check task type
	ctxMark, cancelMark := context.WithTimeout(context.Background(), 5*time.Second)
	wm.RedisQueue.UpdateTaskResult(ctxMark, originalTaskID, queue.TaskStateStopping, "Stop in progress")
	cancelMark()

	// 3. Check if original task was a web recording task
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	originalTask, err := wm.RedisQueue.GetTaskStatus(ctx, originalTaskID)
	cancel()

	if err == nil && wm.isWebTask(originalTask) {
		// This is a web task - use web recorder proxy to stop it
		if wm.webRecorderProxy != nil {
			log.Printf("Stopping web recording task %s via web recorder proxy", originalTaskID)
			err := wm.webRecorderProxy.ProcessRedisTask(task)
			if err != nil {
				log.Printf("Failed to stop web recording task %s: %v", originalTaskID, err)
			} else {
				log.Printf("Successfully stopped web recording task %s", originalTaskID)
			}
		}

		// Mark original task as STOPPED
		ctx, cancel = context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		wm.RedisQueue.UpdateTaskResult(ctx, originalTaskID, queue.TaskStateStopped, "Stopped by user via web recorder")
		return nil
	}

	// 4. Find which worker is running the original native task
	var targetWorker *Worker
	var targetWorkerID int
	wm.Mutex.Lock()
	// Prefer resolution by channel->worker mapping, verifying membership in ChannelTasks
	if err == nil && originalTask != nil && originalTask.Channel != "" {
		if wid, ok := wm.ChannelWorkers[originalTask.Channel]; ok {
			if w := wm.Workers[wid]; w != nil {
				// Verify the task is tracked on this channel
				if tasksByType, exists := wm.ChannelTasks[originalTask.Channel]; exists {
					found := false
					for _, ids := range tasksByType {
						for _, id := range ids {
							if id == originalTaskID {
								found = true
								break
							}
						}
						if found {
							break
						}
					}
					if found {
						targetWorker = w
						targetWorkerID = wid
					}
				} else {
					// No tasks tracked but channel maps to worker; use it as best effort
					targetWorker = w
					targetWorkerID = wid
				}
			}
		}
	}

	// If still not found, search ChannelTasks for this task ID to resolve channel -> worker
	if targetWorker == nil {
		for ch, tasksByType := range wm.ChannelTasks {
			found := false
			for _, ids := range tasksByType {
				for _, id := range ids {
					if id == originalTaskID {
						found = true
						break
					}
				}
				if found {
					break
				}
			}
			if found {
				if wid, ok := wm.ChannelWorkers[ch]; ok {
					if w := wm.Workers[wid]; w != nil {
						targetWorker = w
						targetWorkerID = wid
						break
					}
				}
			}
		}
	}

	// No legacy single-task fallback; resolution must use channel/task mappings
	wm.Mutex.Unlock()

	if targetWorker == nil {
		// Original task may have already completed or not found
		log.Printf("Original task %s not found on any worker for stop task %s", originalTaskID, task.ID)

		// Clean up original task from lease renewal anyway
		wm.TasksMutex.Lock()
		delete(wm.ActiveTasks, originalTaskID)
		wm.TasksMutex.Unlock()

		// Mark original task as STOPPED (may already be completed)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		err := wm.RedisQueue.UpdateTaskResult(ctx, originalTaskID, queue.TaskStateStopped, "Task already completed or not found")
		if err != nil {
			log.Printf("Failed to update original task %s state: %v", originalTaskID, err)
		}

		return nil
	}

	// 5. Try to stop original task gracefully with retries
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
		Cmd:     stopTask.Cmd,
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
			wm.cleanupStoppedTask(stopTask.Channel, stopTask.Cmd, originalTaskID, true)
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
	wm.forceKillWorker(targetWorkerID, originalTaskID, stopTask.Channel, stopTask.Cmd)

	return false
}

// cleanupStoppedTask removes task from all tracking and marks as STOPPED if not a regular stop
func (wm *WorkerManager) cleanupStoppedTask(channel, cmd, taskID string, regularStop bool) {
	// Remove from channel tracking
	wm.removeTaskFromChannel(channel, cmd, taskID)

	// Remove from active tasks tracking (stop lease renewal)
	wm.TasksMutex.Lock()
	delete(wm.ActiveTasks, taskID)
	wm.TasksMutex.Unlock()

	// Mark original task as STOPPED and remove from Redis completely
	// Regular update is handled by handleTaskCompletion
	if !regularStop {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		err := wm.RedisQueue.UpdateTaskResult(ctx, taskID, queue.TaskStateStopped, "Stopped by user(abnormal stop)")
		if err != nil {
			log.Printf("Failed to update task %s state to STOPPED: %v", taskID, err)
		}
	}
}

// forceKillWorker kills worker process and cleans up, WorkerManager handles recovery
func (wm *WorkerManager) forceKillWorker(workerID int, taskID, channel, cmd string) {
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
	wm.cleanupStoppedTask(channel, cmd, taskID, false)

	// Mark worker as dead - WorkerManager will handle respawning
	worker.Status.State = WORKER_NOT_AVAILABLE
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
		Cmd:     task.Cmd,
		Action:  "stop",
		Channel: task.Channel,
		Uid:     []string{}, // Initialize as empty slice to avoid null JSON serialization
	}

	// NOTE: Worker mutex is already locked by the caller (handleTaskCompletion)
	// Do not lock again to avoid deadlock
	// Try to send stop command to worker (best effort)
	log.Printf("Sending stop command to worker %d for FAILED task %s (channel: %s, cmd: %s)", workerID, task.ID, task.Channel, task.Cmd)
	err := wm.sendUDSMessageToWorker(worker, task.ID, udsMsg)
	if err != nil {
		log.Printf("Failed to send stop command to worker %d for FAILED task %s: %v", workerID, task.ID, err)
		// Continue anyway - mark as FAILED even if stop failed
	} else {
		log.Printf("Successfully sent stop command to worker %d for FAILED task %s", workerID, task.ID)
	}

	// Clean up task from channel tracking
	wm.removeTaskFromChannel(task.Channel, task.Cmd, task.ID)

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
		wm.PodID, task.ID, task.Channel, task.Cmd, workerID, reason)
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
	// Route based on payload layout (preferred) or manager mode fallback
	if wm.isWebTask(task) {
		if wm.mode != ModeWeb {
			return fmt.Errorf("native-only manager cannot handle web task status")
		}
		return wm.getWebTaskStatus(task)
	}
	if wm.mode != ModeNative {
		return fmt.Errorf("web-only manager cannot handle native task status")
	}
	return wm.getNativeTaskStatus(task)
}

// getNativeTaskStatus gets status of a native task from eg_workers
func (wm *WorkerManager) getNativeTaskStatus(task *queue.Task) error {
	udsMsg := &UDSMessage{
		TaskID:  task.ID,
		Cmd:     task.Cmd,
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

	// Validate that this task is tracked for the worker's channel
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()
	if ch, ok := wm.WorkerChannels[workerID]; ok {
		if tasksByType, exists := wm.ChannelTasks[ch]; exists {
			found := false
			for _, ids := range tasksByType {
				for _, id := range ids {
					if id == task.ID {
						found = true
						break
					}
				}
				if found {
					break
				}
			}
			if !found {
				return fmt.Errorf("task %s not found on worker %d", task.ID, workerID)
			}
		} else {
			return fmt.Errorf("task %s not found on worker %d", task.ID, workerID)
		}
	} else {
		return fmt.Errorf("worker %d not assigned to any channel", workerID)
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

// getWebTaskStatus gets status of a web recording task using the web recorder proxy
func (wm *WorkerManager) getWebTaskStatus(task *queue.Task) error {
	if wm.webRecorderProxy != nil {
		return wm.webRecorderProxy.ProcessRedisTask(task)
	}
	return fmt.Errorf("web recorder proxy not available for task %s", task.ID)
}

// findWorkerForChannel finds worker for channel (reuse existing or find free)
func (wm *WorkerManager) findWorkerForChannel(channel, cmd string) (*Worker, error, bool) {
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
func (wm *WorkerManager) removeTaskFromChannel(channel, cmd, taskId string) {
	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	if channelTasks, ok := wm.ChannelTasks[channel]; ok {
		if taskIDs, typeExists := channelTasks[cmd]; typeExists {
			// Remove specific task ID
			for i, id := range taskIDs {
				if id == taskId {
					wm.ChannelTasks[channel][cmd] = append(taskIDs[:i], taskIDs[i+1:]...)
					break
				}
			}
			// Clean up empty task type
			if len(wm.ChannelTasks[channel][cmd]) == 0 {
				delete(wm.ChannelTasks[channel], cmd)
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

func InitEgressConfig(configFile string) {
	egressConfigPath = configFile
}

func ManagerMainWithRedisAndHealth(redisQueue *queue.RedisQueue, healthManager *health.HealthManager, numWorkers int, patterns []string) {
	binPath := "./bin/eg_worker"

	if egressConfigPath == "" {
		log.Fatal("egress config file not specified")
	}

	log.Printf("Using egress config file: %s", egressConfigPath)

	// Generate unique 12-character pod ID
	podID := utils.GenerateRandomID(12)

	wm := NewWorkerManagerWithMode(binPath, numWorkers, redisQueue, podID, healthManager, ModeNative, nil, patterns)
	globalWorkerManager = wm // Store for cleanup
	wm.StartAll()

	log.Printf("Native egress worker manager started, listening for Redis tasks with patterns: %v", patterns)

	// Keep running to process Redis tasks
	select {}
}

// getActiveTaskCount returns the number of currently active tasks
func (wm *WorkerManager) getActiveTaskCount() int {
	wm.TasksMutex.RLock()
	defer wm.TasksMutex.RUnlock()
	return len(wm.ActiveTasks)
}
