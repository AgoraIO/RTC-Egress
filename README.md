# Agora RTC Egress

A high-performance egress service solution for Agora RTC streams, with support for saving video frames as images/recording mp4/hls and uploading to S3 or compatible storage.

## Features

- Record Agora RTC streams with configurable quality
- Save video frames as images at regular intervals
- HTTP API for controlling the recording process
- Built-in health checks
- S3 integration for storing recorded media
- Containerized deployment with Docker or Kubernetes

## Architecture

The system consists of five main components:

1. **Go HTTP Server**: Provides REST API for controlling the recorder
2. **Go Egress Manager**: Manages the recording process
3. **Go Flexible Recorder**: Flexible recorder for recording Agora RTC streams
4. **Go Webhook Notifier**: Notifies external systems about recording events
5. **Go Uploader**: Uploads recorded media to S3 or compatible storage

and using C++/C to handle the low-level RTC streaming and frame processing

## Prerequisites

- Agora Developer Account and App ID
- A Redis service
- (Optional) AWS S3 credentials for cloud storage
- (Optional) Docker and Docker Compose
- (Optional) Kubernetes for production deployment, check https://github.com/AgoraIO/Helm-Charts for more details

## Quick Start

### Using Pre-built Docker Images (Recommended)

1. **With External Redis:**
   ```bash
   git clone https://github.com/AgoraIO/RTC-Egress.git
   cd RTC-Egress

   # Create/update .env with your values
   cat <<'ENV' > .env
   AGORA_APP_ID=your-agora-app-id
   REDIS_HOST=your-redis-host
   REDIS_PORT=6379
   # Optional overrides
   # REDIS_PASSWORD=
   # REDIS_DB=0
   # IMAGE_TAG=latest
   # S3_BUCKET=
   # S3_REGION=
   # S3_ACCESS_KEY=
   # S3_SECRET_KEY=
   # S3_ENDPOINT=
   # WEBHOOK_URL=
   ENV

   # Copy default config (edit if you need custom settings)
   cp config/egress_config.yaml.example config/egress_config.yaml

   # Ensure host directories exist for bind mounts
   mkdir -p recordings snapshots logs web_recordings

   # Make sure the expected Docker network exists (no-op if it already does)
   docker network create ag_rtc_egress_external_network 2>/dev/null || true

   # Load env vars locally (Compose also reads .env automatically)
   set -a; source .env; set +a

   # Start the stack
   docker compose --env-file .env \
     -f deployment/docker_compose/docker-compose-external-redis.yml up -d
   ```

   The services prefer `REDIS_ADDR` when set; otherwise they combine
   `REDIS_HOST`/`REDIS_PORT` automatically. Optional values such as `S3_*`,
   `WEBHOOK_URL`, or `IMAGE_TAG` are passed through unchanged.

2. **With Built-in Redis (Development):**
   ```bash
   # Clone the repository for config files
   git clone https://github.com/AgoraIO/RTC-Egress.git
   cd RTC-Egress

   # Configure the application
   cp config/egress_config.yaml.example config/egress_config.yaml
   cp config/api_server_config.yaml.example config/api_server_config.yaml
   cp config/flexible_recorder_config.yaml.example config/flexible_recorder_config.yaml
   cp config/webhook_config.yaml.example config/webhook_config.yaml
   cp config/uploader_config.yaml.example config/uploader_config.yaml
   # Edit config/*_config.yaml files with your Agora credentials/S3 credentials and other settings

   # Prepare bind-mount directories
   mkdir -p ./{recordings,snapshots,logs,web_recordings}

   # Run with built-in Redis
   docker compose -f deployment/docker_compose/docker-compose-redis-debug.yml up -d
   ```

## Configuration

Edit `config/egress_config.yaml` to configure the application:

```yaml
# Agora App ID and Token
app_id: "YOUR_AGORA_APP_ID"
access_token: "YOUR_AGORA_ACCESS_TOKEN"

# Server configuration
health_port: 8182  # Health check endpoint
api_port: 8080     # API server port
template_port: 3000 # Web interface port

# Redis configuration
redis:
  addr: "your-redis-host:6379"

# Recording settings
recording:
  output_dir: "/recordings"  # Where to store recordings
  width: 1280               # Video width
  height: 720               # Video height
  fps: 30                   # Frames per second
  interval_in_ms: 20000  # Save frame every 20 seconds
```

