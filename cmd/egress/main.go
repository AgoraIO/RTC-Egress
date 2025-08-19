package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/agora-build/rtc-egress/pkg/egress"
	"github.com/agora-build/rtc-egress/pkg/health"
	"github.com/agora-build/rtc-egress/pkg/queue"
	"github.com/agora-build/rtc-egress/pkg/uploader"
	"github.com/agora-build/rtc-egress/pkg/utils"
	"github.com/gin-gonic/gin"
	"github.com/spf13/viper"
)

type Config struct {
	AppID        string `mapstructure:"app_id"`
	AccessToken  string `mapstructure:"access_token"`
	HealthPort   int    `mapstructure:"health_port"`
	APIPort      int    `mapstructure:"api_port"`
	TemplatePort int    `mapstructure:"template_port"`
	Snapshots    struct {
		OutputDir string `mapstructure:"output_dir"`
		Layout    string `mapstructure:"layout"`
	} `mapstructure:"snapshots"`
	Recording struct {
		OutputDir string `mapstructure:"output_dir"`
		Layout    string `mapstructure:"layout"`
		Format    string `mapstructure:"format"`
		Video     struct {
			Enabled bool   `mapstructure:"enabled"`
			Codec   string `mapstructure:"codec"`
		} `mapstructure:"video"`
		Audio struct {
			Enabled bool   `mapstructure:"enabled"`
			Codec   string `mapstructure:"codec"`
		} `mapstructure:"audio"`
	} `mapstructure:"recording"`
	Redis struct {
		Addr           string   `mapstructure:"addr"`
		Password       string   `mapstructure:"password"`
		DB             int      `mapstructure:"db"`
		TaskTTL        int      `mapstructure:"task_ttl"`
		WorkerPatterns []string `mapstructure:"worker_patterns"`
	} `mapstructure:"redis"`
	Pod struct {
		Region     string `mapstructure:"region"`
		NumWorkers int    `mapstructure:"workers"`
	} `mapstructure:"pod"`
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
	viper.SetConfigName("egress_config")
	viper.SetConfigType("yaml")
	viper.AddConfigPath("./config")
	viper.AddConfigPath("/etc/ag_egress")
	viper.AddConfigPath("/opt/ag_egress/config")
	viper.AddConfigPath("$HOME/.ag_egress")

	// Enable environment variable overrides
	viper.AutomaticEnv()
	viper.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))

	// Set environment variable bindings for key configuration
	viper.BindEnv("app_id", "APP_ID")
	viper.BindEnv("redis.addr", "REDIS_ADDR")
	viper.BindEnv("redis.password", "REDIS_PASSWORD")
	viper.BindEnv("redis.db", "REDIS_DB")
	viper.BindEnv("pod.region", "POD_REGION")
	viper.BindEnv("pod.workers", "POD_WORKERS")

	// Read config file
	if err := viper.ReadInConfig(); err != nil {
		return fmt.Errorf("error reading config file: %v", err)
	}

	// Unmarshal the entire config
	if err := viper.Unmarshal(&config); err != nil {
		return fmt.Errorf("error unmarshaling config: %v", err)
	}

	// Validate mandatory APP_ID (managed mode - ACCESS_TOKEN comes from requests)
	if strings.TrimSpace(config.AppID) == "" {
		return fmt.Errorf("app_id is required (set via environment variable APP_ID or in egress_config.yaml)")
	}

	log.Printf("Configuration loaded for managed mode:")
	log.Printf("  APP_ID: %s", config.AppID)

	// Validate required fields
	if strings.TrimSpace(config.Snapshots.OutputDir) == "" {
		return fmt.Errorf("snapshots.output_dir is required")
	}
	if strings.TrimSpace(config.Snapshots.Layout) == "" {
		return fmt.Errorf("snapshots.layout is required")
	}

	if strings.TrimSpace(config.Recording.OutputDir) == "" {
		return fmt.Errorf("recording.output_dir is required")
	}
	if strings.TrimSpace(config.Recording.Layout) == "" {
		return fmt.Errorf("recording.layout is required")
	}
	if strings.TrimSpace(config.Recording.Format) == "" {
		return fmt.Errorf("recording.format is required")
	}
	if strings.TrimSpace(config.Recording.Video.Codec) == "" {
		return fmt.Errorf("recording.video.codec is required")
	}
	if strings.TrimSpace(config.Recording.Audio.Codec) == "" {
		return fmt.Errorf("recording.audio.codec is required")
	}

	// Set default worker count if not specified
	if config.Pod.NumWorkers <= 0 {
		config.Pod.NumWorkers = 4 // Default to 4 workers
	}

	// Validate S3 configuration if bucket is provided
	if config.S3.Bucket != "" {
		if config.S3.Region == "" {
			return fmt.Errorf("S3 region is required when S3 bucket is specified")
		}
		if config.S3.AccessKey == "" || config.S3.SecretKey == "" {
			return fmt.Errorf("S3 access_key and secret_key are required when S3 bucket is specified")
		}
		if config.S3.Endpoint != "" {
			if _, err := url.Parse(config.S3.Endpoint); err != nil {
				return fmt.Errorf("S3 endpoint is configured but not a valid URL: %v", err)
			}
		}
	}

	// Ensure snapshots output directory exists
	if err := os.MkdirAll(config.Snapshots.OutputDir, 0755); err != nil {
		return fmt.Errorf("failed to create snapshots output directory: %v", err)
	}

	// Ensure recording output directory exists
	if err := os.MkdirAll(config.Recording.OutputDir, 0755); err != nil {
		return fmt.Errorf("failed to create recording output directory: %v", err)
	}

	return nil
}

