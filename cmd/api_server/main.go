package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/egress"
	"github.com/AgoraIO/RTC-Egress/pkg/logger"
	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/AgoraIO/RTC-Egress/pkg/version"
	"github.com/gin-gonic/gin"
	"github.com/spf13/viper"
)

// parseRegionFromIP extracts region from IP address (fake implementation for now)
func parseRegionFromIP(ip string) string {
	// TODO: Implement real IP-to-region mapping
	// For now, return empty region (global)
	return ""
}

type APIServer struct {
	redisQueue *queue.RedisQueue
	config     *Config
}

type Config struct {
	Redis struct {
		Addr     string `mapstructure:"addr"`
		Password string `mapstructure:"password"`
		DB       int    `mapstructure:"db"`
	} `mapstructure:"redis"`
	Server struct {
		Port       int `mapstructure:"port"`
		HealthPort int `mapstructure:"health_port"`
	} `mapstructure:"server"`
	TaskRouting struct {
		DefaultRegion string `mapstructure:"default_region"`
		TaskTTL       int    `mapstructure:"task_ttl"`
	} `mapstructure:"task_routing"`
}

type Task struct {
	TaskID      string                 `json:"task_id"`
	ChannelName string                 `json:"channel_name"`
	Token       string                 `json:"token"`
	Mode        string                 `json:"mode"` // "record" or "snapshot"
	Output      map[string]interface{} `json:"output"`
	WebRecorder map[string]interface{} `json:"web_recorder,omitempty"`
	Region      string                 `json:"region"`
	CreatedAt   time.Time              `json:"created_at"`
	ExpiresAt   time.Time              `json:"expires_at"`
}

type TaskResponse struct {
	TaskID string `json:"task_id"`
	Status string `json:"status"`
}

var config Config

func NewAPIServer() *APIServer {
	redisQueue := queue.NewRedisQueue(
		config.Redis.Addr,
		config.Redis.Password,
		config.Redis.DB,
		config.TaskRouting.TaskTTL,
		config.TaskRouting.DefaultRegion,
	)

	return &APIServer{
		redisQueue: redisQueue,
		config:     &config,
	}
}

func (s *APIServer) startTask(c *gin.Context) {
	appID := c.Param("app_id")
	if appID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "app_id is required"})
		return
	}

	var taskReq egress.TaskRequest
	if err := c.ShouldBindJSON(&taskReq); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error(), "request_id": taskReq.RequestID})
		return
	}

	// Payload is required
	if taskReq.Payload == nil {
		c.JSON(http.StatusBadRequest, gin.H{
			"error":      "payload is required",
			"request_id": taskReq.RequestID,
		})
		return
	}

	// Add app_id to payload
	taskReq.Payload["app_id"] = appID

	// Set action to "start" since this is the start endpoint
	taskReq.Action = "start"

	// Validate layout in payload
	layout := "flat" // default
	if layoutVal, ok := taskReq.Payload["layout"]; ok {
		if layoutStr, isStr := layoutVal.(string); isStr && layoutStr != "" {
			layout = layoutStr
		}
	}
	validLayouts := []string{"flat", "spotlight", "customized", "freestyle"}
	layoutValid := false
	for _, validLayout := range validLayouts {
		if layout == validLayout {
			layoutValid = true
			break
		}
	}
	if !layoutValid {
		c.JSON(http.StatusBadRequest, gin.H{
			"error":      fmt.Sprintf("invalid layout: %s", layout),
			"request_id": taskReq.RequestID,
		})
		return
	}

	// Validate freestyle layout has web_recorder config
	if layout == "freestyle" {
		if webRecorder, ok := taskReq.Payload["web_recorder"]; !ok || webRecorder == nil {
			c.JSON(http.StatusBadRequest, gin.H{
				"error":      "freestyle layout requires web_recorder configuration in payload",
				"request_id": taskReq.RequestID,
			})
			return
		}
	}

	// Validate request parameters before Redis publishing
	if err := egress.ValidateStartTaskRequest(&taskReq); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{
			"error":      fmt.Sprintf("validation failed: %v", err),
			"request_id": taskReq.RequestID,
		})
		return
	}

	// Redis is required for task management
	if s.redisQueue == nil {
		c.JSON(http.StatusInternalServerError, gin.H{
			"error":      "Redis queue not configured",
			"request_id": taskReq.RequestID,
		})
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Extract channel from payload
	channel := ""
	if ch, ok := taskReq.Payload["channel"].(string); ok && ch != "" {
		channel = ch
	}
	if channel == "" {
		c.JSON(http.StatusBadRequest, gin.H{
			"error":      "channel is required in payload",
			"request_id": taskReq.RequestID,
		})
		return
	}

	// Parse region from client IP
	clientIP := c.ClientIP()
	region := parseRegionFromIP(clientIP)
	logger.Debug("Parsed request region",
		logger.String("client_ip", clientIP),
		logger.String("region", region))

	task, err := s.redisQueue.PublishTaskToRegion(ctx, taskReq.Cmd, "start", channel, taskReq.RequestID, taskReq.Payload, region)
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
}