Edit `config/api_server_config.yaml` to configure the API server:

```yaml
# Redis configuration
redis:
  addr: "your-redis-host:6379"

# API server configuration
api:
  port: 8080
```

Edit `config/flexible_recorder_config.yaml` to configure the flexible recorder:

```yaml
# Redis configuration
redis:
  addr: "your-redis-host:6379"

# Flexible recorder configuration(Web Recorder, optional)
web_recorder:
  base_url: "https://www.youtube.com/"
```

Edit `config/webhook_config.yaml` to configure the webhook notifier:

```yaml
# Redis configuration
redis:
  addr: "your-redis-host:6379"

# Webhook notifier configuration (optional)
webhook:
  url: "your-webhook-url"
```

Edit `config/uploader_config.yaml` to configure the uploader:

```yaml
# Redis configuration
redis:
  addr: "your-redis-host:6379"

# S3 Configuration (optional)
s3:
  bucket: "your-s3-bucket"
  region: "us-west-2"
  access_key: "YOUR_AWS_ACCESS_KEY"
  secret_key: "YOUR_AWS_SECRET_KEY"
  # endpoint: "https://s3.us-west-2.amazonaws.com"  # Optional custom endpoint
```

## API Endpoints
Refer to `designs/restful_api_design.md`

## Building from Source

### Prerequisites

- C++17 compatible compiler
- CMake 3.10+
- Go 1.21+
- FFmpeg libraries
- Agora RTC SDK

### Build Steps

1. **Clone the repository**
   ```bash
   git clone https://github.com/AgoraIO/RTC-Egress.git
   cd RTC-Egress
   ```

2. **Build and run the application**
   ```bash
    ./build.sh local && ./build.sh run all
   ```

## Docker Deployment

### Using Pre-built Images

The easiest way to deploy is using the pre-built images from GitHub Container Registry:

```bash
# Using Docker Run
docker run -d \
  --name rtc-egress \
  -p 8080:8080 \
  -p 8182:8182 \
  -p 3000:3000 \
  -v ./config:/opt/rtc_egress/config:ro \
  -v ./recordings:/recordings \
  -v ./snapshots:/snapshots \
  -e REDIS_ADDR="your-redis-host:6379" \
  -e AGORA_APP_ID="your-agora-app-id" \
  ghcr.io/AgoraIO/RTC-Egress:latest
```

### Building Locally (Development)

For development or customization:

```bash
# Build production image
docker build -f Dockerfile.prod -t rtc-egress:latest .

# Build debug image
docker build -f Dockerfile.debug -t rtc-egress:debug .
```

### Available Images

| Image | Purpose | Architecture | Use Case |
|-------|---------|--------------|----------|
| `ghcr.io/AgoraIO/RTC-Egress:latest` | Production | linux/amd64 | Production deployments |
| `ghcr.io/AgoraIO/RTC-Egress:v1.x.x` | Specific version | linux/amd64 | Version-pinned deployments |
| `ag_rtc_egress:debug` | Local debug | linux/amd64 | Development with debugging tools |

> **Note**: Only x86_64 (amd64) architecture is supported due to Agora SDK limitations.

### Start the services
```bash
# With external Redis
cd deployment/docker_compose
docker compose -f docker-compose-external-redis.yml up -d

# With built-in Redis (development)
docker compose -f docker-compose-redis-debug.yml up -d
```

### View logs
```bash
docker compose logs -f
```

### Health Checks

```bash
# Check if service is healthy
curl http://localhost:8182/health

# Expected response
{"status":"ok","version":"1.2.11"}
```

## Monitoring

The application exposes the following endpoints for monitoring:

- `http://localhost:8182/health` - Health check
- `http://localhost:8182/metrics` - Metrics
- `http://localhost:8080/egress/v1/{app_id}/tasks/{task_id}/status` - Recording status

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Contributing

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## Support

For support, please open an issue in the GitHub repository.

## Acknowledgments

- [Agora.io](https://www.agora.io/) for the RTC SDK
- [FFmpeg](https://ffmpeg.org/) for video processing
- [Gin](https://github.com/gin-gonic/gin) for the HTTP server
- [Docker](https://www.docker.com/) for containerization
- [Redis](https://redis.io/) for caching
- [K8s](https://kubernetes.io/) for container orchestration
- [Helm](https://helm.sh/) for Kubernetes deployment
