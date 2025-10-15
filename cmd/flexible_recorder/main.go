package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/egress"
	"github.com/AgoraIO/RTC-Egress/pkg/health"
	"github.com/AgoraIO/RTC-Egress/pkg/logger"
	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/AgoraIO/RTC-Egress/pkg/version"
	"github.com/gin-gonic/gin"
	"github.com/spf13/viper"
)

// Config matches flexible_recorder_config.yaml.example structure
// Binary name: flexible-recorder
type Config struct {
	Server struct {
		HealthPort     int      `mapstructure:"health_port"`
		Region         string   `mapstructure:"region"`
		Workers        int      `mapstructure:"workers"`
		TaskTTL        int      `mapstructure:"task_ttl"`
		WorkerPatterns []string `mapstructure:"worker_patterns"`
	} `mapstructure:"server"`
	Redis struct {
		Addr     string `mapstructure:"addr"`
		Password string `mapstructure:"password"`
		DB       int    `mapstructure:"db"`
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
	// Support CLI flag and use shared resolver
	var cfFlag string
	flag.StringVar(&cfFlag, "config", "", "Path to config YAML file")
	_ = flag.CommandLine.Parse(os.Args[1:])
	resolved, err := utils.ResolveConfigFile("flexible_recorder_config.yaml", os.Args[1:])
	if err != nil {
		logger.Fatal("failed to resolve flexible recorder config", logger.ErrorField(err))
	}
	logger.Info("Using config file", logger.String("path", resolved))
	viper.SetConfigFile(resolved)
	viper.SetConfigType("yaml")

	// Enable environment variable overrides
	viper.AutomaticEnv()
	viper.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))

	// Set environment variable bindings for key configuration
	viper.BindEnv("redis.addr", "REDIS_ADDR")
	viper.BindEnv("redis.password", "REDIS_PASSWORD")
	viper.BindEnv("redis.db", "REDIS_DB")
	viper.BindEnv("server.region", "SERVER_REGION")
	viper.BindEnv("server.workers", "SERVER_WORKERS")
	viper.BindEnv("server.task_ttl", "TASK_TTL")
	viper.BindEnv("web_recorder.base_url", "WEB_RECORDER_BASE_URL")
	viper.BindEnv("web_recorder.auth_token", "WEB_RECORDER_AUTH_TOKEN")

	// Read config file
	if err := viper.ReadInConfig(); err != nil {
		return fmt.Errorf("error reading config file: %v", err)
	}

	egress.InitEgressConfig(viper.ConfigFileUsed())

	// Unmarshal the entire config
	if err := viper.Unmarshal(&config); err != nil {
		return fmt.Errorf("error unmarshaling config: %v", err)
	}
	config.Redis.Addr = utils.ResolveRedisAddr(config.Redis.Addr)

	if config.Server.TaskTTL <= 60 {
		return fmt.Errorf("server.task_ttl must be greater than 60 seconds; got %d", config.Server.TaskTTL)
	}

	// Validate mandatory fields (no app_id needed for flexible recorder, the page to be recorded will handle RTC credentials)
	if strings.TrimSpace(config.Redis.Addr) == "" {
		return fmt.Errorf("redis.addr is required for flexible recorder")
	}

	// Set default worker count - for web recorder we don't need many "workers"
	// since the actual work is done by the web recorder engine
	if config.Server.Workers <= 0 {
		config.Server.Workers = 1 // Just 1 coordination process for web recorder
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

	logger.Info("Flexible Recorder configuration loaded",
		logger.String("web_recorder_url", config.WebRecorder.BaseURL),
		logger.String("redis_addr", config.Redis.Addr),
		logger.String("server_region", config.Server.Region),
		logger.Int("workers", config.Server.Workers))
	logger.Info("Flexible Recorder worker patterns", logger.String("patterns", fmt.Sprint(config.Server.WorkerPatterns)))

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
		config.Server.TaskTTL,
		config.Server.Region,
	)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := redisQueue.Ping(ctx); err != nil {
		return fmt.Errorf("failed to connect to Redis: %v", err)
	}

	logger.Info("Connected to Redis", logger.String("redis_addr", config.Redis.Addr))
	logger.Info("Flexible Recorder will handle patterns", logger.String("patterns", fmt.Sprint(config.Server.WorkerPatterns)))
	return nil
}

