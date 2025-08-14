package health

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/shirou/gopsutil/v3/cpu"
	"github.com/shirou/gopsutil/v3/mem"
)

const (
	PodHealthTTL      = 60 * time.Second // Pod registration TTL
	HeartbeatInterval = 15 * time.Second // How often pods report health
	StatsInterval     = 5 * time.Second  // How often to calculate regional stats
)

// PodInfo represents a pod's configuration and status
type PodInfo struct {
	PodID          string    `json:"pod_id"`
	Region         string    `json:"region"`
	MaxWorkers     int       `json:"max_workers"`
	CurrentWorkers int       `json:"current_workers"`
	Status         string    `json:"status"` // "healthy", "degraded", "unhealthy"
	LastHeartbeat  time.Time `json:"last_heartbeat"`
	Version        string    `json:"version"`
	StartedAt      time.Time `json:"started_at"`
}

// CapacityMetrics represents real-time capacity information
type CapacityMetrics struct {
	ActiveTasks      int       `json:"active_tasks"`
	AvailableWorkers int       `json:"available_workers"`
	QueueBacklog     int       `json:"queue_backlog"`
	CPUUsage         float64   `json:"cpu_usage"`
	MemoryUsage      float64   `json:"memory_usage"`
	LastUpdated      time.Time `json:"last_updated"`
}

// RegionalStats represents aggregated regional metrics
type RegionalStats struct {
	TotalPods         int       `json:"total_pods"`
	HealthyPods       int       `json:"healthy_pods"`
	TotalWorkers      int       `json:"total_workers"`
	ActiveTasks       int       `json:"active_tasks"`
	AvailableCapacity int       `json:"available_capacity"`
	QueueBacklog      int       `json:"queue_backlog"`
	AvgCPU            float64   `json:"avg_cpu"`
	AvgMemory         float64   `json:"avg_memory"`
	LastUpdated       time.Time `json:"last_updated"`
}

// QueueMetrics represents task queue depths
type QueueMetrics struct {
	Queues      map[string]int `json:"queues"` // "region:type" -> queue_depth
	LastUpdated time.Time      `json:"last_updated"`
}

// HealthManager handles pod registration, heartbeats, and statistics calculation
// It ONLY collects and reports metrics - never dispatches tasks
type HealthManager struct {
	client    *redis.Client
	podID     string
	region    string
	version   string
	startTime time.Time
}

func NewHealthManager(redisClient *redis.Client, podID, region, version string) *HealthManager {
	return &HealthManager{
		client:    redisClient,
		podID:     podID,
		region:    region,
		version:   version,
		startTime: time.Now(),
	}
}

// RegisterPod registers this pod with the health manager
func (hm *HealthManager) RegisterPod(maxWorkers int) error {
	podInfo := PodInfo{
		PodID:          hm.podID,
		Region:         hm.region,
		MaxWorkers:     maxWorkers,
		CurrentWorkers: maxWorkers, // Assume all workers are running initially
		Status:         "healthy",
		LastHeartbeat:  time.Now(),
		Version:        hm.version,
		StartedAt:      hm.startTime,
	}

	data, err := json.Marshal(podInfo)
	if err != nil {
		return fmt.Errorf("failed to marshal pod info: %v", err)
	}

	key := fmt.Sprintf("egress:health:pod:%s", hm.podID)
	ctx := context.Background()

	err = hm.client.Set(ctx, key, data, PodHealthTTL).Err()
	if err != nil {
		return fmt.Errorf("failed to register pod: %v", err)
	}

	log.Printf("Registered pod %s in region %s with %d workers", hm.podID, hm.region, maxWorkers)
	return nil
}

// StartHeartbeat starts the heartbeat process
func (hm *HealthManager) StartHeartbeat(maxWorkers int, getActiveTaskCount func() int) {
	ticker := time.NewTicker(HeartbeatInterval)

	go func() {
		defer ticker.Stop()

		for range ticker.C {
			if err := hm.sendHeartbeat(maxWorkers, getActiveTaskCount); err != nil {
				log.Printf("Failed to send heartbeat: %v", err)
			}
		}
	}()

	log.Printf("Started health heartbeat for pod %s", hm.podID)
}

