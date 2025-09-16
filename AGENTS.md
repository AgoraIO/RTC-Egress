# Development Guidelines

General development guidelines for this project:

## Philosophy

### Core Beliefs

- **Incremental progress over big bangs** - Small changes that compile and pass tests
- **Learning from existing code** - Study and plan before implementing
- **Pragmatic over dogmatic** - Adapt to project reality, real-world focus
- **Clear intent over clever code** - Be boring and obvious

### Simplicity Means

- Single responsibility per function/class
- Avoid premature abstractions
- No clever tricks - choose the boring solution
- Avoid unnecessary complexity, if you need to explain it, it's too complex
- Avoid unreadable code

## Process

### 1. Planning & Staging

Break complex work into 3-5 stages. And document key designs in `designs/IMPLEMENTATION_design.md`:

```markdown
## Stage N: [Name]
**Goal**: [Specific deliverable]
**Success Criteria**: [Testable outcomes]
**Tests**: [Specific test cases]
**Status**: [Not Started|In Progress|Complete]
```
- Update status as you progress
- Remove file when all stages are done

### 2. Implementation Flow

1. **Understand** - Study existing patterns in codebase
2. **Test** - Write test first (red)
3. **Implement** - Minimal code to pass (green)
4. **Refactor** - Clean up with tests passing
5. **Commit** - With clear message linking to plan

### 3. When Stuck (After 3 Attempts)

**CRITICAL**: Maximum 3 attempts per issue, then STOP.

1. **Document what failed**:
   - What you tried
   - Specific error messages
   - Why you think it failed

2. **Research alternatives**:
   - Find 2-3 similar implementations
   - Note different approaches used

3. **Question fundamentals**:
   - Is this the right abstraction level?
   - Can this be split into smaller problems?
   - Is there a simpler approach entirely?

4. **Try different angle**:
   - Different library/framework feature?
   - Different architectural pattern?
   - Remove abstraction instead of adding?

## Technical Standards

### Architecture Principles

- **Composition over inheritance** - Use dependency injection
- **Interfaces over singletons** - Enable testing and flexibility
- **Explicit over implicit** - Clear data flow and dependencies
- **Test-driven when possible** - Never disable tests, fix them

### Code Quality

- **Every commit must**:
  - Compile successfully
  - Pass all existing tests
  - Include tests for new functionality
  - Follow project formatting/linting
  - No performance regressions

- **Before committing**:
  - Run formatters/linters
  - Self-review changes
  - Ensure commit message explains "why"

### Error Handling

- Fail fast with descriptive messages
- Include context for debugging
- Handle errors at appropriate level
- Never silently swallow exceptions

## Decision Framework

When multiple valid approaches exist, choose based on:

1. **Testability** - Can I easily test this?
2. **Readability** - Will someone understand this in 6 months?
3. **Consistency** - Does this match project patterns?
4. **Simplicity** - Is this the simplest solution that works?
5. **Reversibility** - How hard to change later?

## Project Integration

### Learning the Codebase

- Find 3 similar features/components
- Identify common patterns and conventions
- Use same libraries/utilities when possible
- Follow existing test patterns

### Tooling

- Use project's existing build system
- Use project's test framework
- Use project's formatter/linter settings
- Don't introduce new tools without strong justification

## Quality Gates

### Definition of Done

- Tests written and passing
- Code follows project conventions
- No linter/formatter warnings
- Commit messages are clear
- Implementation matches plan
- No TODOs without issue numbers

### Test Guidelines

- Test behavior, not implementation
- One assertion per test when possible
- Clear test names describing scenario
- Use existing test utilities/helpers
- Tests should be deterministic

## Important Reminders

**NEVER**:
- Use `--no-verify` to bypass commit hooks
- Disable tests instead of fixing them
- Commit code that doesn't compile
- Make assumptions - verify with existing code

**ALWAYS**:
- Commit working code incrementally
- Update plan documentation as you go
- Learn from existing implementations
- Stop after 3 failed attempts and reassess

# Repository Guidelines

This is an Agora RTC egress recording system with a hybrid C++/Go architecture:

## Build Commands

### Local Build Components (for development, without docker)
- **Build all components**: `./build.sh full [AGORA_SDK_URL]` - Downloads Agora SDK, installs dependencies, and builds both C++ and Go components
- **Build C++ only**: `./build.sh cpp` - Builds C++ components (requires Agora SDK to be present)
- **Build Go only**: `./build.sh go` - Builds Go server (requires Agora SDK to be present)
- **Build local services**: `./build.sh local` - Builds all local services(C++ and Go) (requires Agora SDK to be present)
- **Run local services**: `./build.sh run all` - Runs all local services(C++ and Go)
- **Clean build**: `./build.sh clean` - Removes build artifacts
- **Install dependencies**: `./build.sh deps` - Installs system dependencies (cmake, ffmpeg, yaml-cpp, etc.)