func (s *APIServer) stopTask(c *gin.Context) {
	appID := c.Param("app_id")
	taskID := c.Param("task_id")
	if appID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "app_id is required"})
		return
	}
	if taskID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "task_id is required"})
		return
	}

	var req struct {
		RequestID string `json:"request_id"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	// Validate request parameters
	if err := egress.ValidateStopTaskRequest(req.RequestID, taskID); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{
			"error":      fmt.Sprintf("validation failed: %v", err),
			"request_id": req.RequestID,
		})
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Get the existing task
	task, err := s.redisQueue.GetTaskStatus(ctx, taskID)
	if err != nil {
		c.JSON(http.StatusOK, gin.H{
			"request_id": req.RequestID,
			"task_id":    taskID,
			"status":     "rejected",
			"error":      fmt.Sprintf("Task %s not found", taskID),
		})
		return
	}

	// Check if task is already completed
	if task.State == queue.TaskStateStopped || task.State == queue.TaskStateFailed || task.State == queue.TaskStateTimeout {
		c.JSON(http.StatusOK, gin.H{
			"request_id": req.RequestID,
			"task_id":    taskID,
			"status":     "completed",
			"message":    fmt.Sprintf("Task already in terminal state: %s", task.State),
		})
		return
	}

	// Parse region from client IP for stop task routing as well
	clientIP := c.ClientIP()
	region := parseRegionFromIP(clientIP)
	logger.Debug("Parsed stop request region",
		logger.String("client_ip", clientIP),
		logger.String("region", region))

	// Publish a stop task derived from original without requiring layout in payload
	_, err = s.redisQueue.PublishStopTaskFor(ctx, taskID, req.RequestID, region)
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{
			"error":      fmt.Sprintf("Failed to create stop task: %v", err),
			"request_id": req.RequestID,
			"task_id":    taskID,
		})
		return
	}

	logger.Info("Stop requested",
		logger.String("task_id", taskID),
		logger.String("request_id", req.RequestID))

	c.JSON(http.StatusOK, gin.H{
		"request_id": req.RequestID,
		"task_id":    taskID,
		"status":     "enqueued",
		"message":    fmt.Sprintf("Stop task enqueued for task %s", taskID),
	})
}

func (s *APIServer) getTaskStatus(c *gin.Context) {
	appID := c.Param("app_id")
	taskID := c.Param("task_id")
	if appID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "app_id is required"})
		return
	}
	if taskID == "" {
		c.JSON(http.StatusBadRequest, gin.H{"error": "task_id is required"})
		return
	}

	var req struct {
		RequestID string `json:"request_id"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": err.Error()})
		return
	}

	// Validate request parameters
	if err := egress.ValidateStatusTaskRequest(req.RequestID, taskID); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{
			"error":      fmt.Sprintf("validation failed: %v", err),
			"request_id": req.RequestID,
		})
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	task, err := s.redisQueue.GetTaskStatus(ctx, taskID)
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
		"message":    task.Message,
		"worker_id":  task.WorkerID,
	})
}

func (s *APIServer) validateTask(task *Task) error {
	// Validate mode
	if task.Mode != "record" && task.Mode != "snapshot" {
		return fmt.Errorf("invalid mode: %s", task.Mode)
	}

	return nil
}