func initHealthManager() error {
	if redisQueue == nil {
		logger.Warn("Redis not configured, HealthManager disabled")
		return nil
	}

	// Generate unique pod ID for flexible recorder
	podID := fmt.Sprintf("flex-%s", utils.GenerateRandomID(8))
	appVersion := version.GetVersion()

	healthManager = health.NewHealthManager(redisQueue.Client(), podID, config.Server.Region, appVersion, int(egress.ModeWeb))

	// Register this pod as flexible recorder
	if err := healthManager.RegisterPod(config.Server.Workers); err != nil {
		return fmt.Errorf("failed to register flexible recorder pod: %v", err)
	}

	logger.Info("HealthManager initialized for flexible recorder",
		logger.String("pod_id", podID),
		logger.String("server_region", config.Server.Region))
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
		config.Server.Workers,
		redisQueue,
		podID,
		healthManager,
		egress.ModeWeb, // This is the key difference - web mode only
		webConfig,
		config.Server.WorkerPatterns,
	)

	// Start the worker manager
	go func() {
		wm.StartAll()
	}()

	logger.Info("Flexible Recorder worker manager started in ModeWeb",
		logger.String("pod_id", podID),
		logger.String("redis_addr", config.Redis.Addr),
		logger.String("patterns", fmt.Sprint(config.Server.WorkerPatterns)))
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
		"version":             version.GetVersion(),
		"mode":                "web-only",
		"web_recorder_status": webRecorderStatus,
		"web_recorder_url":    config.WebRecorder.BaseURL,
		"worker_patterns":     config.Server.WorkerPatterns,
	})
}

func startHealthCheckServer() {
	r := gin.New()
	r.Use(logger.GinRequestLogger(), gin.Recovery())

	// Health check
	r.GET("/health", healthCheckHandler)

	// Add health monitoring API if HealthManager is available
	if healthManager != nil {
		healthAPI := health.NewHealthAPI(healthManager)
		healthAPI.RegisterRoutes(r)
		logger.Info("Health monitoring API enabled for flexible recorder")
	}

	go func() {
		srv := &http.Server{
			Addr:    fmt.Sprintf(":%d", config.Server.HealthPort),
			Handler: r,
		}
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logger.Fatal("Failed to start health check server", logger.ErrorField(err))
		}
	}()

	logger.Info("Flexible recorder health server started", logger.Int("port", config.Server.HealthPort))
}

func main() {
	logger.Init("flexible-recorder")
	logger.Info("flexible-recorder starting", logger.String("version", version.FullVersion()))

	// Load configuration
	if err := loadConfig(); err != nil {
		logger.Fatal("Failed to load configuration", logger.ErrorField(err))
	}

	// Initialize Redis queue
	if err := initRedisQueue(); err != nil {
		logger.Fatal("Failed to initialize Redis", logger.ErrorField(err))
	}

	logger.Info("Web Recorder Engine URL configured", logger.String("url", config.WebRecorder.BaseURL))

	// Initialize health manager
	if err := initHealthManager(); err != nil {
		logger.Fatal("Failed to initialize health manager", logger.ErrorField(err))
	}

	// Start health check server
	startHealthCheckServer()

	// Start worker manager - REUSES existing egress manager but in ModeWeb
	startWorkerManager()

	// Wait for interrupt signal to gracefully shutdown the server
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	logger.Info("Flexible Recorder service stopping")

	// Cleanup health manager
	if healthManager != nil {
		logger.Info("Cleaning up health manager")
	}

	// Clean up worker processes (shared cleanup function)
	egress.CleanupWorkers()

	logger.Info("Flexible Recorder service stopped")
}
