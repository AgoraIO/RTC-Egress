package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/egress"
	"github.com/AgoraIO/RTC-Egress/pkg/health"
	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/gin-gonic/gin"
	"github.com/spf13/viper"
)

// Config matches flexible_recorder_config.yaml.example structure
// Binary name: flexible-recorder
type Config struct {
	Server struct {
		HealthPort int `mapstructure:"health_port"`
	} `mapstructure:"server"`
	Pod struct {
		Region     string `mapstructure:"region"`
		NumWorkers int    `mapstructure:"workers"`
	} `mapstructure:"pod"`
	Redis struct {
		Addr           string   `mapstructure:"addr"`
		Password       string   `mapstructure:"password"`
		DB             int      `mapstructure:"db"`
		TaskTTL        int      `mapstructure:"task_ttl"`
		WorkerPatterns []string `mapstructure:"worker_patterns"`
	} `mapstructure:"redis"`
	WebRecorder struct {
		BaseURL           string `mapstructure:"base_url"`
		Timeout           int    `mapstructure:"timeout"`
		Filename          string `mapstructure:"filename"`
		Format            string `mapstructure:"format"`
		LogName           string `mapstructure:"logname"`
		AudioSample       int    `mapstructure:"audioSample"`
		AudioChannel      int    `mapstructure:"audioChannel"`
		AudioBitrate      int    `mapstructure:"audioBitrate"`
		WindowWidth       int    `mapstructure:"windowWidth"`
		WindowHeight      int    `mapstructure:"windowHeight"`
		VideoFPS          int    `mapstructure:"videoFps"`
		VideoBitrate      int    `mapstructure:"videoBitrate"`
		SliceDuration     int    `mapstructure:"sliceDuration"`
		MaxRecordingHours int    `mapstructure:"maxRecordingHours"`
		EnableWebGL       bool   `mapstructure:"enableWebGL"`
		OutputPath        string `mapstructure:"output_path"`
		MaxRetries        int    `mapstructure:"max_retries"`
	} `mapstructure:"web_recorder"`
	S3 struct {
		Bucket    string `mapstructure:"bucket"`
		Region    string `mapstructure:"region"`
		AccessKey string `mapstructure:"access_key"`
		SecretKey string `mapstructure:"secret_key"`
		Endpoint  string `mapstructure:"endpoint"`
	} `mapstructure:"s3"`
}

var (
	config        Config
	redisQueue    *queue.RedisQueue
	healthManager *health.HealthManager
)

func loadConfig() error {
	viper.SetConfigName("flexible_recorder_config")
	viper.SetConfigType("yaml")
	viper.AddConfigPath("./config")
	viper.AddConfigPath("/etc/rtc_egress")
	viper.AddConfigPath("/opt/rtc_egress/config")
	viper.AddConfigPath("$HOME/.rtc_egress")

	// Enable environment variable overrides
	viper.AutomaticEnv()
	viper.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))

	// Set environment variable bindings for key configuration
	viper.BindEnv("redis.addr", "REDIS_ADDR")
	viper.BindEnv("redis.password", "REDIS_PASSWORD")
	viper.BindEnv("redis.db", "REDIS_DB")
	viper.BindEnv("pod.region", "POD_REGION")
	viper.BindEnv("web_recorder.base_url", "WEB_RECORDER_BASE_URL")
	viper.BindEnv("web_recorder.auth_token", "WEB_RECORDER_AUTH_TOKEN")

	// Read config file
	if err := viper.ReadInConfig(); err != nil {
		return fmt.Errorf("error reading config file: %v", err)
	}

	// Unmarshal the entire config
	if err := viper.Unmarshal(&config); err != nil {
		return fmt.Errorf("error unmarshaling config: %v", err)
	}

	// No app_id validation needed - web recorder engine handles RTC credentials

	// Set default worker count - for web recorder we don't need many "workers"
	// since the actual work is done by the web recorder engine
	if config.Pod.NumWorkers <= 0 {
		config.Pod.NumWorkers = 1 // Just 1 coordination process for web recorder
	}

	// Set web recorder defaults
	if config.WebRecorder.BaseURL == "" {
		config.WebRecorder.BaseURL = "http://localhost:8001"
	}
	if config.WebRecorder.Timeout <= 0 {
		config.WebRecorder.Timeout = 30
	}
	if config.WebRecorder.MaxRetries <= 0 {
		config.WebRecorder.MaxRetries = 3
	}
	if config.WebRecorder.OutputPath == "" {
		config.WebRecorder.OutputPath = "/opt2"
	}

	log.Printf("Flexible Recorder configuration loaded:")
	log.Printf("  Web Recorder URL: %s", config.WebRecorder.BaseURL)
	log.Printf("  Worker Patterns: %v", config.Redis.WorkerPatterns)

	return nil
}