func initRedisQueue() error {
	if config.Redis.Addr == "" {
		log.Println("Redis not configured, using direct worker management")
		return nil
	}

	redisQueue = queue.NewRedisQueue(
		config.Redis.Addr,
		config.Redis.Password,
		config.Redis.DB,
		config.Redis.TaskTTL,
		config.Redis.WorkerPatterns,
		config.Pod.Region,
	)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := redisQueue.Ping(ctx); err != nil {
		return fmt.Errorf("failed to connect to Redis: %v", err)
	}

	log.Printf("Connected to Redis at %s", config.Redis.Addr)
	if config.Pod.Region != "" {
		log.Printf("Pod region configured: %s", config.Pod.Region)
	} else {
		log.Printf("Pod region not configured, will handle all regional queues")
	}
	log.Printf("Pod workers configured: %d", config.Pod.NumWorkers)
	return nil
}

func startWorkerManager() {
	go func() {
		egress.ManagerMainWithRedisAndHealth(redisQueue, healthManager, config.Pod.NumWorkers)
	}()
}

func healthCheckHandler(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{
		"status":  "ok",
		"version": "1.0.0",
	})
}

func initHealthManager() error {
	if redisQueue == nil {
		log.Println("Redis not configured, HealthManager disabled")
		return nil
	}

	// Generate unique 12-character pod ID
	podID := utils.GenerateRandomID(12)
	version := "v1.0.0" // Could be from build flags

	healthManager = health.NewHealthManager(redisQueue.Client(), podID, config.Pod.Region, version)

	// Register this pod
	if err := healthManager.RegisterPod(config.Pod.NumWorkers); err != nil {
		return fmt.Errorf("failed to register pod: %v", err)
	}

	// Start background processes
	healthManager.StartRegionalStatsCalculation()

	log.Printf("HealthManager initialized for pod %s in region %s", podID, config.Pod.Region)
	return nil
}

func startHealthCheckServer() {
	r := gin.Default()

	// Health check
	r.GET("/health", healthCheckHandler)

	// Add health monitoring API if HealthManager is available
	if healthManager != nil {
		healthAPI := health.NewHealthAPI(healthManager)
		healthAPI.RegisterRoutes(r)
		log.Println("Health monitoring API enabled")
	}

	go func() {
		srv := &http.Server{
			Addr:    fmt.Sprintf(":%d", config.HealthPort),
			Handler: r,
		}
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("Failed to start health check server: %v", err)
		}
	}()

	_, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	log.Printf("Health server started on port %d", config.HealthPort)
}

func startTemplateServer() {
	r := gin.Default()
	r.Static("/template", "./web/template")

	// Start the server in a goroutine
	go func() {
		if err := r.Run(fmt.Sprintf(":%d", config.TemplatePort)); err != nil {
			log.Fatalf("Failed to start template server: %v", err)
		}
	}()
}

func startUploader() {
	log.Println("Starting uploader initialization...")

	// Check if we have any output directories to watch
	outputDirs := []string{}
	if config.Snapshots.OutputDir != "" {
		outputDirs = append(outputDirs, config.Snapshots.OutputDir)
		log.Printf("Snapshots output directory: %s", config.Snapshots.OutputDir)
	}
	if config.Recording.OutputDir != "" {
		outputDirs = append(outputDirs, config.Recording.OutputDir)
		log.Printf("Recording output directory: %s", config.Recording.OutputDir)
	}

	if len(outputDirs) == 0 {
		log.Println("No output directories configured, file watcher disabled")
		return
	}

	if config.S3.Bucket == "" || config.S3.Bucket == "your-s3-bucket" {
		log.Println("S3 bucket not configured, file watcher disabled")
		return
	}

	log.Printf("S3 Bucket: %s, Region: %s, Endpoint: %s", config.S3.Bucket, config.S3.Region, config.S3.Endpoint)

	s3Config := uploader.S3Config{
		Bucket:    config.S3.Bucket,
		Region:    config.S3.Region,
		AccessKey: config.S3.AccessKey,
		SecretKey: config.S3.SecretKey,
		Endpoint:  config.S3.Endpoint,
	}

	// Start file watchers for each output directory
	for _, dir := range outputDirs {
		log.Printf("Creating file watcher for directory: %s", dir)
		watcher, err := uploader.NewWatcher(dir, s3Config)
		if err != nil {
			log.Printf("Failed to create file watcher for %s: %v", dir, err)
			continue
		}

		// Start each watcher in a separate goroutine
		go func(watchDir string, w *uploader.Watcher) {
			log.Printf("Starting file watcher for directory: %s", watchDir)
			if err := w.Start(context.Background()); err != nil {
				log.Printf("File watcher error for %s: %v", watchDir, err)
			} else {
				log.Printf("File watcher for %s started successfully", watchDir)
			}
		}(dir, watcher)
	}
}

func main() {
	// Load configuration
	if err := loadConfig(); err != nil {
		log.Fatalf("Error loading config: %v", err)
	}

	// Initialize Redis queue if configured
	if err := initRedisQueue(); err != nil {
		log.Fatalf("Error initializing Redis: %v", err)
	}

	// Initialize HealthManager if Redis is available
	if err := initHealthManager(); err != nil {
		log.Printf("Warning: Failed to initialize HealthManager: %v", err)
	}

	// Start worker manager in background in a separate goroutine
	startWorkerManager()

	// Start template server
	startTemplateServer()

	// Start uploader if configured
	startUploader()

	// Setup and start Health server
	startHealthCheckServer()

	// Wait for interrupt signal to gracefully shut down the server
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	log.Println("Shutting down server...")

	// Clean up worker processes
	egress.CleanupWorkers()

	log.Println("Server exiting")
}
