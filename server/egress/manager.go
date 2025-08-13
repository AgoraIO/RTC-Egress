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
	"sync"
	"syscall"
	"time"

	"github.com/agora-build/rtc-egress/server/queue"
	"github.com/gin-gonic/gin"
)

var egressConfigPath string
var requestIdRegexp = regexp.MustCompile(`^[0-9a-zA-Z]{1,32}$`)

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
	ChannelWorkers map[string]int                 // Maps channel name to worker ID
	WorkerChannels map[int]string                 // Maps worker ID to channel name
	ChannelTasks   map[string]map[string][]string // Maps channel -> task type -> []task_ids
	RedisQueue     *queue.RedisQueue              // Redis queue for task management
	WorkerID       string                         // Unique worker manager ID
	ActiveTasks    map[string]string              // Maps task_id -> worker_id for lease renewal
	TasksMutex     sync.RWMutex                   // Protects ActiveTasks map
}

// Global worker manager instance for cleanup
var globalWorkerManager *WorkerManager

func NewWorkerManager(binPath string, num int, redisQueue *queue.RedisQueue, workerID string) *WorkerManager {
	return &WorkerManager{
		Workers:        make([]*Worker, num),
		NumWorkers:     num,
		BinPath:        binPath,
		ChannelWorkers: make(map[string]int),
		WorkerChannels: make(map[int]string),
		ChannelTasks:   make(map[string]map[string][]string),
		RedisQueue:     redisQueue,
		WorkerID:       workerID,
		ActiveTasks:    make(map[string]string),
	}
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
			// Try to parse as completion message
			var completion UDSCompletionMessage
			if err := json.Unmarshal(buffer[:n], &completion); err != nil {
				log.Printf("Failed to parse completion message from worker %d: %v", worker.ID, err)
				continue
			}

			// Handle the completion message
			wm.handleTaskCompletion(worker, &completion)
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

	// Update Redis task status
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

	log.Printf("Started lease renewal process for worker %s", wm.WorkerID)

	for {
		select {
		case <-ticker.C:
			wm.renewAllLeases()
		}
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
		if workerManagerID == wm.WorkerID {
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
	task.State = queue.TaskStatePending
	task.WorkerID = ""
	task.LeaseExpiry = nil
	task.ProcessedAt = nil
	task.RetryCount++

	// Update task and move back to main queue
	originalQueue := wm.RedisQueue.BuildQueueKey(task.Type, task.Channel)
	processingQueue := wm.RedisQueue.BuildProcessingQueueKey(wm.WorkerID)
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
	log.Printf("Starting Redis task subscriber for worker %s", wm.WorkerID)

	for {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)

		// Use configured worker patterns or default patterns
		patterns := []string{"snapshot:*", "record:*"}
		if wm.RedisQueue != nil {
			// Get patterns from Redis queue configuration
			patterns = []string{"snapshot:*", "record:*", "web:record:*"}
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

// fetchTaskFromRedis polls Redis queues for available tasks
func (wm *WorkerManager) fetchTaskFromRedis(ctx context.Context, patterns []string) (*queue.Task, error) {
	if wm.RedisQueue == nil {
		return nil, nil
	}

	// Get matching queue keys
	var queues []string
	for _, pattern := range patterns {
		queuePattern := fmt.Sprintf("egress:%s", pattern)
		keys, err := wm.RedisQueue.Client().Keys(ctx, queuePattern).Result()
		if err != nil {
			continue
		}
		queues = append(queues, keys...)
	}

	if len(queues) == 0 {
		return nil, nil
	}

	// Try to pop a task from any available queue
	result, err := wm.RedisQueue.Client().BRPop(ctx, 1*time.Second, queues...).Result()
	if err != nil {
		if err.Error() == "redis: nil" { // No tasks available
			return nil, nil
		}
		return nil, err
	}

	if len(result) < 2 {
		return nil, fmt.Errorf("unexpected result format")
	}

	taskID := result[1]
	return wm.RedisQueue.GetTaskStatus(ctx, taskID)
}

// processRedisTask converts Redis task to existing TaskRequest and processes it
func (wm *WorkerManager) processRedisTask(task *queue.Task) {
	log.Printf("Processing Redis task %s (type: %s, channel: %s)", task.ID, task.Type, task.Channel)

	// Convert Redis task to existing TaskRequest format
	taskReq := TaskRequest{
		RequestID: task.RequestID,
		Cmd:       task.Type,
		Action:    task.Action,
		TaskID:    task.ID,
		Payload:   task.Payload,
	}

	// Use existing assignment logic
	response, err := wm.AssignTask(taskReq)

	// Update task status in Redis
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err != nil {
		log.Printf("Failed to process Redis task %s: %v", task.ID, err)
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateFailed, err.Error())
	} else {
		log.Printf("Successfully assigned Redis task %s: %+v", task.ID, response)
		// Task is now being processed - completion will be handled by the completion listener
		wm.RedisQueue.UpdateTaskResult(ctx, task.ID, queue.TaskStateProcessing, "")

		// Track task for lease renewal
		wm.TasksMutex.Lock()
		wm.ActiveTasks[task.ID] = wm.WorkerID
		wm.TasksMutex.Unlock()
	}
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

func parseWorkerIdFromTaskId(taskId string, numWorkers int) (int, error) {
	if len(taskId) != 12 {
		return -1, fmt.Errorf("invalid taskId length")
	}
	workerId := int(taskId[11] - '0')
	if workerId < 0 || workerId >= numWorkers {
		return -1, fmt.Errorf("invalid taskId, can not find a proper worker")
	}
	return workerId, nil
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

func (wm *WorkerManager) AssignTask(taskReq TaskRequest) (*TaskResponse, error) {
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

	// 3. Generate task ID
	taskId := fmt.Sprintf("%s%d", randomDigits(11), worker.ID)

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

	log.Printf("Assigned task %s (%s:%s) to worker %d", taskId, channel, taskType, worker.ID)
	return &TaskResponse{
		RequestID: taskReq.RequestID,
		TaskID:    taskId,
		Status:    "success",
	}, nil
}

func (wm *WorkerManager) ReleaseWorker(workerID int, taskReq TaskRequest) (*TaskResponse, error) {
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

	log.Printf("Released task %s (%s:%s) from worker %d", taskReq.TaskID, udsMsg.Channel, taskReq.Cmd, workerID)
	return &TaskResponse{
		RequestID: taskReq.RequestID,
		TaskID:    taskReq.TaskID,
		Status:    "success",
	}, nil
}

func (wm *WorkerManager) GetWorkerStatus(workerStatus *WorkerStatus, workerID int, taskReq TaskRequest) error {
	wm.Mutex.Lock()
	worker := wm.Workers[workerID]
	wm.Mutex.Unlock()

	if worker == nil {
		workerStatus.State = WORKER_NOT_AVAILABLE
		workerStatus.LastErr = fmt.Sprintf("worker %d not found", workerID)
		return fmt.Errorf("worker %d not found", workerID)
	}

	workerStatus.State = worker.Status.State
	workerStatus.TaskID = worker.Status.TaskID
	workerStatus.Pid = worker.Status.Pid
	workerStatus.LastErr = ""

	if worker.Status.TaskID != taskReq.TaskID {
		workerStatus.LastErr = fmt.Sprintf("taskId %s not found on worker %d", taskReq.TaskID, workerID)
		return fmt.Errorf("taskId %s not found on worker %d", taskReq.TaskID, workerID)
	}

	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	// Validate and flatten the task request
	udsMsg, err := ValidateAndFlattenTaskRequest(&taskReq)
	if err != nil {
		workerStatus.LastErr = fmt.Sprintf("failed to validate and flatten task request: %v", err)
		return fmt.Errorf("failed to validate and flatten task request: %v", err)
	}

	// Send the flattened UDSMessage to worker
	err = wm.sendUDSMessageToWorker(worker, taskReq.TaskID, udsMsg)

	if err != nil {
		workerStatus.LastErr = fmt.Sprintf("failed to send status command to worker %d: %v", workerID, err)
		return fmt.Errorf("failed to send status command to worker %d: %v", workerID, err)
	}

	log.Printf("Get status of task %s on worker %d", taskReq.TaskID, workerID)

	return nil
}

// sendUDSMessageToWorker sends a flattened UDSMessage directly to a worker
func (wm *WorkerManager) sendUDSMessageToWorker(worker *Worker, taskID string, udsMsg *UDSMessage) error {
	if worker == nil || worker.Conn == nil {
		return fmt.Errorf("worker not running")
	}

	// Create a wrapper message that includes the task ID for the C++ worker
	messageWithTaskID := struct {
		TaskID string `json:"task_id"`
		*UDSMessage
	}{
		TaskID:     taskID,
		UDSMessage: udsMsg,
	}

	// Send the message with task ID to worker
	data, err := json.Marshal(messageWithTaskID)
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

func ManagerMain() {
	ManagerMainWithRedis(nil)
}

func ManagerMainWithRedis(redisQueue *queue.RedisQueue) {
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

	// Generate unique worker ID
	workerID := fmt.Sprintf("worker-%d", time.Now().UnixNano())

	wm := NewWorkerManager(binPath, 4, redisQueue, workerID)
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

		task, err := wm.RedisQueue.PublishTask(ctx, taskReq.Cmd, "start", channel, taskReq.RequestID, taskReq.Payload)
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
			"status":     "queued",
		})
	})

	// API endpoint to stop a worker from a task
	r.POST("/egress/v1/task/stop", func(c *gin.Context) {
		var taskReq TaskRequest
		if err := c.ShouldBindJSON(&taskReq); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
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

		task, err := wm.RedisQueue.PublishTask(ctx, taskReq.Cmd, "stop", channel, taskReq.RequestID, taskReq.Payload)
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
			"status":     "queued",
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

		if req.TaskID == "" {
			c.JSON(http.StatusBadRequest, gin.H{"error": "task_id is required"})
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
