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

	"github.com/AgoraIO/RTC-Egress/pkg/egress"
	"github.com/AgoraIO/RTC-Egress/pkg/health"
	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/uploader"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/AgoraIO/RTC-Egress/pkg/version"
	"github.com/gin-gonic/gin"
	"github.com/spf13/viper"
)

type Config struct {
	Pod struct {
		Region     string `mapstructure:"region"`
		NumWorkers int    `mapstructure:"workers"`
	} `mapstructure:"pod"`
	Server struct {
		HealthPort int `mapstructure:"health_port"`
	} `mapstructure:"server"`
	Redis struct {
		Addr           string   `mapstructure:"addr"`
		Password       string   `mapstructure:"password"`
		DB             int      `mapstructure:"db"`
		TaskTTL        int      `mapstructure:"task_ttl"`
		WorkerPatterns []string `mapstructure:"worker_patterns"`
	} `mapstructure:"redis"`
	Agora struct {
		AppID       string `mapstructure:"app_id"`
		ChannelName string `mapstructure:"channel_name"`
		AccessToken string `mapstructure:"access_token"`
		EgressUID   string `mapstructure:"egress_uid"`
		RTCTimeout  int    `mapstructure:"rtc_timeout"`
	} `mapstructure:"agora"`
	Snapshots struct {
		OutputDir    string `mapstructure:"output_dir"`
		Width        int    `mapstructure:"width"`
		Height       int    `mapstructure:"height"`
		Users        string `mapstructure:"users"`
		Layout       string `mapstructure:"layout"`
		IntervalInMs int    `mapstructure:"interval_in_ms"`
		Quality      int    `mapstructure:"quality"`
	} `mapstructure:"snapshots"`
	Recording struct {
		OutputDir          string `mapstructure:"output_dir"`
		Users              string `mapstructure:"users"`
		Layout             string `mapstructure:"layout"`
		Format             string `mapstructure:"format"`
		MaxDurationSeconds int    `mapstructure:"max_duration_seconds"`
		Video              struct {
			Enabled    bool   `mapstructure:"enabled"`
			Width      int    `mapstructure:"width"`
			Height     int    `mapstructure:"height"`
			FPS        int    `mapstructure:"fps"`
			Bitrate    int    `mapstructure:"bitrate"`
			Codec      string `mapstructure:"codec"`
			BufferSize int    `mapstructure:"buffer_size"`
		} `mapstructure:"video"`
		Audio struct {
			Enabled    bool   `mapstructure:"enabled"`
			SampleRate int    `mapstructure:"sample_rate"`
			Channels   int    `mapstructure:"channels"`
			Bitrate    int    `mapstructure:"bitrate"`
			Codec      string `mapstructure:"codec"`
			BufferSize int    `mapstructure:"buffer_size"`
		} `mapstructure:"audio"`
		TS struct {
			SegmentDuration        int  `mapstructure:"segment_duration"`
			GeneratePlaylist       bool `mapstructure:"generate_playlist"`
			KeepIncompleteSegments bool `mapstructure:"keep_incomplete_segments"`
		} `mapstructure:"ts"`
	} `mapstructure:"recording"`
	S3 struct {
		Bucket    string `mapstructure:"bucket"`
		Region    string `mapstructure:"region"`
		AccessKey string `mapstructure:"access_key"`
		SecretKey string `mapstructure:"secret_key"`
		Endpoint  string `mapstructure:"endpoint"`
	} `mapstructure:"s3"`
	Log struct {
		Level      string `mapstructure:"level"`
		Path       string `mapstructure:"path"`
		MaxSize    int    `mapstructure:"max_size"`
		MaxBackups int    `mapstructure:"max_backups"`
		MaxAge     int    `mapstructure:"max_age"`
	} `mapstructure:"log"`
	Advanced struct {
		VideoBufferSize int  `mapstructure:"video_buffer_size"`
		AudioBufferSize int  `mapstructure:"audio_buffer_size"`
		GPUAcceleration bool `mapstructure:"gpu_acceleration"`
		ThreadPoolSize  int  `mapstructure:"thread_pool_size"`
	} `mapstructure:"advanced"`
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
	viper.AddConfigPath("/etc/rtc_egress")
	viper.AddConfigPath("/opt/rtc_egress/config")
	viper.AddConfigPath("$HOME/.rtc_egress")

	// Enable environment variable overrides
	viper.AutomaticEnv()
	viper.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))

	// Set environment variable bindings for key configuration
	viper.BindEnv("agora.app_id", "APP_ID")
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
	if strings.TrimSpace(config.Agora.AppID) == "" {
		return fmt.Errorf("agora.app_id is required (set via environment variable APP_ID or in egress_config.yaml)")
	}

	log.Printf("Configuration loaded for managed mode:")
	log.Printf("  APP_ID: %s", config.Agora.AppID)

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

	// Create directories only if not in container AND they don't exist
	if !utils.IsRunningInContainer() {
		log.Printf("Running in local mode - checking output directories")

		// Check and create snapshots directory if needed
		if _, err := os.Stat(config.Snapshots.OutputDir); os.IsNotExist(err) {
			if err := os.MkdirAll(config.Snapshots.OutputDir, 0755); err != nil {
				return fmt.Errorf("failed to create snapshots output directory: %v", err)
			}
			log.Printf("Created snapshots directory: %s", config.Snapshots.OutputDir)
		}

		// Check and create recording directory if needed
		if _, err := os.Stat(config.Recording.OutputDir); os.IsNotExist(err) {
			if err := os.MkdirAll(config.Recording.OutputDir, 0755); err != nil {
				return fmt.Errorf("failed to create recording output directory: %v", err)
			}
			log.Printf("Created recording directory: %s", config.Recording.OutputDir)
		}

		log.Printf("Output directories ready: snapshots=%s, recordings=%s", config.Snapshots.OutputDir, config.Recording.OutputDir)
	} else {
		// In container mode, directories should already exist via volume mounts
		log.Printf("Running in container mode - output directories should be mounted as volumes")
		log.Printf("Expected mount points: snapshots=%s, recordings=%s", config.Snapshots.OutputDir, config.Recording.OutputDir)
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
		egress.ManagerMainWithRedisAndHealth(redisQueue, healthManager, config.Pod.NumWorkers, config.Redis.WorkerPatterns)
	}()
}

func healthCheckHandler(c *gin.Context) {
	c.JSON(http.StatusOK, gin.H{
		"status":  "ok",
		"version": version.GetVersion(),
	})
}

func initHealthManager() error {
	if redisQueue == nil {
		log.Println("Redis not configured, HealthManager disabled")
		return nil
	}

	// Generate unique 12-character pod ID
	podID := utils.GenerateRandomID(12)
	appVersion := version.GetVersion()

	healthManager = health.NewHealthManager(redisQueue.Client(), podID, config.Pod.Region, appVersion, int(egress.ModeNative))

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
			Addr:    fmt.Sprintf(":%d", config.Server.HealthPort),
			Handler: r,
		}
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("Failed to start health check server: %v", err)
		}
	}()

	_, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	log.Printf("Health server started on port %d", config.Server.HealthPort)
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
