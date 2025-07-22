#!/bin/bash

set -e

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${YELLOW}Warning: Not running as root. Some commands might require sudo privileges.${NC}"
fi

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
    cd server
    go mod tidy
    go build -o ../bin/egress
    cd ../
}

# Function to create necessary directories
create_dirs() {
    echo -e "${GREEN}Creating necessary directories...${NC}"
    mkdir -p bin
    mkdir -p logs/egress
    mkdir -p recordings
    mkdir -p snapshots

    # Set permissions
    chmod 755 logs/egress
    chmod 755 recordings
    chmod 755 snapshots

    echo -e "${YELLOW}Note: Using local 'logs' and 'recordings' directories in the project folder.${NC}"
    echo -e "${YELLOW}For production, you may want to use system directories like /var/log/egress and /recordings${NC}"
}

# Function to install dependencies
install_dependencies() {
    echo -e "${GREEN}Installing dependencies...${NC}"
    
    # For Ubuntu/Debian
    if [ -f /etc/debian_version ]; then
        sudo apt-get update
        sudo apt-get install -y \
            build-essential \
            cmake \
            libavcodec-dev \
            libavformat-dev \
            libavutil-dev \
            libswscale-dev \
            libswresample-dev \
            libyaml-cpp-dev \
            libopencv-dev \
            pkg-config \
            golang
    # For CentOS/RHEL
    elif [ -f /etc/redhat-release ]; then
        sudo yum groupinstall -y "Development Tools"
        sudo yum install -y \
            cmake \
            ffmpeg-devel \
            yaml-cpp-devel \
            golang
    else
        echo -e "${YELLOW}Unsupported Linux distribution. Please install dependencies manually.${NC}"
        exit 1
    fi
}

# Default Agora SDK URL (version 4.4.32)
DEFAULT_AGORA_SDK_URL="https://download.agora.io/rtsasdk/release/Agora-RTC-x86_64-linux-gnu-v4.4.32-20250425_144419-675648.tgz"

# Main script
# Check if second argument is a URL
if [[ $2 == http* ]]; then
    AGORA_SDK_URL=$2
    export AGORA_SDK_URL
fi

# Set default URL if not provided
if [ -z "$AGORA_SDK_URL" ] && [ "$1" != "clean" ] && [ "$1" != "go" ]; then
    AGORA_SDK_URL="$DEFAULT_AGORA_SDK_URL"
    echo -e "${YELLOW}Using default Agora SDK URL: $AGORA_SDK_URL${NC}"
    echo -e "${YELLOW}To use a different URL, you can:${NC}"
    echo -e "1. Set environment variable: export AGORA_SDK_URL=YOUR_URL"
    echo -e "2. Or pass as second argument: $0 all YOUR_URL"
fi

./prettifier.sh

case "$1" in
    full)
        install_dependencies
        create_dirs
        build_cpp
        build_go
        ;;
    build)
        build_cpp
        build_go
        ;;
    cpp)
        build_cpp
        ;;
    go)
        build_go
        ;;
    deps)
        install_dependencies
        create_dirs
        ;;
    clean)
        echo -e "${GREEN}Cleaning build directories...${NC}"
        rm -rf build/
        rm -f bin/egress
        rm -f bin/egress-server
        ;;
    *)
        echo "Usage: $0 {all|build|cpp|go|deps|clean} [AGORA_SDK_URL]"
        echo "  all   - Install dependencies and build all components"
        echo "  build - Build all components"
        echo "  cpp   - Build only C++ components"
        echo "  go    - Build only Go server"
        echo "  deps  - Install dependencies only"
        echo "  clean - Clean build artifacts"
        echo ""
        echo "You must provide the Agora SDK URL either as:"
        echo "1. Environment variable: export AGORA_SDK_URL=YOUR_URL"
        echo "2. Second argument: $0 all YOUR_URL"
        exit 1
        ;;
esac

echo -e "${GREEN}Build completed successfully!${NC}"
