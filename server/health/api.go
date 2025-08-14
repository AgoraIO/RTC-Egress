package health

import (
	"net/http"

	"github.com/gin-gonic/gin"
)

// HealthAPI provides HTTP endpoints for health and capacity monitoring
type HealthAPI struct {
	healthManager *HealthManager
}

func NewHealthAPI(healthManager *HealthManager) *HealthAPI {
	return &HealthAPI{
		healthManager: healthManager,
	}
}

// RegisterRoutes adds health monitoring routes to the gin router
func (api *HealthAPI) RegisterRoutes(r *gin.Engine) {
	health := r.Group("/health/v1")
	{
		// Overall system health
		health.GET("/status", api.getSystemStatus)

		// Regional statistics
		health.GET("/regions", api.getAllRegions)
		health.GET("/regions/:region", api.getRegionalStats)

		// Queue monitoring
		health.GET("/queues", api.getQueueMetrics)

		// Pod information
		health.GET("/pods", api.getAllPods)
		health.GET("/pods/:podId", api.getPodDetails)

		// Capacity planning
		health.GET("/capacity", api.getCapacityOverview)
		health.GET("/capacity/:region", api.getRegionalCapacity)
	}
}

// getSystemStatus returns overall system health summary
func (api *HealthAPI) getSystemStatus(c *gin.Context) {
	regions, err := api.healthManager.GetAllRegions()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	systemStatus := map[string]interface{}{
		"status":    "healthy",
		"regions":   len(regions),
		"timestamp": api.healthManager.startTime,
	}

	// Aggregate health across all regions
	totalPods := 0
	healthyPods := 0
	totalCapacity := 0
	usedCapacity := 0

	for _, region := range regions {
		stats, err := api.healthManager.GetRegionalStats(region)
		if err != nil {
			continue
		}

		totalPods += stats.TotalPods
		healthyPods += stats.HealthyPods
		totalCapacity += stats.TotalWorkers
		usedCapacity += stats.ActiveTasks
	}

	systemStatus["total_pods"] = totalPods
	systemStatus["healthy_pods"] = healthyPods
	systemStatus["capacity_utilization"] = float64(usedCapacity) / float64(totalCapacity) * 100

	if float64(healthyPods)/float64(totalPods) < 0.8 {
		systemStatus["status"] = "degraded"
	}

	c.JSON(http.StatusOK, systemStatus)
}

// getAllRegions returns list of all active regions
func (api *HealthAPI) getAllRegions(c *gin.Context) {
	regions, err := api.healthManager.GetAllRegions()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	regionDetails := make([]map[string]interface{}, 0, len(regions))

	for _, region := range regions {
		stats, err := api.healthManager.GetRegionalStats(region)
		if err != nil {
			continue
		}

		regionDetails = append(regionDetails, map[string]interface{}{
			"region":             region,
			"total_pods":         stats.TotalPods,
			"healthy_pods":       stats.HealthyPods,
			"total_workers":      stats.TotalWorkers,
			"active_tasks":       stats.ActiveTasks,
			"available_capacity": stats.AvailableCapacity,
			"capacity_utilization": func() float64 {
				if stats.TotalWorkers == 0 {
					return 0
				}
				return float64(stats.ActiveTasks) / float64(stats.TotalWorkers) * 100
			}(),
			"avg_cpu":    stats.AvgCPU,
			"avg_memory": stats.AvgMemory,
			"status": func() string {
				if float64(stats.HealthyPods)/float64(stats.TotalPods) < 0.5 {
					return "unhealthy"
				}
				if float64(stats.HealthyPods)/float64(stats.TotalPods) < 0.8 {
					return "degraded"
				}
				return "healthy"
			}(),
		})
	}

	c.JSON(http.StatusOK, gin.H{
		"regions": regionDetails,
		"total":   len(regions),
	})
}

// getRegionalStats returns detailed statistics for a specific region
func (api *HealthAPI) getRegionalStats(c *gin.Context) {
	region := c.Param("region")

	stats, err := api.healthManager.GetRegionalStats(region)
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "Region not found"})
		return
	}

	c.JSON(http.StatusOK, stats)
}

// getQueueMetrics returns current queue depths and backlogs
func (api *HealthAPI) getQueueMetrics(c *gin.Context) {
	metrics, err := api.healthManager.GetQueueMetrics()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	// Add total backlog calculation
	totalBacklog := 0
	for _, depth := range metrics.Queues {
		totalBacklog += depth
	}

	response := map[string]interface{}{
		"queues":        metrics.Queues,
		"total_backlog": totalBacklog,
		"last_updated":  metrics.LastUpdated,
	}

	c.JSON(http.StatusOK, response)
}

