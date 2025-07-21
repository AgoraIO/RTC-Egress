package egress

import (
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
	ChannelWorkers map[string]int             // Maps channel name to worker ID
	WorkerChannels map[int]string             // Maps worker ID to channel name
	ChannelTasks   map[string]map[string]bool // Maps channel -> task type -> true
}

// Global worker manager instance for cleanup
var globalWorkerManager *WorkerManager

func NewWorkerManager(binPath string, num int) *WorkerManager {
	return &WorkerManager{
		Workers:        make([]*Worker, num),
		NumWorkers:     num,
		BinPath:        binPath,
		ChannelWorkers: make(map[string]int),
		WorkerChannels: make(map[int]string),
		ChannelTasks:   make(map[string]map[string]bool),
	}
}

func (wm *WorkerManager) StartAll() {
	for i := 0; i < wm.NumWorkers; i++ {
		wm.startWorker(i)
	}
	// Start monitoring goroutine
	go wm.monitorWorkers()
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
				log.Printf("Worker %d (pid %d) is dead, relaunching", i, w.Cmd.Process.Pid)
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

// findWorkerForChannel finds the appropriate worker for a channel and task:
// 1. If channel already has a worker with tasks, check for duplicate task
// 2. If channel already has a worker, reuse it for different task
// 3. If no existing channel assignment, find a free worker
func (wm *WorkerManager) findWorkerForChannel(channel, taskType string) (*Worker, error, bool) {
	// Check if channel already has a worker assigned
	if workerID, exists := wm.ChannelWorkers[channel]; exists {
		worker := wm.Workers[workerID]
		if worker == nil || worker.Status.State == WORKER_NOT_AVAILABLE {
			// Worker is dead, clean up mapping
			delete(wm.ChannelWorkers, channel)
			delete(wm.WorkerChannels, workerID)
			delete(wm.ChannelTasks, channel)
		} else {
			// Check for duplicate task on this channel
			if channelTasks, ok := wm.ChannelTasks[channel]; ok {
				if _, taskExists := channelTasks[taskType]; taskExists {
					return nil, fmt.Errorf("task %s already running for channel %s on worker %d", taskType, channel, workerID), true
				}
			}
			// Return existing worker for different task on same channel
			return worker, nil, false
		}
	}

	// No existing assignment, find a free worker
	worker, err := wm.findFreeWorker()
	if err != nil {
		return nil, err, false
	}

	return worker, nil, false
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
	log.Printf("AssignTask payload: %+v", taskReq.Payload)

	wm.Mutex.Lock()
	defer wm.Mutex.Unlock()

	// Validate and flatten the task request first to get channel info
	udsMsg, err := ValidateAndFlattenTaskRequest(&taskReq)
	if err != nil {
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    "",
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// Extract channel and task type for intelligent assignment
	channel := udsMsg.Channel
	taskType := taskReq.Cmd
	requestId := taskReq.RequestID

	// Find the appropriate worker based on channel assignment rules
	worker, err, isDuplicate := wm.findWorkerForChannel(channel, taskType)
	if isDuplicate {
		// Return specific error for duplicate tasks
		return &TaskResponse{
			RequestID: requestId,
			TaskID:    "",
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}
	if err != nil {
		return &TaskResponse{
			RequestID: requestId,
			TaskID:    "",
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	workerID := worker.ID
	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	// Generate taskId: 11 random digits + workerId (12 digits total)
	randomPart := randomDigits(11)
	taskId := fmt.Sprintf("%s%d", randomPart, workerID)

	// Send the flattened UDSMessage directly to worker
	err = wm.sendUDSMessageToWorker(worker, taskId, udsMsg)
	if err != nil {
		return &TaskResponse{
			RequestID: requestId,
			TaskID:    taskId,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// Update worker status
	worker.Status.State = WORKER_RUNNING
	worker.Status.TaskID = taskId

	// Update channel and task mappings
	wm.ChannelWorkers[channel] = workerID
	wm.WorkerChannels[workerID] = channel
	if wm.ChannelTasks[channel] == nil {
		wm.ChannelTasks[channel] = make(map[string]bool)
	}
	wm.ChannelTasks[channel][taskType] = true

	log.Printf("Assigned task %s (channel: %s, type: %s) to worker %d", taskId, channel, taskType, workerID)

	return &TaskResponse{
		RequestID: requestId,
		TaskID:    taskId,
		Status:    "success",
	}, nil
}

func (wm *WorkerManager) ReleaseWorker(workerID int, taskReq TaskRequest) (*TaskResponse, error) {
	wm.Mutex.Lock()
	worker := wm.Workers[workerID]
	wm.Mutex.Unlock()

	if worker == nil {
		err := fmt.Errorf("worker %d not found", workerID)
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	if worker.Status.TaskID != taskReq.TaskID {
		err := fmt.Errorf("taskId %s not found on worker %d", taskReq.TaskID, workerID)
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	worker.Mutex.Lock()
	defer worker.Mutex.Unlock()

	// Validate and flatten the task request
	udsMsg, err := ValidateAndFlattenTaskRequest(&taskReq)
	if err != nil {
		err := fmt.Errorf("failed to validate and flatten task request: %v", err)
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// Send the flattened UDSMessage to worker
	err = wm.sendUDSMessageToWorker(worker, taskReq.TaskID, udsMsg)
	if err != nil {
		err := fmt.Errorf("failed to send stop command to worker %d: %v", workerID, err)
		return &TaskResponse{
			RequestID: taskReq.RequestID,
			TaskID:    taskReq.TaskID,
			Status:    "failed",
			Error:     err.Error(),
		}, err
	}

	// Clean up channel and task mappings
	wm.Mutex.Lock()
	channel := udsMsg.Channel
	taskType := taskReq.Cmd

	// Remove this specific task type from the channel
	if channelTasks, ok := wm.ChannelTasks[channel]; ok {
		delete(channelTasks, taskType)
		// If no more task types for this channel, clean up completely
		if len(channelTasks) == 0 {
			delete(wm.ChannelTasks, channel)
			delete(wm.ChannelWorkers, channel)
			delete(wm.WorkerChannels, workerID)
			worker.Status.State = WORKER_IDLE
		}
	}

	wm.Mutex.Unlock()

	worker.Status.TaskID = ""
	log.Printf("Released task %s (channel: %s, type: %s) from worker %d", taskReq.TaskID, channel, taskType, workerID)

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

	// Send flattened UDSMessage to worker
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

func ManagerMain() {
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
	wm := NewWorkerManager(binPath, 4)
	globalWorkerManager = wm // Store for cleanup
	wm.StartAll()

	r := gin.Default()

	// New API endpoint for automatic task assignment
	r.POST("/egress/v1/task/assign", func(c *gin.Context) {
		var taskReq TaskRequest
		if err := c.ShouldBindJSON(&taskReq); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error(), "request_id": taskReq.RequestID})
			return
		}
		taskReq.Action = "start"
		// Validate and flatten task request using the unified function
		_, err := ValidateAndFlattenTaskRequest(&taskReq)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error(), "request_id": taskReq.RequestID})
			return
		}
		response, err := wm.AssignTask(taskReq)
		if err != nil {
			c.JSON(http.StatusInternalServerError, response)
			return
		}
		c.JSON(http.StatusOK, response)
	})

	// API endpoint to release a worker from a task
	r.POST("/egress/v1/task/release", func(c *gin.Context) {
		var taskReq TaskRequest
		if err := c.ShouldBindJSON(&taskReq); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}
		taskReq.Action = "release"
		workerID, err := parseWorkerIdFromTaskId(taskReq.TaskID, wm.NumWorkers)
		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}
		response, err := wm.ReleaseWorker(workerID, taskReq)
		if err != nil {
			c.JSON(http.StatusInternalServerError, response)
			return
		}
		c.JSON(http.StatusOK, response)
	})

	// Legacy API endpoint for direct worker commands (still available)
	r.POST("/egress/v1/task/status", func(c *gin.Context) {
		var taskReq TaskRequest
		if err := c.ShouldBindJSON(&taskReq); err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}

		taskReq.Action = "status"
		workerID, err := parseWorkerIdFromTaskId(taskReq.TaskID, wm.NumWorkers)

		if err != nil {
			c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
			return
		}
		workerStatus := &WorkerStatus{}
		if err := wm.GetWorkerStatus(workerStatus, workerID, taskReq); err != nil {
			c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
			return
		}

		c.JSON(http.StatusOK, gin.H{"status": workerStatus})
	})

	log.Println("Manager API server running on :8090")
	r.Run(":8090")
}
