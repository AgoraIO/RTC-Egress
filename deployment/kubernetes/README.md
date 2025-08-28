# Kubernetes Deployment Guide

This directory contains Kubernetes manifests for deploying the RTC Egress system with different scaling strategies as requested.

## Scaling Configuration Summary

Your exact requirements are implemented:

- **🔄 Egress (native)**: 3-10 pods (auto-scaling via HPA)
- **🔄 Webhook Notifier**: 2-4 pods (auto-scaling via HPA)  
- **🔒 Web Dispatch**: Fixed 6 pods (no auto-scaling)
- **🔒 Web Recorder**: Fixed 6 pods (3rd-party service)

## Architecture Overview

```
┌─────────────────────┐    ┌─────────────────────┐    ┌─────────────────────┐
│                     │    │                     │    │                     │
│   Egress Pods       │    │  Web Dispatch Pods  │    │  Web Recorder Pods  │
│   (3-10, Auto HPA)  │────│   (Fixed 6 pods)    │────│   (Fixed 6 pods)    │
│                     │    │                     │    │                     │
└─────────────────────┘    └─────────────────────┘    └─────────────────────┘
         │                           │                           │
         │                           │                           │
         └───────────────────────────┼───────────────────────────┘
                                     │
                      ┌─────────────────────┐    ┌─────────────────────┐
                      │                     │    │                     │
                      │   Redis Queue       │    │ Webhook Notifier    │
                      │   (Single instance) │    │ (2-4, Auto HPA)     │
                      │                     │    │                     │
                      └─────────────────────┘    └─────────────────────┘
```

## File Structure

```
deployment/kubernetes/
├── 01-namespace.yaml              # Namespace and Redis config
├── 02-redis.yaml                  # Redis deployment
├── 03-egress-deployment.yaml      # Egress service (auto-scaling)
├── 04-webhook-notifier-deployment.yaml # Webhook notifier (auto-scaling)
├── 05-web-recorder-deployment.yaml # Web recorder + dispatch (fixed)
├── 06-hpa-configs.yaml           # Auto-scaling configurations
├── 07-configmaps-secrets.yaml    # Configuration and secrets
├── 08-storage.yaml               # Persistent volumes
├── 09-monitoring.yaml            # Monitoring and policies
└── README.md                     # This file
```

## Quick Start

### 1. Prerequisites

```bash
# Ensure you have kubectl configured
kubectl cluster-info

# Ensure Horizontal Pod Autoscaler is enabled
kubectl get hpa -A

# Install metrics-server if not present (required for HPA)
kubectl apply -f https://github.com/kubernetes-sigs/metrics-server/releases/latest/download/components.yaml
```

### 2. Deploy Everything

```bash
# Apply all manifests in order
kubectl apply -f deployment/kubernetes/
```

### 3. Verify Deployment

```bash
# Check all pods are running
kubectl get pods -n rtc-egress

# Check HPA status
kubectl get hpa -n rtc-egress

# Check services
kubectl get svc -n rtc-egress
```

## Scaling Behavior

### Auto-Scaling Services (HPA Managed)

#### Egress Service (3-10 pods)
- **Min replicas**: 3
- **Max replicas**: 10
- **Scale up triggers**: 
  - CPU > 70%
  - Memory > 80%
  - Active tasks per pod > 20
- **Scale up behavior**: +50% or +2 pods (whichever is smaller)
- **Scale down behavior**: -25% or -1 pod (whichever is smaller)
- **Scale down delay**: 5 minutes stabilization

#### Webhook Notifier (2-4 pods)
- **Min replicas**: 2  
- **Max replicas**: 4
- **Scale up triggers**:
  - CPU > 60%
  - Memory > 70% 
  - Pending notifications per pod > 100
- **Scale up behavior**: +100% or +1 pod (faster scaling for notifications)
- **Scale down behavior**: -50% (3 minute delay)

### Fixed Scaling Services (No HPA)

#### Web Dispatch (Fixed 6 pods)
- **Replicas**: Exactly 6 (as requested)
- **No auto-scaling**: Maintains fixed count regardless of load
- **Updates**: Rolling updates with 50% max unavailable

#### Web Recorder (Fixed 6 pods)  
- **Replicas**: Exactly 6 (as requested)
- **No auto-scaling**: Fixed capacity for web recording service
- **Updates**: Rolling updates with 50% max unavailable

## Configuration

### Environment Variables

Set these in your CI/CD or manually update secrets:

```bash
# Required secrets (base64 encoded)
APP_ID="your_agora_app_id"
WEBHOOK_URL="https://your-webhook-endpoint.com"
WEB_RECORDER_AUTH_TOKEN="your_token"

# Optional
REDIS_PASSWORD="your_redis_password"
POD_REGION="us-west-1"
```

### Update Secrets

```bash
# Update Agora App ID
kubectl create secret generic agora-secrets \
  --from-literal=app-id="your_actual_app_id" \
  -n rtc-egress --dry-run=client -o yaml | kubectl apply -f -

# Update webhook URL
kubectl create secret generic webhook-secrets \
  --from-literal=url="https://your-actual-webhook.com" \
  --from-literal=auth-token="your_actual_token" \
  -n rtc-egress --dry-run=client -o yaml | kubectl apply -f -
```

