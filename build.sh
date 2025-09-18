#!/bin/bash

set -e

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to check if command needs sudo
check_sudo_warning() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${YELLOW}Warning: Not running as root. Package installation requires sudo privileges.${NC}"
    fi
}

# Function to download and setup Agora SDK
download_agora_sdk() {
    local url=$1
    if [ -z "$url" ]; then
        echo -e "${RED}Error: No Agora SDK URL provided.${NC}"
        echo -e "Usage: $0 [full|build|cpp|go|deps|clean] [AGORA_SDK_URL]"
        echo -e "Example: $0 full https://download.agora.io/rtsasdk/release/Agora-RTC-x86_64-linux-gnu-v4.4.32-20250425_144419-675648.tgz"
        exit 1
    fi

    echo -e "${GREEN}Downloading Agora SDK from: $url${NC}"

    # Create temporary directory for extraction
    local temp_dir=$(mktemp -d)
    
    echo -e "${GREEN}Downloading and extracting Agora SDK...${NC}"
    
    # Download and extract to temp directory
    if ! (cd "$temp_dir" && curl -L "$url" | tar xz); then
        echo -e "${RED}Failed to download or extract Agora SDK from $url${NC}"
        rm -rf "$temp_dir"
        exit 1
    fi
    
    # Find the extracted agora_sdk directory
    local sdk_dir=$(find "$temp_dir" -type d -name 'agora_sdk*' -o -name 'Agora_*' | head -n 1)
    
    if [ -z "$sdk_dir" ] || [ ! -d "$sdk_dir" ]; then
        echo -e "${RED}Could not find Agora SDK directory in the downloaded archive${NC}"
        echo -e "${YELLOW}Contents of the archive:${NC}"
        (cd "$temp_dir" && ls -la)
        rm -rf "$temp_dir"
        exit 1
    fi
    
    # Create target directory
    rm -rf agora_sdk
    mkdir -p agora_sdk
    
    # Move contents from the extracted sdk directory to our target directory
    echo -e "${GREEN}Moving SDK files to agora_sdk/...${NC}"
    mv "$sdk_dir"/* agora_sdk/ 2>/dev/null || true
    
    # Clean up
    rm -rf "$temp_dir"
    
    # Verify the download
    if [ ! -f "agora_sdk/include/IAgoraService.h" ]; then
        echo -e "${RED}Downloaded Agora SDK has incorrect structure.${NC}"
        echo -e "${YELLOW}Expected file not found: agora_sdk/include/IAgoraService.h${NC}"
        echo -e "${YELLOW}Contents of agora_sdk/include:${NC}"
        ls -la agora_sdk/include/
        exit 1
    fi
    
    echo -e "${GREEN}Successfully downloaded and extracted Agora SDK${NC}"
}

# Function to check Agora SDK
check_agora_sdk() {
    if [ ! -d "agora_sdk/include/api" ] || [ ! -f "agora_sdk/include/IAgoraService.h" ]; then
        echo -e "${YELLOW}Agora SDK not found. Downloading...${NC}"
        download_agora_sdk "$AGORA_SDK_URL"
    fi
}

# Function to build the C++ components
build_cpp() {
    echo -e "${GREEN}Building C++ components...${NC}"

    # Check if Agora SDK is available
    check_agora_sdk

    local current_dir=$(pwd)
    mkdir -p build
    cd build

    # Use the full path to the source directory
    cmake "$current_dir" -DCMAKE_BUILD_TYPE=Release

    # Check if cmake succeeded
    if [ $? -ne 0 ]; then
        echo -e "${RED}CMake configuration failed. See errors above.${NC}"
        exit 1
    fi

    make -j$(nproc)

    if [ $? -ne 0 ]; then
        echo -e "${RED}Build failed. See errors above.${NC}"
        exit 1
    fi

    cd "$current_dir"

    # Copy Agora SDK libraries to bin directory
    if [ -d "agora_sdk/" ]; then
        echo -e "${GREEN}Copying Agora SDK libraries...${NC}"
        mkdir -p bin
        cp -P agora_sdk/*.so bin/
    fi
}

# Function to build the Go server
build_go() {
    echo -e "${GREEN}Building Go server...${NC}"

    if [ ! -d "cmd/egress" ]; then
        echo -e "${RED}Error: cmd/egress directory not found. Current directory: $(pwd)${NC}"
        echo -e "${RED}Contents: $(ls -la)${NC}"
        exit 1
    fi

    if [ ! -f "go.mod" ]; then
        echo -e "${RED}Error: go.mod not found in project root${NC}"
        exit 1
    fi

    echo -e "${GREEN}Tidying Go modules...${NC}"
    go mod tidy

    echo -e "${GREEN}Building egress service...${NC}"
    go build -o bin/egress ./cmd/egress

    if [ ! -f "bin/egress" ]; then
        echo -e "${RED}Build failed - egress binary not found${NC}"
        exit 1
    fi
    echo -e "${GREEN}egress build completed successfully${NC}"

    # Build api-server service if directory exists
    if [ -d "cmd/api_server" ]; then
        echo -e "${GREEN}Building api-server service...${NC}"
        go build -o bin/api-server ./cmd/api_server

        if [ ! -f "bin/api-server" ]; then
            echo -e "${RED}Build failed - api-server binary not found${NC}"
            exit 1
        fi
        echo -e "${GREEN}api-server build completed successfully${NC}"
    fi

    # Build flexible-recorder service if directory exists
    if [ -d "cmd/flexible_recorder" ]; then
        echo -e "${GREEN}Building flexible-recorder service...${NC}"
        go build -o bin/flexible-recorder ./cmd/flexible_recorder
        if [ ! -f "bin/flexible-recorder" ]; then
            echo -e "${RED}Build failed - flexible-recorder binary not found${NC}"
            exit 1
        fi
        echo -e "${GREEN}flexible-recorder build completed successfully${NC}"
    fi

    # Build uploader service if directory exists
    if [ -d "cmd/uploader" ]; then
        echo -e "${GREEN}Building uploader service...${NC}"
        go build -o bin/uploader ./cmd/uploader

        if [ ! -f "bin/uploader" ]; then
            echo -e "${RED}Build failed - uploader binary not found${NC}"
            exit 1
        fi
        echo -e "${GREEN}uploader build completed successfully${NC}"
    fi
    
    # Build webhook_notifier service if directory exists
    if [ -d "cmd/webhook_notifier" ]; then
        echo -e "${GREEN}Building webhook-notifier service...${NC}"
        go build -o bin/webhook-notifier ./cmd/webhook_notifier

        if [ ! -f "bin/webhook-notifier" ]; then
            echo -e "${RED}Build failed - webhook-notifier binary not found${NC}"
            exit 1
        fi
        echo -e "${GREEN}webhook-notifier build completed successfully${NC}"
    fi
    
    echo -e "${GREEN}Go build completed successfully${NC}"
}

# Function to create necessary directories
create_dirs() {
    echo -e "${GREEN}Creating necessary directories...${NC}"
    mkdir -p bin
    mkdir -p logs/rtc_egress
    mkdir -p recordings
    mkdir -p snapshots
    mkdir -p web_recorder

    # Set permissions
    chmod 755 logs/rtc_egress
    chmod 755 recordings
    chmod 755 snapshots
    chmod 755 web_recorder

    echo -e "${YELLOW}Note: Using local 'logs', 'recordings', 'snapshots', 'web_recorder' directories in the project folder.${NC}"
    echo -e "${YELLOW}For production, you may want to use system directories like /var/log/rtc_egress, /recordings, /snapshots, /web_recorder${NC}"
}

create_dirs_docker() {
    echo -e "${GREEN}Creating necessary directories for Docker...${NC}"
    mkdir -p bin
    # For Docker, don't create logs/recordings/snapshots here as they'll be volumes
    # Just ensure bin directory exists for build output
}

# Function to install dependencies
install_dependencies() {
    check_sudo_warning
    echo -e "${GREEN}Installing dependencies...${NC}"
    
    # For Ubuntu/Debian
    if [ -f /etc/debian_version ]; then
        sudo apt-get update
        sudo apt-get install -y \
            build-essential \
            cmake \
            libssl-dev \
            libavcodec-dev \
            libavformat-dev \
            libavutil-dev \
            libswscale-dev \
            libswresample-dev \
            libyaml-cpp-dev \
            pkg-config \
            golang
    # For CentOS/RHEL
    elif [ -f /etc/redhat-release ]; then
        sudo yum groupinstall -y "Development Tools"
        sudo yum install -y \
            cmake \
            openssl-devel \
            ffmpeg-devel \
            yaml-cpp-devel \
            golang
    else
        echo -e "${YELLOW}Unsupported Linux distribution. Please install dependencies manually.${NC}"
        exit 1
    fi
}

# Function to build standard service images
build_service_image() {
    local service=$1

    echo -e "${GREEN}Building $service service image...${NC}"

    case $service in
        api-server)
            echo -e "${YELLOW}Building API Server image (task publisher)...${NC}"
            if [ ! -f "bin/api-server" ]; then
                echo -e "${RED}Error: Missing binaries. Run './build.sh local' first${NC}"
                exit 1
            fi
            docker build -f cmd/api_server/Dockerfile -t rtc-egress/api-server:latest .
            ;;

        egress)
            echo -e "${YELLOW}Building Egress image (handles native recording)...${NC}"
            # Ensure binaries exist for production image
            if [ ! -f "bin/egress" ] || [ ! -f "bin/eg_worker" ]; then
                echo -e "${RED}Error: Missing binaries. Run './build.sh local' first${NC}"
                exit 1
            fi
            docker build -f cmd/egress/Dockerfile -t rtc-egress/egress:latest .
            ;;

        flexible-recorder)
            echo -e "${YELLOW}Building Flexible Recorder image (handles web recording)...${NC}"
            if [ ! -f "bin/flexible-recorder" ]; then
                echo -e "${RED}Error: Missing binaries. Run './build.sh local' first${NC}"
                exit 1
            fi
            docker build -f cmd/flexible_recorder/Dockerfile -t rtc-egress/flexible-recorder:latest .
            ;;

        uploader)
            echo -e "${YELLOW}Building Uploader image (standalone file upload)...${NC}"
            if [ ! -f "bin/uploader" ]; then
                echo -e "${RED}Error: Missing binaries. Run './build.sh local' first${NC}"
                exit 1
            fi
            docker build -f cmd/uploader/Dockerfile -t rtc-egress/uploader:latest .
            ;;

        webhook-notifier)
            echo -e "${YELLOW}Building Webhook Notifier image...${NC}"
            if [ ! -f "bin/webhook-notifier" ]; then
                echo -e "${RED}Error: Missing binaries. Run './build.sh local' first${NC}"
                exit 1
            fi
            docker build -f cmd/webhook_notifier/Dockerfile -t rtc-egress/webhook-notifier:latest .
            ;;

        *)
            echo -e "${RED}Error: Unknown service '$service'${NC}"
            echo -e "Available services: api-server, egress, flexible-recorder, uploader, webhook-notifier"
            exit 1
            ;;
    esac

    echo -e "${GREEN}Successfully built rtc-egress/$service:latest${NC}"
}

# Function to build debug service images
build_debug_image() {
    local service=$1

    echo -e "${GREEN}Building $service debug image...${NC}"

    case $service in
        api-server)
            echo -e "${YELLOW}Building API Server debug image (with source code and symbols)...${NC}"
            docker build -f cmd/api_server/Dockerfile.debug -t rtc-egress/api-server:debug .
            ;;

        egress)
            echo -e "${YELLOW}Building Egress debug image (with source code and symbols)...${NC}"
            docker build -f cmd/egress/Dockerfile.debug -t rtc-egress/egress:debug .
            ;;

        flexible-recorder)
            echo -e "${YELLOW}Building Flexible Recorder debug image (with source code and symbols)...${NC}"
            docker build -f cmd/flexible_recorder/Dockerfile.debug -t rtc-egress/flexible-recorder:debug .
            ;;

        uploader)
            echo -e "${YELLOW}Building Uploader debug image (with source code and symbols)...${NC}"
            docker build -f cmd/uploader/Dockerfile.debug -t rtc-egress/uploader:debug .
            ;;

        webhook-notifier)
            echo -e "${YELLOW}Building Webhook Notifier debug image (with source code and symbols)...${NC}"
            docker build -f cmd/webhook_notifier/Dockerfile.debug -t rtc-egress/webhook-notifier:debug .
            ;;

        *)
            echo -e "${RED}Error: Unknown service '$service'${NC}"
            echo -e "Available services: api-server, egress, flexible-recorder, uploader, webhook-notifier"
            exit 1
            ;;
    esac

    echo -e "${GREEN}Successfully built rtc-egress/$service:debug${NC}"
}


# Function to run services locally
run_local_service() {
    local service=$1

    # Load port configurations
    load_port_config

    case $service in
        api-server)
            echo -e "${GREEN}Starting API Server (port ${API_PORT}, health ${API_HEALTH_PORT})...${NC}"
            ./bin/api-server --config config/api_server_config.yaml
            ;;

        egress)
            echo -e "${GREEN}Starting Egress (handles native recording, health ${EGRESS_HEALTH_PORT})...${NC}"
            LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/egress --config config/egress_config.yaml
            ;;

        flexible-recorder)
            echo -e "${GREEN}Starting Flexible Recorder (handles web recording, health ${FLEXIBLE_HEALTH_PORT})...${NC}"
            echo -e "${YELLOW}Note: This requires Agora Web Recorder Engine running on localhost:8001${NC}"
            LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/flexible-recorder --config config/flexible_recorder_config.yaml
            ;;

        uploader)
            echo -e "${GREEN}Starting Uploader (handles file uploads, health ${UPLOADER_HEALTH_PORT})...${NC}"
            ./bin/uploader --config config/uploader_config.yaml
            ;;

        webhook-notifier)
            echo -e "${GREEN}Starting Webhook Notifier (health ${WEBHOOK_HEALTH_PORT})...${NC}"
            ./bin/webhook-notifier --config config/webhook_notifier_config.yaml
            ;;

        all)
            echo -e "${GREEN}Starting all services in background...${NC}"
            run_all_services
            ;;

        *)
            echo -e "${RED}Error: Unknown service '$service'${NC}"
            echo -e "Available services: api-server, egress, flexible-recorder, uploader, webhook-notifier, all"
            exit 1
            ;;
    esac
}

# Function to read port from config file
read_port_from_config() {
    local config_file=$1
    local port_key=$2

    if [ -f "$config_file" ]; then
        # Use grep and awk to extract port value from YAML
        grep "^[[:space:]]*${port_key}:" "$config_file" | awk '{print $2}' | tr -d '"'
    fi
}

# Function to load all port configurations as global variables
load_port_config() {
    API_PORT=$(read_port_from_config "config/api_server_config.yaml" "port")
    API_HEALTH_PORT=$(read_port_from_config "config/api_server_config.yaml" "health_port")
    EGRESS_HEALTH_PORT=$(read_port_from_config "config/egress_config.yaml" "health_port")
    FLEXIBLE_HEALTH_PORT=$(read_port_from_config "config/flexible_recorder_config.yaml" "health_port")
    UPLOADER_HEALTH_PORT=$(read_port_from_config "config/uploader_config.yaml" "health_port")
    WEBHOOK_HEALTH_PORT=$(read_port_from_config "config/webhook_notifier_config.yaml" "health_port")
}

# Function to check for running services and ports
preflight_check() {
    echo -e "${GREEN}Running pre-flight checks...${NC}"

    # Load port configurations
    load_port_config

    local conflicts_found=false
    local running_processes=""
    local occupied_ports=""

    # Check for running egress processes
    local processes=$(ps aux \
        | grep -E "(bin/api-server|bin/egress|bin/flexible-recorder|bin/uploader|bin/webhook-notifier|bin/eg_worker)" \
        | grep -v grep \
        | grep -v "agora-web-recorder" \
        | awk '{print $2, $11, $12, $13, $14, $15}')
    if [ ! -z "$processes" ]; then
        conflicts_found=true
        running_processes="$processes"

        # Count eg_worker processes
        local eg_worker_count=$(echo "$processes" | grep "bin/eg_worker" | wc -l)
        if [ $eg_worker_count -gt 0 ]; then
            echo -e "${YELLOW}Found $eg_worker_count eg_worker process(es) running${NC}"
        fi
    fi

    # Build list of ports to check from global variables
    local ports_to_check="$API_PORT $API_HEALTH_PORT $EGRESS_HEALTH_PORT $FLEXIBLE_HEALTH_PORT $UPLOADER_HEALTH_PORT $WEBHOOK_HEALTH_PORT"

    # Check for port conflicts
    for port in $ports_to_check; do
        if [ ! -z "$port" ] && ss -tlnp | grep -q ":$port "; then
            conflicts_found=true
            occupied_ports="$occupied_ports $port"
        fi
    done

    if [ "$conflicts_found" = true ]; then
        echo -e "${RED}⚠️  Services or ports are already in use!${NC}"
        echo ""

        if [ ! -z "$running_processes" ]; then
            echo -e "${YELLOW}Running egress processes:${NC}"
            echo "$running_processes" | while read line; do
                echo -e "  ${RED}•${NC} $line"
            done
            echo ""
        fi

        if [ ! -z "$occupied_ports" ]; then
            echo -e "${YELLOW}Occupied ports:${NC}$occupied_ports"
            echo ""
        fi

        echo -e "${GREEN}Shutdown options:${NC}"
        echo -e "  ${YELLOW}Graceful:${NC} ./build.sh stop"
        echo -e "  ${YELLOW}Force kill:${NC} ./build.sh force-kill"
        echo ""
        echo -e "${RED}Please stop conflicting services before running './build.sh run all'${NC}"
        exit 1
    fi

    echo -e "${GREEN}✓ Pre-flight checks passed - ready to start services${NC}"
}

# Function to run all services in background processes
run_all_services() {
    echo -e "${GREEN}Starting complete RTC Egress system locally...${NC}"

    # Run pre-flight checks first
    preflight_check

    # Create log directory
    mkdir -p logs

    # Set library path for Agora SDK
    export LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH"

    # Start services in background with logging
    echo -e "${YELLOW}Starting API Server (port ${API_PORT})...${NC}"
    LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/api-server --config config/api_server_config.yaml > logs/api-server.log 2>&1 &
    API_PID=$!

    echo -e "${YELLOW}Starting Egress (handles native recording)...${NC}"
    LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/egress --config config/egress_config.yaml > logs/egress.log 2>&1 &
    EGRESS_PID=$!

    echo -e "${YELLOW}Starting Flexible Recorder (handles web recording)...${NC}"
    LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/flexible-recorder --config config/flexible_recorder_config.yaml > logs/flexible-recorder.log 2>&1 &
    FLEXIBLE_PID=$!

    echo -e "${YELLOW}Starting Uploader (handles file uploads)...${NC}"
    LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/uploader --config config/uploader_config.yaml > logs/uploader.log 2>&1 &
    UPLOADER_PID=$!

    echo -e "${YELLOW}Starting Webhook Notifier...${NC}"
    LD_LIBRARY_PATH="$(pwd)/bin:$LD_LIBRARY_PATH" ./bin/webhook-notifier --config config/webhook_notifier_config.yaml > logs/webhook-notifier.log 2>&1 &
    WEBHOOK_PID=$!

    # Store PIDs for cleanup
    echo "$API_PID $EGRESS_PID $FLEXIBLE_PID $UPLOADER_PID $WEBHOOK_PID" > logs/service.pids

    echo -e "${GREEN}All services started!${NC}"
    echo -e "${YELLOW}Service PIDs saved to logs/service.pids${NC}"
    echo -e "${YELLOW}Logs available in logs/ directory${NC}"
    echo ""
    echo -e "${GREEN}Service Endpoints:${NC}"
    echo -e "  API Server:        restful: http://localhost:${API_PORT} (restful api)"
    echo -e "  API Server:        health: http://localhost:${API_HEALTH_PORT} (restful api)"
    echo -e "  Egress:            health: http://localhost:${EGRESS_HEALTH_PORT} (native recording)"
    echo -e "  Flexible Recorder: health: http://localhost:${FLEXIBLE_HEALTH_PORT} (web recording)"
    echo -e "  Uploader:          health: http://localhost:${UPLOADER_HEALTH_PORT} (file uploads)"
    echo -e "  Webhook Notifier:  health: http://localhost:${WEBHOOK_HEALTH_PORT} (event notifier)"
    echo ""
    echo -e "${YELLOW}To stop all services: ./build.sh stop${NC}"
    echo -e "${YELLOW}To view logs: tail -f logs/*.log${NC}"
}

# Function to stop all local services
stop_local_services() {
    echo -e "${GREEN}Stopping main services (egress will handle eg_workers gracefully)...${NC}"

    local stopped=false

    # Try to stop services using PID file first (main services only)
    if [ -f logs/service.pids ]; then
        echo -e "${YELLOW}Stopping services using saved PIDs...${NC}"
        PIDS=$(cat logs/service.pids)
        for pid in $PIDS; do
            if kill -0 $pid 2>/dev/null; then
                echo -e "${YELLOW}Stopping process $pid...${NC}"
                kill $pid
                stopped=true
            fi
        done

        # Wait for graceful shutdown (egress will stop eg_workers)
        sleep 3

        # Force kill main services if still running
        for pid in $PIDS; do
            if kill -0 $pid 2>/dev/null; then
                echo -e "${YELLOW}Force stopping process $pid...${NC}"
                kill -9 $pid
            fi
        done

        rm -f logs/service.pids
    fi

    # Also check for main service processes (exclude eg_worker - let egress handle them)
    local main_services=$(ps aux \
        | grep -E "(bin/api-server|bin/egress|bin/flexible-recorder|bin/uploader|bin/webhook-notifier)" \
        | grep -v grep \
        | grep -v "agora-web-recorder" \
        | awk '{print $2}')

    if [ ! -z "$main_services" ]; then
        echo -e "${YELLOW}Found additional main service processes...${NC}"
        echo "$main_services" | while read pid; do
            if kill -0 $pid 2>/dev/null; then
                echo -e "${YELLOW}Stopping process $pid...${NC}"
                kill $pid
                stopped=true
            fi
        done

        # Wait for graceful shutdown
        sleep 3
        echo "$main_services" | while read pid; do
            if kill -0 $pid 2>/dev/null; then
                echo -e "${YELLOW}Force stopping process $pid...${NC}"
                kill -9 $pid
            fi
        done
    fi

    if [ "$stopped" = true ]; then
        echo -e "${GREEN}All services stopped!${NC}"
    else
        echo -e "${YELLOW}No running services found${NC}"
    fi
}

# Function to bump version
bump_version() {
    local new_version=$1
    local current_version

    # Read current version from VERSION file
    if [ -f "VERSION" ]; then
        current_version=$(cat VERSION | tr -d '\n\r' | sed 's/^v//')
    else
        echo -e "${RED}Error: VERSION file not found${NC}"
        exit 1
    fi

    echo -e "${GREEN}Current version: v$current_version${NC}"

    if [ -z "$new_version" ]; then
        # Auto-increment patch version (third number)
        IFS='.' read -r major minor patch <<< "$current_version"

        # Validate version format
        if ! [[ "$major" =~ ^[0-9]+$ ]] || ! [[ "$minor" =~ ^[0-9]+$ ]] || ! [[ "$patch" =~ ^[0-9]+$ ]]; then
            echo -e "${RED}Error: Invalid version format in VERSION file: $current_version${NC}"
            echo -e "${YELLOW}Expected format: x.y.z (e.g., 1.0.0)${NC}"
            exit 1
        fi

        # Increment patch version
        patch=$((patch + 1))
        new_version="$major.$minor.$patch"
        echo -e "${GREEN}Auto-incrementing to: v$new_version${NC}"
    else
        # Remove 'v' prefix if provided
        new_version=$(echo "$new_version" | sed 's/^v//')

        # Validate provided version format
        if ! [[ "$new_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
            echo -e "${RED}Error: Invalid version format: $new_version${NC}"
            echo -e "${YELLOW}Expected format: x.y.z (e.g., 1.2.0)${NC}"
            exit 1
        fi

        echo -e "${GREEN}Setting version to: v$new_version${NC}"
    fi

    # Update VERSION file
    echo "v$new_version" > VERSION
    echo -e "${GREEN}Updated VERSION file${NC}"

    # Commit version changes
    git add VERSION
    git commit -m "Chore: Bump version to v$new_version"
    echo -e "${GREEN}Committed version bump${NC}"

    # Create git tag
    local tag_name="v$new_version"

    # Check if tag already exists
    if git rev-parse "$tag_name" >/dev/null 2>&1; then
        echo -e "${YELLOW}Warning: Tag $tag_name already exists${NC}"
        read -p "Do you want to delete and recreate the tag? (y/N): " confirm
        if [[ "$confirm" == "y" || "$confirm" == "Y" ]]; then
            git tag -d "$tag_name"
            echo -e "${YELLOW}Deleted existing tag $tag_name${NC}"
        else
            echo -e "${YELLOW}Skipping tag creation${NC}"
            return
        fi
    fi

    # Create new tag
    git tag -a "$tag_name" -m "Release version $tag_name"
    echo -e "${GREEN}Created git tag: $tag_name${NC}"
    echo -e "${YELLOW}To push the tag to remote, run: git push origin $tag_name${NC}"
}

# Function to force kill all egress processes
force_kill_services() {
    echo -e "${GREEN}Force killing all egress processes...${NC}"

    # Search for processes matching either './bin/eg_worker' or './bin/egress' or './bin/flexible-recorder' or './bin/uploader' or './bin/webhook-notifier' or './bin/api-server' command
    # Includes arguments like --config
    matches=$(ps -eo pid,command \
        | grep -E '\./bin/(eg_worker|egress|flexible-recorder|uploader|webhook-notifier|api-server)' \
        | grep -v grep \
        | grep -v "agora-web-recorder")

    if [ -z "$matches" ]; then
      echo -e "${YELLOW}❌ No eg_worker/egress/flexible-recorder/uploader/webhook-notifier/api-server process found.${NC}"
      exit 0
    fi

    # Display matched processes
    echo -e "${YELLOW}✅ Found the following matching process(es):${NC}"
    echo "$matches"
    echo

    # Ask the user to confirm before killing
    read -p "⚠️ Do you want to kill ALL of these processes? (y/N): " confirm

    # If not confirmed, exit safely
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
      echo -e "${YELLOW}❎ Aborted by user.${NC}"
      exit 0
    fi

    # Extract and kill each PID
    echo "$matches" | awk '{print $1}' | while read pid; do
      echo -e "${YELLOW}🔪 Killing PID $pid ...${NC}"
      kill -9 "$pid"
    done

    echo -e "${GREEN}✅ Done.${NC}"
}

# Default Agora SDK URL (version 4.4.32)
DEFAULT_AGORA_SDK_URL="https://download.agora.io/rtsasdk/release/Agora-RTC-x86_64-linux-gnu-v4.4.32-20250425_144419-675648.tgz"

# Main script
# Check if second argument is a URL
if [[ $2 == http* ]]; then
    AGORA_SDK_URL=$2
    export AGORA_SDK_URL
fi

# Set default URL if not provided (skip for commands that don't need SDK)
if [ -z "$AGORA_SDK_URL" ] && [ "$1" != "clean" ] && [ "$1" != "go" ] && [ "$1" != "launch_web_recorder" ] && [ "$1" != "format" ] && [ "$1" != "stop" ] && [ "$1" != "force-kill" ] && [ "$1" != "run" ] && [ "$1" != "images" ] && [ "$1" != "images-debug" ] && [ "$1" != "image" ] && [ "$1" != "image-debug" ] && [ "$1" != "bump_version" ]; then
    AGORA_SDK_URL="$DEFAULT_AGORA_SDK_URL"
    echo -e "${YELLOW}Using default Agora SDK URL: $AGORA_SDK_URL${NC}"
    echo -e "${YELLOW}To use a different URL, you can:${NC}"
    echo -e "1. Set environment variable: export AGORA_SDK_URL=YOUR_Agora_SDK_URL"
    echo -e "2. Or pass as second argument: $0 all YOUR_Agora_SDK_URL"
fi

case "$1" in
    full)
    echo -e "${GREEN}Building all for local execution (no Docker)...${NC}"
        ./prettifier.sh
        install_dependencies
        create_dirs
        build_cpp
        build_go
        ;;

    local)
        echo -e "${GREEN}Building for local execution (no Docker)...${NC}"
        ./prettifier.sh
        create_dirs
        build_cpp
        build_go
        ;;

    ci)
        echo -e "${GREEN}Building for CI/Docker (no formatting, no deps install)...${NC}"
        create_dirs_docker
        build_cpp
        build_go
        ;;

    run)
        if [ -z "$2" ]; then
            echo -e "${RED}Error: Service name or 'all' required${NC}"
            echo -e "Usage: $0 run [api-server|egress|flexible-recorder|uploader|webhook-notifier|all]"
            exit 1
        fi
        run_local_service "$2"
        ;;

    stop)
        stop_local_services
        ;;

    force-kill)
        force_kill_services
        ;;

    format)
        echo -e "${GREEN}Running code formatting...${NC}"
        ./prettifier.sh
        ;;

    cpp)
        ./prettifier.sh
        create_dirs
        build_cpp
        ;;

    go)
        ./prettifier.sh
        create_dirs
        build_go
        ;;

    deps)
        install_dependencies
        create_dirs
        ;;

    clean)
        echo -e "${GREEN}Cleaning build directories...${NC}"
        rm -rf build/
        rm -f bin/api-server
        rm -f bin/egress
        rm -f bin/eg_worker
        rm -f bin/flexible-recorder
        rm -f bin/uploader
        rm -f bin/webhook-notifier
        ;;

    launch_web_recorder)
        # Use provided version or default
        AGORA_WEB_RECORDER_VERSION=${2:-"test_20250818024350_v2.13.8"}
        AGORA_WEB_RECORDER_IMAGE="hub-master.agoralab.co/uap/cloud_recording/cloud_recording-web_recorder_engine:${AGORA_WEB_RECORDER_VERSION}"

        echo -e "${GREEN}Launching Web Recorder Engine for testing...${NC}"
        echo -e "Using version: ${AGORA_WEB_RECORDER_VERSION}"

        # Create web_recorder directory if it doesn't exist
        mkdir -p ./web_recorder

        # Check if container is already running
        if docker ps | grep -q "cloud_recording-web_recorder_engine"; then
            echo -e "${YELLOW}Web Recorder Engine is already running${NC}"
            echo -e "To stop it, run: docker stop \$(docker ps -q --filter ancestor=${AGORA_WEB_RECORDER_IMAGE})"
            exit 0
        fi

        # Launch the container
        echo -e "${GREEN}Starting Web Recorder Engine container...${NC}"
        echo -e "${YELLOW}Note: For local testing, web recorder uses ./web_recorder (maps to /opt2 inside container)${NC}"
        echo -e "${YELLOW}Note: For production images, use /web_recorder volume mount${NC}"
        docker run --rm --shm-size="2g" -p 8001:8001 -p 8002:8002 -v ./web_recorder:/opt2 -d \
            ${AGORA_WEB_RECORDER_IMAGE} \
            /usr/src/cloud_recording/run_default.sh --daemon --restful 8001 --probe-port 8002

        echo -e "${GREEN}Web Recorder Engine started successfully!${NC}"
        echo -e "  REST API: http://localhost:8001"
        echo -e "  Probe port: http://localhost:8002"
        echo -e "  Output directory: ./web_recorder (local) -> /opt2 (container)"
        echo -e "  Version: ${AGORA_WEB_RECORDER_VERSION}"
        echo -e "  To stop: docker stop \$(docker ps -q --filter ancestor=${AGORA_WEB_RECORDER_IMAGE})"
        ;;

    images)
        echo -e "${GREEN}Building all standard service images...${NC}"
        create_dirs_docker
        build_service_image "api-server"
        build_service_image "egress"
        build_service_image "flexible-recorder"
        build_service_image "uploader"
        build_service_image "webhook-notifier"

        # Build monolithic image
        echo -e "${GREEN}Building monolithic production image...${NC}"
        docker build -f Dockerfile.prod -t rtc-egress:latest .
        echo -e "${GREEN}Successfully built rtc-egress:latest${NC}"
        ;;

    images-debug)
        echo -e "${GREEN}Building all debug service images...${NC}"
        create_dirs_docker
        build_debug_image "api-server"
        build_debug_image "egress"
        build_debug_image "flexible-recorder"
        build_debug_image "uploader"
        build_debug_image "webhook-notifier"

        # Build monolithic debug image
        echo -e "${GREEN}Building monolithic debug image...${NC}"
        docker build -f Dockerfile.debug -t rtc-egress:debug .
        echo -e "${GREEN}Successfully built rtc-egress:debug${NC}"
        ;;

    image)
        if [ -z "$2" ]; then
            echo -e "${RED}Error: Service name required${NC}"
            echo -e "Usage: $0 image [api-server|egress|flexible-recorder|uploader|webhook-notifier]"
            exit 1
        fi
        create_dirs_docker
        build_service_image "$2"
        ;;

    image-debug)
        if [ -z "$2" ]; then
            echo -e "${RED}Error: Service name required${NC}"
            echo -e "Usage: $0 image-debug [api-server|egress|flexible-recorder|uploader|webhook-notifier]"
            exit 1
        fi
        create_dirs_docker
        build_debug_image "$2"
        ;;

    bump_version)
        bump_version "$2"
        ;;

    *)
        echo "Usage: $0 {all|build|cpp|go|deps|clean|launch_web_recorder|bump_version} [AGORA_SDK_URL|WEB_RECORDER_VERSION|VERSION]"
        echo "  all   - Install dependencies and build all services/components"
        echo "  cpp   - Build only C++ components"
        echo "  go    - Build only Go server"
        echo "  local - Build all services for local execution (no Docker)"
        echo "  ci    - Build for CI/Docker (no formatting, no deps install)"
        echo "  run [service|all] - Run service(s) locally"
        echo "  stop  - Stop all local services"
        echo "  force-kill - Force kill all egress processes with confirmation"
        echo "  format - Run code formatting only"
        echo "  deps  - Install dependencies only"
        echo "  clean - Clean build artifacts"
        echo ""
        echo "  launch_web_recorder [version] - Launch Web Recorder Engine container for testing"
        echo ""
        echo "  bump_version [version] - Bump version and create git tag"
        echo "    Without version: auto-increment patch version (x.y.z -> x.y.z+1)"
        echo "    With version: set specific version (e.g., 1.2.0)"
        echo ""
        echo "  images - Build all production service images (requires ./build.sh local first)"
        echo "  images-debug - Build all debug service images (with source code and symbols)"
        echo "  image [service] - Build specific production service image"
        echo "  image-debug [service] - Build specific debug service image"
        echo ""
        echo "Standard Service Images:"
        echo "  api-server - Task publisher (HTTP → Redis)"
        echo "  egress - Native recording service (handles native tasks only)"
        echo "  flexible-recorder - Web recording service (handles web tasks only)"
        echo "  uploader - File upload service (handles file uploads only)"
        echo "  webhook-notifier - Webhook notification service"
        echo ""
        echo "Storage Paths:"
        echo "  Local development: ./web_recorder (maps to /opt2 in web recorder engine)"
        echo "  Docker production: /web_recorder (volume mount for consistent paths)"
        echo ""
        echo "Notes:"
        echo "  - Agora Web Recorder Engine is a separate third-party service."
        echo "    We do not conflict-check it during preflight."
        echo ""
        echo "For build, you can provide the Agora SDK URL either as:"
        echo "1. Environment variable: export AGORA_SDK_URL=YOUR_Agora_SDK_URL"
        echo "2. Second argument: $0 all YOUR_Agora_SDK_URL"
        echo ""
        echo "For launch_web_recorder, you can provide the version either as:"
        echo "1. Environment variable: export AGORA_WEB_RECORDER_VERSION=YOUR_Agora_WEB_RECORDER_VERSION"
        echo "2. Second argument: $0 launch_web_recorder YOUR_Agora_WEB_RECORDER_VERSION"
        exit 1
        ;;

esac

echo -e "${GREEN}Build completed successfully!${NC}"
