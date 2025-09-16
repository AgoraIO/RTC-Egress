# Agora RTC Egress

A high-performance egress recording solution for Agora RTC streams, with support for saving video frames as images/recording mp4 and uploading to S3 or compatible storage.

## Features

- Record Agora RTC streams with configurable quality
- Save video frames as images at regular intervals
- HTTP API for controlling the recording process
- Built-in health checks
- S3 integration for storing recorded media
- Containerized deployment with Docker or Kubernetes

## Architecture

The system consists of five main components:

1. **C++ Core**: Handles the low-level RTC streaming and frame processing
2. **Go HTTP Server**: Provides REST API for controlling the recorder
3. **Go Egress Manager**: Manages the recording process
4. **Go Flexible Recorder**: Flexible recorder for recording Agora RTC streams
5. **Go Webhook Notifier**: Notifies external systems about recording events
6. **Go Uploader**: Uploads recorded media to S3 or compatible storage

## Prerequisites

- Docker and Docker Compose
- Agora Developer Account and App ID
- (Optional) AWS S3 credentials for cloud storage

## Quick Start

### Using Pre-built Docker Images (Recommended)

1. **With External Redis:**
   ```bash
   # Set your Redis connection
   export REDIS_ADDR="your-redis-host:6379"
   export APP_ID="your-agora-app-id"

   # Download and run
   curl -O https://raw.githubusercontent.com/AgoraIO/RTC-Egress/main/deployment/docker_compose/docker-compose-external-redis.yml
   docker compose -f docker-compose-external-redis.yml up -d
   ```

2. **With Built-in Redis (Development):**
   ```bash
   # Clone the repository for config files
   git clone https://github.com/AgoraIO/RTC-Egress.git
   cd RTC-Egress

   # Configure the application
   cp config/egress_config.yaml.example config/egress_config.yaml
   # Edit config/egress_config.yaml with your Agora credentials

   # Run with built-in Redis
   cd deployment/docker_compose
   docker compose -f docker-compose-redis-debug.yml up -d
   ```

3. **Access the web interface**
   Open `http://localhost:3000` in your browser

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

# Recording settings
recording:
  output_dir: "/recordings"  # Where to store recordings
  width: 1280               # Video width
  height: 720               # Video height
  fps: 30                   # Frames per second
  interval_in_ms: 20000  # Save frame every 20 seconds
```

Edit `config/uploader_config.yaml` to configure the uploader:

```yaml
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

2. **Build the C++ components**
   ```bash
   ./build.sh cpp
   ```

3. **Build the Go server**
   ```bash
   ./build.sh go
   ```

4. **Run the application**
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
  -e APP_ID="your-agora-app-id" \
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
{"status":"ok","version":"1.0.0"}
```

## Monitoring

The application exposes the following endpoints for monitoring:

- `http://localhost:8182/health` - Health check
- `http://localhost:8182/metrics` - Metrics
- `http://localhost:8080/api/v1/egress/status/{egress_id}` - Recording status

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