// sendHeartbeat updates pod health and capacity metrics
func (hm *HealthManager) sendHeartbeat(maxWorkers int, getActiveTaskCount func() int) error {
	ctx := context.Background()
	now := time.Now()

	// Get system metrics
	cpuPercent, _ := cpu.Percent(time.Second, false)
	memInfo, _ := mem.VirtualMemory()

	activeTaskCount := getActiveTaskCount()
	availableWorkers := maxWorkers - activeTaskCount

	// Update pod registration
	podInfo := PodInfo{
		PodID:          hm.podID,
		Region:         hm.region,
		MaxWorkers:     maxWorkers,
		CurrentWorkers: maxWorkers,
		Status:         hm.calculatePodStatus(cpuPercent[0], memInfo.UsedPercent),
		LastHeartbeat:  now,
		Version:        hm.version,
		StartedAt:      hm.startTime,
	}

	podData, _ := json.Marshal(podInfo)
	podKey := fmt.Sprintf("egress:health:pod:%s", hm.podID)

	// Update capacity metrics
	capacityMetrics := CapacityMetrics{
		ActiveTasks:      activeTaskCount,
		AvailableWorkers: availableWorkers,
		QueueBacklog:     0, // Will be calculated by regional stats
		CPUUsage:         cpuPercent[0],
		MemoryUsage:      memInfo.UsedPercent,
		LastUpdated:      now,
	}

	capacityData, _ := json.Marshal(capacityMetrics)
	capacityKey := fmt.Sprintf("egress:health:capacity:%s", hm.podID)

	// Atomic update
	pipe := hm.client.TxPipeline()
	pipe.Set(ctx, podKey, podData, PodHealthTTL)
	pipe.Set(ctx, capacityKey, capacityData, PodHealthTTL)

	_, err := pipe.Exec(ctx)
	return err
}

// calculatePodStatus determines pod health based on system metrics
func (hm *HealthManager) calculatePodStatus(cpuPercent, memPercent float64) string {
	if cpuPercent > 90 || memPercent > 90 {
		return "unhealthy"
	}
	if cpuPercent > 75 || memPercent > 75 {
		return "degraded"
	}
	return "healthy"
}

// StartRegionalStatsCalculation starts the background regional statistics calculation
func (hm *HealthManager) StartRegionalStatsCalculation() {
	ticker := time.NewTicker(StatsInterval)

	go func() {
		defer ticker.Stop()

		for range ticker.C {
			if err := hm.calculateRegionalStats(); err != nil {
				log.Printf("Failed to calculate regional stats: %v", err)
			}

			if err := hm.calculateQueueMetrics(); err != nil {
				log.Printf("Failed to calculate queue metrics: %v", err)
			}
		}
	}()

	log.Printf("Started regional statistics calculation")
}

