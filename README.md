# Agora RTC Egress

A high-performance egress recording solution for Agora RTC streams, with support for saving video frames as images/recording mp4 and uploading to S3.

## Features

- Record Agora RTC streams with configurable quality
- Save video frames as images at regular intervals
- HTTP API for controlling the recording process
- Web interface for monitoring and control
- Built-in health checks
- S3 integration for storing recorded media
- Containerized deployment with Docker

## Architecture

The system consists of three main components:

1. **C++ Core**: Handles the low-level RTC streaming and frame processing
2. **Go HTTP Server**: Provides REST API for controlling the recorder
3. **Web Interface**: Simple UI for monitoring and controlling recordings

## Prerequisites

- Docker and Docker Compose
- Agora Developer Account and App ID
- (Optional) AWS S3 credentials for cloud storage

## Quick Start

1. **Clone the repository**
   ```bash
   git clone https://github.com/guohai/agora-rtc-egress.git
   cd agora-rtc-egress
   ```

2. **Configure the application**
   Edit the config file:
   ```bash
   cp config/egress_config.yaml.example config/egress_config.yaml
   vim config/egress_config.yaml
   ```

3. **Build and run with Docker Compose**
   ```bash
   docker-compose up --build -d
   ```

4. **Access the web interface**
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

# S3 Configuration (optional)
s3:
  bucket: "your-s3-bucket"
  region: "us-west-2"
  access_key: "YOUR_AWS_ACCESS_KEY"
  secret_key: "YOUR_AWS_SECRET_KEY"
  # endpoint: "https://s3.us-west-2.amazonaws.com"  # Optional custom endpoint

# Recording settings
recording:
  output_dir: "/recordings"  # Where to store recordings
  width: 1280               # Video width
  height: 720               # Video height
  fps: 30                   # Frames per second
  interval_in_ms: 20000  # Save frame every 20 seconds
```

## API Endpoints

### Start Recording
```http
POST /api/v1/egress/start
Content-Type: application/json

{
  "egress_id": "EG_egress001",
  "app_id": "YOUR_APP_ID",
  "access_token": "YOUR_ACCESS_TOKEN",
  "room_id": "test-room",
  "room_composite": {
    "room_name": "Podcast-Realtime",
    "layout": "speaker-dark",
    "audio_only": false,
    "video_only": false,
    "default_template_base_url": "http://localhost:3000/template/",
    "file_outputs": [
      {
        "file_type": "MP4",
        "filepath": "/recordings/output.mp4"
      }
    ]
  }
}
```

### Stop Recording
```http
POST /api/v1/egress/stop/{egress_id}
```

### Get Recording Status
```http
GET /api/v1/egress/status/{egress_id}
```

### Health Check
```http
GET /health
```

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
   git clone https://github.com/guohai/agora-rtc-egress.git
   cd agora-rtc-egress
   ```

2. **Build the C++ components**
   ```bash
   mkdir -p build
   cd build
   cmake ..
   make -j$(nproc)
   ```

3. **Build the Go server**
   ```bash
   cd cmd/egress-server
   go build -o ../../bin/egress-server
   ```

4. **Run the application**
   ```bash
   ./bin/egress-server --config config/egress_config.yaml
   ```

## Docker Deployment

### Build the Docker image
```bash
docker-compose build
```

### Start the services
```bash
docker-compose up -d
```

### View logs
```bash
docker-compose logs -f
```

## Monitoring

The application exposes the following endpoints for monitoring:

- `http://localhost:8182/health` - Health check
- `http://localhost:3000` - Web interface
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
