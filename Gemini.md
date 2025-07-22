# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

### C++ Components
- **Build all components**: `./build.sh full [AGORA_SDK_URL]` - Downloads Agora SDK, installs dependencies, and builds both C++ and Go components
- **Build C++ and Go**: `./build.sh build` - Builds C++ components and Go server (requires Agora SDK to be present)
- **Build C++ only**: `./build.sh cpp` - Builds C++ components (requires Agora SDK to be present)
- **Clean build**: `./build.sh clean` - Removes build artifacts
- **Install dependencies**: `./build.sh deps` - Installs system dependencies (cmake, ffmpeg, yaml-cpp, etc.)

### Go Server
- **Build Go server**: `./build.sh go` or `cd server && go build -o ../bin/egress`
- **Run with dependencies**: `cd server && go mod tidy && go build -o ../bin/egress`

### Docker
- **Build and run**: `docker-compose up --build -d`
- **View logs**: `docker-compose logs -f`
- **Stop services**: `docker-compose down`

## Architecture Overview

This is an Agora RTC egress recording system with a hybrid C++/Go architecture:

### Core Components

1. **C++ Core (`src/` directory)**:
   - `main.cpp` - Entry point for the egress worker (`eg_worker` executable)
   - `rtc_client.cpp` and `include/rtc_client.h` - Handles Agora RTC SDK integration and channel connections
   - `snapshot_sink.cpp` and `include/snapshot_sink.h` - Captures video frames and saves them as image snapshots
   - `recording_sink.cpp` and `include/recording_sink.h` - Captures video frames and saves them as image snapshots
   - `frame_processor.cpp` and `include/frame_processor.h` - Processes video/audio frames
   - `task_pipe.cpp` and `include/task_pipe.h` - IPC communication between Go manager and C++ workers via Unix domain sockets
   - `common/` - Shared utilities, logging, configuration parsing

2. **Go HTTP Server (`server/` directory)**:
   - `main.go` - HTTP server providing REST API and health checks
   - `egress/manager.go` - Manages C++ worker processes and task distribution
   - `uploader/` - Handles S3 file uploads and filesystem watching

3. **Web Interface (`web/template/`)**:
   - Simple HTML template for monitoring and control

### Data Flow

1. Go server receives HTTP API requests to start/stop recording sessions
2. Manager spawns C++ worker processes (`eg_worker`) via Unix domain sockets
3. C++ workers connect to Agora RTC channels and capture video frames
4. Frames are processed and saved as snapshots to configured output directories
5. File watcher detects new files and uploads them to S3 if configured
6. Workers report status back to manager via IPC

### Key Dependencies

- **Agora RTC SDK**: Core RTC functionality (downloaded automatically by build script)
- **FFmpeg**: Video/audio processing (`libavcodec`, `libavformat`, `libswscale`)
- **OpenCV**: Image processing and snapshot generation
- **yaml-cpp**: Configuration file parsing
- **Go libraries**: Gin (HTTP server), AWS SDK (S3 uploads), Viper (config)

## Configuration

- Main config file: `config/egress_config.yaml` (copy from `egress_config.yaml.example`)
- Key settings:
  - `app_id`: Agora application ID
  - `access_token`: Agora channel token
  - `snapshots.output_dir`: Where to save captured frames
  - `s3`: AWS S3 configuration for uploads
  - `health_port`: Health check endpoint (default 8182)
  - `api_port`: REST API port (default 8080)

## Running the Application

### Development Mode
```bash
# Build everything
./build.sh full [AGORA_SDK_URL]

# Configure
cp config/egress_config.yaml.example config/egress_config.yaml
# Edit config file with your Agora credentials

# Run the server
./bin/egress --config config/egress_config.yaml
```

### Production Mode
```bash
# Using Docker Compose
docker-compose up --build -d

# Check health
curl http://localhost:8182/health

# Access web interface
open http://localhost:3000
```

## API Endpoints

- `POST /egress/v1/task/assign` - Start recording/snapshot session
- `POST /egress/v1/task/release` - Stop recording/snapshot session
- `GET /egress/v1/task/status` - Get session status
- `GET /health` - Health check

## Development Notes

- The C++ worker supports two modes: standalone (auto-connect) and managed (socket-based IPC)
- Video frames are captured per-user and saved to separate directories
- The system uses Unix domain sockets for IPC between Go manager and C++ workers
- S3 uploads are handled asynchronously by a file watcher
- All executables are built to the `bin/` directory
- Agora SDK libraries are automatically copied to `bin/` during build

## Common Issues

- Missing Agora SDK: Run `./build.sh full` with a valid SDK download URL
- Missing system dependencies: Run `./build.sh deps`
- Configuration errors: Ensure `config/egress_config.yaml` has valid Agora credentials
- Port conflicts: Check that ports 8080, 8182, and 3000 are available

# Code style
- Always use simple and high performance code

# Workflow
- Be sure to typecheck when you’re done making a series of code changes
- Prefer running single tests, and not the whole test suite, for performance