// getAllPods returns information about all registered pods
func (api *HealthAPI) getAllPods(c *gin.Context) {
	// This would require additional Redis queries to get all pod information
	// Implementation similar to calculateRegionalStats but returning pod details
	c.JSON(http.StatusOK, gin.H{"message": "Pod listing endpoint - implementation needed"})
}

// getPodDetails returns detailed information about a specific pod
func (api *HealthAPI) getPodDetails(c *gin.Context) {
	podID := c.Param("podId")

	// Get pod info and capacity from Redis
	// Implementation would fetch specific pod data
	c.JSON(http.StatusOK, gin.H{
		"pod_id":  podID,
		"message": "Pod details endpoint - implementation needed",
	})
}

// getCapacityOverview returns capacity planning information across all regions
func (api *HealthAPI) getCapacityOverview(c *gin.Context) {
	regions, err := api.healthManager.GetAllRegions()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	queueMetrics, err := api.healthManager.GetQueueMetrics()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	capacityOverview := map[string]interface{}{
		"regions":              make([]map[string]interface{}, 0),
		"global_queue_backlog": 0,
		"recommendations":      make([]string, 0),
	}

	totalGlobalBacklog := 0
	recommendations := make([]string, 0)

	for _, region := range regions {
		stats, err := api.healthManager.GetRegionalStats(region)
		if err != nil {
			continue
		}

		utilization := float64(stats.ActiveTasks) / float64(stats.TotalWorkers) * 100

		regionCapacity := map[string]interface{}{
			"region":               region,
			"capacity_utilization": utilization,
			"available_capacity":   stats.AvailableCapacity,
			"queue_backlog":        stats.QueueBacklog,
			"recommendation":       "",
		}

		// Generate recommendations
		if utilization > 90 {
			regionCapacity["recommendation"] = "Critical: Scale up immediately"
			recommendations = append(recommendations, region+": Scale up immediately")
		} else if utilization > 75 {
			regionCapacity["recommendation"] = "Warning: Consider scaling up"
			recommendations = append(recommendations, region+": Consider scaling up")
		} else if utilization < 20 {
			regionCapacity["recommendation"] = "Info: Consider scaling down"
		}

		capacityOverview["regions"] = append(capacityOverview["regions"].([]map[string]interface{}), regionCapacity)
	}

	// Calculate global queue backlog
	for queue, depth := range queueMetrics.Queues {
		if queue == "global:snapshot" || queue == "global:record" {
			totalGlobalBacklog += depth
		}
	}

	capacityOverview["global_queue_backlog"] = totalGlobalBacklog
	capacityOverview["recommendations"] = recommendations

	c.JSON(http.StatusOK, capacityOverview)
}

// getRegionalCapacity returns detailed capacity information for a specific region
func (api *HealthAPI) getRegionalCapacity(c *gin.Context) {
	region := c.Param("region")

	stats, err := api.healthManager.GetRegionalStats(region)
	if err != nil {
		c.JSON(http.StatusNotFound, gin.H{"error": "Region not found"})
		return
	}

	queueMetrics, err := api.healthManager.GetQueueMetrics()
	if err != nil {
		c.JSON(http.StatusInternalServerError, gin.H{"error": err.Error()})
		return
	}

	// Calculate regional queue backlogs
	snapshotBacklog := 0
	recordBacklog := 0

	for queue, depth := range queueMetrics.Queues {
		if queue == region+":snapshot" {
			snapshotBacklog = depth
		}
		if queue == region+":record" {
			recordBacklog = depth
		}
	}

	utilization := float64(stats.ActiveTasks) / float64(stats.TotalWorkers) * 100

	response := map[string]interface{}{
		"region":               region,
		"total_workers":        stats.TotalWorkers,
		"active_tasks":         stats.ActiveTasks,
		"available_capacity":   stats.AvailableCapacity,
		"capacity_utilization": utilization,
		"queue_backlogs": map[string]int{
			"snapshot": snapshotBacklog,
			"record":   recordBacklog,
		},
		"resource_usage": map[string]float64{
			"avg_cpu":    stats.AvgCPU,
			"avg_memory": stats.AvgMemory,
		},
		"health": map[string]interface{}{
			"total_pods":   stats.TotalPods,
			"healthy_pods": stats.HealthyPods,
			"health_ratio": float64(stats.HealthyPods) / float64(stats.TotalPods) * 100,
		},
		"scaling_recommendation": func() string {
			if utilization > 90 {
				return "scale_up_critical"
			} else if utilization > 75 {
				return "scale_up_warning"
			} else if utilization < 20 {
				return "scale_down"
			}
			return "optimal"
		}(),
		"last_updated": stats.LastUpdated,
	}

	c.JSON(http.StatusOK, response)
}
