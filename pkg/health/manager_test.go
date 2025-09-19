package health

import "testing"

func TestCalculatePodStatus(t *testing.T) {
    hm := &HealthManager{}

    if got := hm.calculatePodStatus(50, 50); got != "healthy" {
        t.Fatalf("expected healthy, got %s", got)
    }
    if got := hm.calculatePodStatus(80, 50); got != "degraded" {
        t.Fatalf("expected degraded for high CPU, got %s", got)
    }
    if got := hm.calculatePodStatus(50, 85); got != "degraded" {
        t.Fatalf("expected degraded for high memory, got %s", got)
    }
    if got := hm.calculatePodStatus(95, 20); got != "unhealthy" {
        t.Fatalf("expected unhealthy for very high CPU, got %s", got)
    }
    if got := hm.calculatePodStatus(10, 92); got != "unhealthy" {
        t.Fatalf("expected unhealthy for very high memory, got %s", got)
    }
}