func initRedisQueue() error {
	if config.Redis.Addr == "" {
		return fmt.Errorf("Redis connection required for flexible recorder")
	}

	redisQueue = queue.NewRedisQueue(
		config.Redis.Addr,
		config.Redis.Password,
		config.Redis.DB,
		config.Redis.TaskTTL,
		config.Pod.Region,
	)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := redisQueue.Ping(ctx); err != nil {
		return fmt.Errorf("failed to connect to Redis: %v", err)
	}

	log.Printf("Connected to Redis at %s", config.Redis.Addr)
	log.Printf("Flexible Recorder will handle patterns: %v", config.Redis.WorkerPatterns)
	return nil
}

func initHealthManager() error {
	if redisQueue == nil {
		log.Println("Redis not configured, HealthManager disabled")
		return nil
	}

	// Generate unique pod ID for flexible recorder
	podID := fmt.Sprintf("flex-%s", utils.GenerateRandomID(8))
	version := "v1.0.0"

	healthManager = health.NewHealthManager(redisQueue.Client(), podID, config.Pod.Region, version)

	// Register this pod as flexible recorder
	if err := healthManager.RegisterPod(config.Pod.NumWorkers); err != nil {
		return fmt.Errorf("failed to register flexible recorder pod: %v", err)
	}

	log.Printf("HealthManager initialized for flexible recorder pod %s", podID)
	return nil
}

func startWorkerManager() {
	// Create web recorder configuration
	webConfig := &egress.WebRecorderConfig{
		BaseURL:    config.WebRecorder.BaseURL,
		Timeout:    config.WebRecorder.Timeout,
		MaxRetries: config.WebRecorder.MaxRetries,
	}

	// Generate unique pod ID
	podID := utils.GenerateRandomID(12)

	// Create worker manager in ModeWeb (web tasks only)
	wm := egress.NewWorkerManagerWithMode(
		"./bin/eg_worker", // Not used for web tasks but required for interface
		config.Pod.NumWorkers,
		redisQueue,
		podID,
		healthManager,
		egress.ModeWeb, // This is the key difference - web mode only
		webConfig,
		config.Redis.WorkerPatterns,
	)

	// Start the worker manager
	go func() {
		wm.StartAll()
	}()

	log.Printf("Flexible Recorder worker manager started in ModeWeb")
}

func healthCheckHandler(c *gin.Context) {
	// Check web recorder connectivity
	webRecorderStatus := "healthy"
	client := &http.Client{Timeout: 5 * time.Second}
	if _, err := client.Get(config.WebRecorder.BaseURL + "/health"); err != nil {
		webRecorderStatus = "web_recorder_unreachable"
	}

	c.JSON(http.StatusOK, gin.H{
		"status":              "ok",
		"service":             "flexible-recorder",
		"version":             "1.0.0",
		"mode":                "web-only",
		"web_recorder_status": webRecorderStatus,
		"web_recorder_url":    config.WebRecorder.BaseURL,
		"worker_patterns":     config.Redis.WorkerPatterns,
	})
}

func startHealthCheckServer() {
	r := gin.Default()

	// Health check
	r.GET("/health", healthCheckHandler)

	// Add health monitoring API if HealthManager is available
	if healthManager != nil {
		healthAPI := health.NewHealthAPI(healthManager)
		healthAPI.RegisterRoutes(r)
		log.Println("Health monitoring API enabled for flexible recorder")
	}

	go func() {
		srv := &http.Server{
			Addr:    fmt.Sprintf(":%d", config.Server.HealthPort),
			Handler: r,
		}
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("Failed to start health check server: %v", err)
		}
	}()

	log.Printf("Flexible recorder health server started on port %d", config.Server.HealthPort)
}

func main() {
	// Load configuration
	if err := loadConfig(); err != nil {
		log.Fatalf("Failed to load configuration: %v", err)
	}

	// Initialize Redis queue
	if err := initRedisQueue(); err != nil {
		log.Fatalf("Failed to initialize Redis: %v", err)
	}

	// Initialize health manager
	if err := initHealthManager(); err != nil {
		log.Fatalf("Failed to initialize health manager: %v", err)
	}

	// Start health check server
	startHealthCheckServer()

	// Start worker manager - REUSES existing egress manager but in ModeWeb
	startWorkerManager()

	log.Printf("Flexible Recorder service started in web-only mode")
	log.Printf("Handling web recording patterns: %v", config.Redis.WorkerPatterns)
	log.Printf("Web Recorder Engine URL: %s", config.WebRecorder.BaseURL)

	// Wait for interrupt signal to gracefully shutdown the server
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	log.Println("Flexible Recorder service stopping...")

	// Cleanup health manager
	if healthManager != nil {
		log.Println("Cleaning up health manager...")
	}

	// Clean up worker processes (shared cleanup function)
	egress.CleanupWorkers()

	log.Println("Flexible Recorder service stopped")
}