### Docker
- **Build images**: `./build.sh images` - Build all standard service images
- **Build image**: `./build.sh image [api-server|egress|flexible-recorder|webhook-notifier]` - Build specific service image
- **Build and run**: `docker compose up --build -d`
- **View logs**: `docker compose logs -f`
- **Stop services**: `docker compose down`

## Architecture Overview

This is an Agora RTC egress system with a hybrid C++/Go architecture:

### Core Components

1. **C++ Core (`src/` directory)**:
   - `main.cpp` - Entry point for the egress worker (`eg_worker` executable)
   - `rtc_client.cpp` and `include/rtc_client.h` - Handles Agora RTC SDK integration and channel connections
   - `snapshot_sink.cpp` and `include/snapshot_sink.h` - Captures video frames and saves them as image snapshots
   - `recording_sink.cpp` and `include/recording_sink.h` - Captures video frames and saves them as image snapshots
   - `frame_processor.cpp` and `include/frame_processor.h` - Processes video/audio frames
   - `task_pipe.cpp` and `include/task_pipe.h` - IPC communication between Go manager and C++ workers via Unix domain sockets
   - `common/` - Shared utilities, logging, configuration parsing

2. **Go HTTP Server (`cmd/` directory)**:

### Data Flow

1. `cmd/api_server/` - Task publisher service
- receives HTTP API requests and puts tasks into redis queue
for native
2. `cmd/egress/` - Native recording service (ModeNative)
- Manager fetches tasks from redis queue and spawns C++ worker processes (`eg_worker`) via Unix domain sockets
- C++ workers connect to Agora RTC channels and capture video frames
- Frames are processed and saved as snapshots/recordings to configured output directories
- Workers report status back to manager via IPC
for web
3. `cmd/flexible_recorder/` - Web recording service (ModeWeb, shares the code with `cmd/egress`, binary: flexible-recorder)
- Manager fetches tasks from redis queue and send to Web Recorder
- Web Recorder Engine(external stanalone service in a docker container)
4. `cmd/uploader/` - File uploader service
- detects new files in configured directory and uploads them to S3
5. `cmd/webhook-notifier/` - Webhook notifier service
- send webhook notifications to configured endpoints

### Key Dependencies

- **Agora RTC SDK**: Core RTC functionality (downloaded automatically by build script)
- **FFmpeg**: Video/audio processing (`libavcodec`, `libavformat`, `libswscale`)
- **yaml-cpp**: Configuration file parsing
- **Go libraries**: Gin (HTTP server), AWS SDK (S3 uploads), Viper (config)

## Low level designs

- under `designs` directory
   - `unified_pod_architecture_design.md`: unified pod architecture design
   - `restful_api_design.md`: restful api design
   - `vars_config_design.md`: variables and configuration design
   - `composition_mode_design.md`: composition mode design
   - `rtc_pts_design.md`: a/v sync, pts design
   - `task_dispatching_design.md`: task dispatching design
   - `validation_security_design.md`: validation and security design
   - `worker_recovery_design.md`: worker/egress recovery design

## Configuration

- Main config file: `config/egress_config.yaml` (copy from `egress_config.yaml.example`)
- Key settings:
  - `app_id`: Agora application ID
  - `access_token`: Agora channel token
  - `snapshots.output_dir`: Where to save captured frames
  - `health_port`: Health check endpoint (default 8182)

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

Refer to `designs/restful_api_design.md`

## Development Notes

- The C++ worker supports two modes: standalone (auto-connect) and managed (socket-based IPC). For the standalone mode, it will connect to Agora RTC channels and capture video frames, do snapshot and record at same time, this is for testing and need to handle 'Ctrl + C' to stop the worker, for the managed mode, dont need to handle 'Ctrl + C' to stop the worker, it will be stopped by the manager
- The system uses Unix domain sockets for IPC between Go manager and C++ workers
- Video frames are captured per-user and may be composited before saving to disk
- S3 uploads are handled asynchronously by a file watcher
- All executables are built to the `bin/` directory
- Agora SDK libraries are automatically copied to `bin/` during build

## Common Issues

- Missing Agora SDK: Run `./build.sh full` with a valid SDK download URL
- Missing system dependencies: Run `./build.sh deps`
- Configuration errors: Ensure `config/egress_config.yaml` has valid Agora credentials if you are running standalone mode
- Port conflicts: Check that ports 8080, 8182, 8192, and 3000 are available