// calculateRegionalStats aggregates pod metrics by region
func (hm *HealthManager) calculateRegionalStats() error {
	ctx := context.Background()

	// Get all pod registrations
	podKeys, err := hm.client.Keys(ctx, "egress:health:pod:*").Result()
	if err != nil {
		return err
	}

	// Group pods by region
	regionPods := make(map[string][]PodInfo)
	regionCapacity := make(map[string][]CapacityMetrics)

	for _, podKey := range podKeys {
		// Get pod info
		podData, err := hm.client.Get(ctx, podKey).Result()
		if err != nil {
			continue
		}

		var podInfo PodInfo
		if err := json.Unmarshal([]byte(podData), &podInfo); err != nil {
			continue
		}

		// Get capacity info
		podID := strings.TrimPrefix(podKey, "egress:health:pod:")
		capacityKey := fmt.Sprintf("egress:health:capacity:%s", podID)
		capacityData, err := hm.client.Get(ctx, capacityKey).Result()
		if err != nil {
			continue
		}

		var capacity CapacityMetrics
		if err := json.Unmarshal([]byte(capacityData), &capacity); err != nil {
			continue
		}

		region := podInfo.Region
		if region == "" {
			region = "global"
		}

		regionPods[region] = append(regionPods[region], podInfo)
		regionCapacity[region] = append(regionCapacity[region], capacity)
	}

	// Calculate regional aggregates
	for region, pods := range regionPods {
		capacity := regionCapacity[region]

		stats := RegionalStats{
			TotalPods:         len(pods),
			HealthyPods:       0,
			TotalWorkers:      0,
			ActiveTasks:       0,
			AvailableCapacity: 0,
			QueueBacklog:      0,
			AvgCPU:            0,
			AvgMemory:         0,
			LastUpdated:       time.Now(),
		}

		var totalCPU, totalMemory float64

		for i, pod := range pods {
			if pod.Status == "healthy" {
				stats.HealthyPods++
			}

			stats.TotalWorkers += pod.MaxWorkers

			if i < len(capacity) {
				cap := capacity[i]
				stats.ActiveTasks += cap.ActiveTasks
				stats.AvailableCapacity += cap.AvailableWorkers
				totalCPU += cap.CPUUsage
				totalMemory += cap.MemoryUsage
			}
		}

		if len(capacity) > 0 {
			stats.AvgCPU = totalCPU / float64(len(capacity))
			stats.AvgMemory = totalMemory / float64(len(capacity))
		}

		// Store regional stats
		statsData, _ := json.Marshal(stats)
		statsKey := fmt.Sprintf("egress:health:region:%s", region)
		hm.client.Set(ctx, statsKey, statsData, 2*StatsInterval)
	}

	return nil
}

// calculateQueueMetrics calculates queue depths per region/type
func (hm *HealthManager) calculateQueueMetrics() error {
	ctx := context.Background()

	// Get all queue keys
	queueKeys, err := hm.client.Keys(ctx, "egress:*:*:channel:*").Result()
	if err != nil {
		return err
	}

	queueDepths := make(map[string]int)

	for _, queueKey := range queueKeys {
		// Parse queue key: egress:{region}:{type}:channel:{channel}
		parts := strings.Split(queueKey, ":")
		if len(parts) < 4 {
			continue
		}

		region := parts[1]
		taskType := parts[2]

		// Get queue length
		length, err := hm.client.LLen(ctx, queueKey).Result()
		if err != nil {
			continue
		}

		key := fmt.Sprintf("%s:%s", region, taskType)
		queueDepths[key] += int(length)
	}

	// Store queue metrics
	queueMetrics := QueueMetrics{
		Queues:      queueDepths,
		LastUpdated: time.Now(),
	}

	data, _ := json.Marshal(queueMetrics)
	return hm.client.Set(ctx, "egress:health:queues", data, 2*StatsInterval).Err()
}

// GetRegionalStats returns current regional statistics
func (hm *HealthManager) GetRegionalStats(region string) (*RegionalStats, error) {
	ctx := context.Background()
	key := fmt.Sprintf("egress:health:region:%s", region)

	data, err := hm.client.Get(ctx, key).Result()
	if err != nil {
		return nil, err
	}

	var stats RegionalStats
	err = json.Unmarshal([]byte(data), &stats)
	return &stats, err
}

// GetQueueMetrics returns current queue metrics
func (hm *HealthManager) GetQueueMetrics() (*QueueMetrics, error) {
	ctx := context.Background()

	data, err := hm.client.Get(ctx, "egress:health:queues").Result()
	if err != nil {
		return nil, err
	}

	var metrics QueueMetrics
	err = json.Unmarshal([]byte(data), &metrics)
	return &metrics, err
}

// GetAllRegions returns list of all active regions
func (hm *HealthManager) GetAllRegions() ([]string, error) {
	ctx := context.Background()
	keys, err := hm.client.Keys(ctx, "egress:health:region:*").Result()
	if err != nil {
		return nil, err
	}

	var regions []string
	for _, key := range keys {
		region := strings.TrimPrefix(key, "egress:health:region:")
		regions = append(regions, region)
	}

	return regions, nil
}
