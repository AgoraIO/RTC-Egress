package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/AgoraIO/RTC-Egress/pkg/version"
	"github.com/AgoraIO/RTC-Egress/pkg/webhook"
	"github.com/redis/go-redis/v9"
	"github.com/spf13/viper"
)

// Config holds configuration for the webhook notifier service
type Config struct {
	Redis struct {
		Addr     string `mapstructure:"addr"`
		Password string `mapstructure:"password"`
		DB       int    `mapstructure:"db"`
	} `mapstructure:"redis"`
	Server struct {
		HealthPort int `mapstructure:"health_port"`
	} `mapstructure:"server"`
	Pod struct {
		Region string `mapstructure:"region"`
	} `mapstructure:"pod"`
	Webhook struct {
		URL               string   `mapstructure:"url"`
		Timeout           int      `mapstructure:"timeout"` // seconds
		MaxRetries        int      `mapstructure:"max_retries"`
		BaseRetryInterval int      `mapstructure:"base_retry_interval"` // seconds
		MaxRetryInterval  int      `mapstructure:"max_retry_interval"`  // seconds
		AuthToken         string   `mapstructure:"auth_token"`
		NotifyStates      []string `mapstructure:"notify_states"`
		DeliveredTTL      int      `mapstructure:"delivered_ttl"` // seconds, dedupe retention for terminal states
	} `mapstructure:"webhook"`
}

var (
	config      Config
	redisClient *redis.Client
	notifier    *webhook.WebhookNotifier
)

func loadConfig() error {
	viper.SetConfigName("webhook_notifier_config")
	viper.SetConfigType("yaml")
	viper.AddConfigPath("./config")
	viper.AddConfigPath("/etc/rtc_egress")
	viper.AddConfigPath("/opt/rtc_egress/config")
	viper.AddConfigPath("$HOME/.rtc_egress")

	// Enable environment variable overrides
	viper.AutomaticEnv()
	viper.SetEnvKeyReplacer(strings.NewReplacer(".", "_"))

	// Set environment variable bindings for key configuration
	viper.BindEnv("app_id", "APP_ID")
	viper.BindEnv("redis.addr", "REDIS_ADDR")
	viper.BindEnv("redis.password", "REDIS_PASSWORD")
	viper.BindEnv("redis.db", "REDIS_DB")
	viper.BindEnv("pod.region", "POD_REGION")
	viper.BindEnv("webhook.url", "WEBHOOK_URL")
	viper.BindEnv("webhook.auth_token", "WEBHOOK_AUTH_TOKEN")

	// Try to read config file - it's optional for containerized deployments
	if err := viper.ReadInConfig(); err != nil {
		log.Printf("Config file not found, using environment variables: %v", err)
	} else {
		log.Printf("Using config file: %s", viper.ConfigFileUsed())
	}

	// Unmarshal the entire config
	if err := viper.Unmarshal(&config); err != nil {
		return fmt.Errorf("error unmarshaling config: %v", err)
	}

	// Validate mandatory fields (no app_id needed for webhook notifier)

	if strings.TrimSpace(config.Redis.Addr) == "" {
		return fmt.Errorf("redis.addr is required for webhook notifications")
	}

	if strings.TrimSpace(config.Webhook.URL) == "" {
		return fmt.Errorf("webhook.url is required")
	}

	// Set defaults
	if config.Server.HealthPort <= 0 {
		config.Server.HealthPort = 8185 // Different from other services
	}

	if config.Webhook.Timeout <= 0 {
		config.Webhook.Timeout = 30 // 30 seconds default
	}

	if config.Webhook.MaxRetries <= 0 {
		config.Webhook.MaxRetries = 5 // 5 retries default
	}

	if config.Webhook.BaseRetryInterval <= 0 {
		config.Webhook.BaseRetryInterval = int(queue.LeaseRenewalInterval.Seconds()) // 15 seconds default
	}

	if config.Webhook.MaxRetryInterval <= 0 {
		config.Webhook.MaxRetryInterval = 300 // 5 minutes default
	}

	if len(config.Webhook.NotifyStates) == 0 {
		// Default to all important states
		config.Webhook.NotifyStates = []string{
			queue.TaskStateProcessing,
			queue.TaskStateStopped,
			queue.TaskStateFailed,
			queue.TaskStateTimeout,
		}
	}

	log.Printf("Configuration loaded for webhook notifier:")
	// No APP_ID needed for webhook notifier
	log.Printf("  Redis: %s (db: %d)", config.Redis.Addr, config.Redis.DB)
	log.Printf("  Webhook URL: %s", config.Webhook.URL)
	log.Printf("  Pod Region: %s", config.Pod.Region)
	log.Printf("  Notify States: %v", config.Webhook.NotifyStates)
	log.Printf("  Max Retries: %d", config.Webhook.MaxRetries)

	return nil
}

func initRedisClient() error {
	redisClient = redis.NewClient(&redis.Options{
		Addr:     config.Redis.Addr,
		Password: config.Redis.Password,
		DB:       config.Redis.DB,
	})

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := redisClient.Ping(ctx).Err(); err != nil {
		return fmt.Errorf("failed to connect to Redis: %v", err)
	}

	log.Printf("Connected to Redis at %s", config.Redis.Addr)
	return nil
}

func initWebhookNotifier() error {
	webhookConfig := webhook.WebhookConfig{
		URL:               config.Webhook.URL,
		Timeout:           time.Duration(config.Webhook.Timeout) * time.Second,
		MaxRetries:        config.Webhook.MaxRetries,
		BaseRetryInterval: time.Duration(config.Webhook.BaseRetryInterval) * time.Second,
		MaxRetryInterval:  time.Duration(config.Webhook.MaxRetryInterval) * time.Second,
		AuthToken:         config.Webhook.AuthToken,
		NotifyStates:      config.Webhook.NotifyStates,
		DeliveredTTL:      time.Duration(config.Webhook.DeliveredTTL) * time.Second,
	}

	notifier = webhook.NewWebhookNotifier(webhookConfig, redisClient)
	log.Printf("Webhook notifier initialized")
	return nil
}