func (s *APIServer) healthCheck(c *gin.Context) {
	// Check Redis connection
	ctx := context.Background()
	err := s.redisQueue.Ping(ctx)
	if err != nil {
		c.JSON(http.StatusServiceUnavailable, gin.H{
			"status": "unhealthy",
			"error":  "Redis connection failed",
		})
		return
	}

	c.JSON(http.StatusOK, gin.H{
		"status":    "healthy",
		"service":   "api-server",
		"version":   version.GetVersion(),
		"timestamp": time.Now().Unix(),
	})
}

func getRegion() string {
	region := os.Getenv("POD_REGION")
	if region == "" {
		return "us-west-1"
	}
	return region
}

func generateRandomString(length int) string {
	// Simple random string generation
	const charset = "abcdefghijklmnopqrstuvwxyz0123456789"
	b := make([]byte, length)
	for i := range b {
		b[i] = charset[time.Now().UnixNano()%int64(len(charset))]
	}
	return string(b)
}

func loadConfig() error {
	// Support CLI flag and use shared resolver
	var cfFlag string
	flag.StringVar(&cfFlag, "config", "", "Path to config YAML file")
	_ = flag.CommandLine.Parse(os.Args[1:])
	resolved, err := utils.ResolveConfigFile("api_server_config.yaml", os.Args[1:])
	if err != nil {
		return err
	}
	logger.Info("Using config file", logger.String("path", resolved))
	viper.SetConfigFile(resolved)
	viper.SetConfigType("yaml")

	// Environment variable overrides
	viper.SetEnvPrefix("API_SERVER")
	viper.AutomaticEnv()
	viper.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))
	viper.BindEnv("redis.addr", "REDIS_ADDR")
	viper.BindEnv("redis.password", "REDIS_PASSWORD")
	viper.BindEnv("redis.db", "REDIS_DB")
	viper.BindEnv("server.port", "API_PORT")
	viper.BindEnv("server.health_port", "HEALTH_PORT")

	if err := viper.ReadInConfig(); err != nil {
		logger.Warn("Config file not found, using environment variables and defaults", logger.ErrorField(err))
	}

	if err := viper.Unmarshal(&config); err != nil {
		return fmt.Errorf("error unmarshaling config: %v", err)
	}
	config.Redis.Addr = utils.ResolveRedisAddr(config.Redis.Addr)

	if strings.TrimSpace(config.Redis.Addr) == "" {
		return fmt.Errorf("redis.addr is required for api server")
	}
	return nil
}

func main() {
	logger.Init("api-server")
	logger.Info("api-server starting", logger.String("version", version.FullVersion()))

	// Load configuration
	if err := loadConfig(); err != nil {
		logger.Fatal("Failed to load config", logger.ErrorField(err))
	}
	server := NewAPIServer()

	logger.Info("Connected to Redis", logger.String("redis_addr", config.Redis.Addr))

	// Main API router
	r := gin.New()
	r.Use(logger.GinRequestLogger(), gin.Recovery())

	// API routes
	v1 := r.Group("/egress/v1")
	{
		v1.POST("/:app_id/tasks", server.startTask)
		v1.POST("/:app_id/tasks/:task_id/stop", server.stopTask)
		v1.POST("/:app_id/tasks/:task_id/status", server.getTaskStatus)
	}

	// Health check router (separate port)
	healthRouter := gin.New()
	healthRouter.Use(logger.GinRequestLogger(), gin.Recovery())
	healthRouter.GET("/health", server.healthCheck)

	// Start health server
	go func() {
		healthAddr := fmt.Sprintf(":%d", config.Server.HealthPort)
		logger.Info("Health server starting", logger.String("address", healthAddr))
		if err := healthRouter.Run(healthAddr); err != nil {
			logger.Fatal("Health server failed", logger.ErrorField(err))
		}
	}()

	// Start main API server
	addr := fmt.Sprintf(":%d", config.Server.Port)
	logger.Info("API server starting", logger.String("address", addr))

	if err := r.Run(addr); err != nil {
		logger.Fatal("API server failed", logger.ErrorField(err))
	}
}