### Customize Scaling

Edit `06-hpa-configs.yaml` to adjust scaling parameters:

```yaml
# For more aggressive egress scaling
maxReplicas: 20  # Increase max pods
metrics:
- type: Resource
  resource:
    name: cpu
    target:
      averageUtilization: 50  # Scale earlier (lower threshold)
```

## Monitoring

### Health Endpoints

```bash
# Check egress health
kubectl port-forward -n rtc-egress svc/egress 8182:8182
curl http://localhost:8182/health

# Check web dispatch health
kubectl port-forward -n rtc-egress svc/web-dispatch 8182:8182  
curl http://localhost:8182/health
```

# Check webhook notifier health
kubectl port-forward -n rtc-egress svc/webhook-notifier 8185:8185
curl http://localhost:8185/health

### Scaling Status

```bash
# Monitor HPA scaling decisions
kubectl describe hpa egress-hpa -n rtc-egress
kubectl describe hpa webhook-notifier-hpa -n rtc-egress

# Watch scaling in real-time
watch kubectl get hpa,pods -n rtc-egress
```

### Resource Usage

```bash
# Check resource consumption
kubectl top pods -n rtc-egress

# Check node resource allocation
kubectl describe nodes | grep -A 3 "Allocated resources"
```

## Task Distribution

### Redis Queue Patterns

Different services handle different task types:

- **Egress pods**: Handle `egress:record:*` and `egress:snapshot:*`
- **Web dispatch pods**: Handle `egress:web:record:*` and `egress:web:snapshot:*`
- **Webhook notifier**: Listens to all task state changes

### Load Balancing

```bash
# Check task distribution across egress pods
kubectl logs -n rtc-egress -l app=egress | grep "assigned task"

# Check web dispatch task processing
kubectl logs -n rtc-egress -l app=web-dispatch | grep "Processing"

# Monitor webhook notifications
kubectl logs -n rtc-egress -l app=webhook-notifier | grep "notification"
```

## Troubleshooting

### Common Issues

1. **HPA not scaling**
   ```bash
   # Check metrics-server is running
   kubectl get pods -n kube-system | grep metrics-server
   
   # Check HPA conditions
   kubectl describe hpa egress-hpa -n rtc-egress
   ```

2. **Web dispatch pods not processing web tasks**
   ```bash
   # Check Redis connection and patterns
   kubectl logs -n rtc-egress -l app=web-dispatch
   
   # Verify Redis queue patterns
   kubectl exec -n rtc-egress redis-xxx -- redis-cli KEYS "egress:web:*"
   ```

3. **Webhook notifications not working**
   ```bash
   # Check Redis keyspace notifications enabled
   kubectl exec -n rtc-egress redis-xxx -- redis-cli CONFIG GET notify-keyspace-events
   
   # Should return: Kg$ or similar
   # If not, enable:
   kubectl exec -n rtc-egress redis-xxx -- redis-cli CONFIG SET notify-keyspace-events Kg$
   ```

### Scaling Debugging

```bash
# Force scale for testing
kubectl scale deployment egress --replicas=5 -n rtc-egress

# Check why HPA isn't scaling
kubectl describe hpa egress-hpa -n rtc-egress

# Check resource metrics
kubectl top pods -n rtc-egress
```

### Performance Tuning

```bash
# Adjust C++ workers per egress pod
kubectl set env deployment/egress -n rtc-egress POD_WORKERS=8

# Monitor resource usage after changes
kubectl top pods -n rtc-egress --sort-by=cpu
```

## Production Considerations

### Resource Limits

Current resource allocations:
- **Egress**: 500m CPU, 512Mi RAM (request) → 2000m CPU, 2Gi RAM (limit)
- **Webhook Notifier**: 100m CPU, 128Mi RAM → 500m CPU, 512Mi RAM  
- **Web Dispatch**: 100m CPU, 128Mi RAM → 500m CPU, 512Mi RAM
- **Web Recorder**: 200m CPU, 256Mi RAM → 1000m CPU, 1Gi RAM

### High Availability

- **Pod Disruption Budgets**: Ensure minimum pods during updates
- **Anti-affinity**: Spread pods across nodes
- **Health checks**: Proper liveness and readiness probes
- **Rolling updates**: Zero-downtime deployments

### Security

- **Network policies**: Restrict inter-pod communication
- **Secrets management**: Never commit secrets to Git
- **RBAC**: Limit service account permissions
- **Image security**: Scan images for vulnerabilities

### Scaling Strategy

Your configuration provides:
- **Predictable web capacity**: Fixed 6 web dispatch + 6 web recorder pods
- **Dynamic native capacity**: 3-10 egress pods based on load
- **Notification resilience**: 2-4 webhook notifier pods
- **Cost optimization**: Scale down during low usage periods

This setup handles both predictable workloads (web recording) and variable workloads (native recording) efficiently while maintaining the exact pod counts you requested!