func startWebhookNotifier() error {
	ctx := context.Background()
	if err := notifier.Start(ctx); err != nil {
		return fmt.Errorf("failed to start webhook notifier: %v", err)
	}
	log.Printf("Webhook notifier started successfully")
	return nil
}

func startHealthServer() {
	if config.Server.HealthPort <= 0 {
		log.Println("Health server disabled (port not configured)")
		return
	}

	go func() {
		mux := http.NewServeMux()

		mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")

			// Check Redis connectivity
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()

			status := "ok"
			statusCode := http.StatusOK
			var redisStatus string

			if err := redisClient.Ping(ctx).Err(); err != nil {
				status = "degraded"
				statusCode = http.StatusServiceUnavailable
				redisStatus = fmt.Sprintf("error: %v", err)
			} else {
				redisStatus = "ok"
			}

			response := map[string]interface{}{
				"status":        status,
				"version":       version.GetVersion(),
				"service":       "webhook-notifier",
				"redis_status":  redisStatus,
				"webhook_url":   config.Webhook.URL,
				"notify_states": config.Webhook.NotifyStates,
			}

			w.WriteHeader(statusCode)
			json.NewEncoder(w).Encode(response)
		})

		// Webhook test endpoint
		mux.HandleFunc("/test", func(w http.ResponseWriter, r *http.Request) {
			if r.Method != "POST" {
				http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
				return
			}

			// Create a test webhook payload
			testPayload := webhook.WebhookPayload{
				TaskID:    fmt.Sprintf("test_%s", utils.GenerateRandomID(8)),
				RequestID: fmt.Sprintf("req_%s", utils.GenerateRandomID(8)),
				Cmd:       "test",
				Action:    "start",
				Channel:   "test_channel",
				State:     queue.TaskStateProcessing,
				CreatedAt: time.Now(),
				Timestamp: time.Now(),
			}

			// Test the webhook endpoint directly
			success := testWebhookEndpoint(testPayload)

			response := map[string]interface{}{
				"test_payload": testPayload,
				"success":      success,
			}

			if success {
				w.WriteHeader(http.StatusOK)
			} else {
				w.WriteHeader(http.StatusBadGateway)
			}
			json.NewEncoder(w).Encode(response)
		})

		server := &http.Server{
			Addr:    fmt.Sprintf(":%d", config.Server.HealthPort),
			Handler: mux,
		}

		log.Printf("Health server starting on port %d", config.Server.HealthPort)
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("Failed to start health server: %v", err)
		}
	}()
}

// testWebhookEndpoint tests the webhook endpoint with a test payload
func testWebhookEndpoint(testPayload webhook.WebhookPayload) bool {
	// Create a temporary notifier for testing
	testConfig := webhook.WebhookConfig{
		URL:       config.Webhook.URL,
		Timeout:   time.Duration(config.Webhook.Timeout) * time.Second,
		AuthToken: config.Webhook.AuthToken,
	}
	testNotifier := webhook.NewWebhookNotifier(testConfig, nil)

	if testNotifier == nil {
		return false
	}

	// Actually send the test webhook using HTTP client directly
	ctx, cancel := context.WithTimeout(context.Background(), testConfig.Timeout)
	defer cancel()

	payloadBytes, err := json.Marshal(testPayload)
	if err != nil {
		log.Printf("Error marshaling test payload: %v", err)
		return false
	}

	req, err := http.NewRequestWithContext(ctx, "POST", testConfig.URL, bytes.NewBuffer(payloadBytes))
	if err != nil {
		log.Printf("Error creating test request: %v", err)
		return false
	}

	req.Header.Set("Content-Type", "application/json")
	if testConfig.AuthToken != "" {
		req.Header.Set("Authorization", "Bearer "+testConfig.AuthToken)
	}

	client := &http.Client{Timeout: testConfig.Timeout}
	resp, err := client.Do(req)
	if err != nil {
		log.Printf("Error sending test webhook: %v", err)
		return false
	}
	defer resp.Body.Close()

	log.Printf("Test webhook sent successfully (status: %d)", resp.StatusCode)
	return resp.StatusCode >= 200 && resp.StatusCode < 300
}

func main() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
	log.Println("Starting Webhook Notifier Service")

	// Load configuration
	if err := loadConfig(); err != nil {
		log.Fatalf("Error loading config: %v", err)
	}

	// Initialize Redis client
	if err := initRedisClient(); err != nil {
		log.Fatalf("Error initializing Redis: %v", err)
	}

	// Initialize webhook notifier
	if err := initWebhookNotifier(); err != nil {
		log.Fatalf("Error initializing webhook notifier: %v", err)
	}

	// Start health server
	startHealthServer()

	// Start webhook notifier
	if err := startWebhookNotifier(); err != nil {
		log.Fatalf("Error starting webhook notifier: %v", err)
	}

	log.Println("Webhook Notifier Service is running...")
	log.Printf("Health endpoint: http://localhost:%d/health", config.Server.HealthPort)
	log.Printf("Test endpoint: http://localhost:%d/test", config.Server.HealthPort)

	// Wait for interrupt signal to gracefully shut down
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	log.Println("Shutting down Webhook Notifier Service...")

	// Stop webhook notifier
	if notifier != nil {
		notifier.Stop()
	}

	// Close Redis connection
	if redisClient != nil {
		redisClient.Close()
	}

	log.Println("Webhook Notifier Service stopped")
}
