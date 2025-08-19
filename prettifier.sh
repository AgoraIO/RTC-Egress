#!/usr/bin/env bash

# Check if clang-format is installed, if not install it
if ! command -v clang-format &> /dev/null; then
    echo "⚠️  clang-format not found. Attempting to install..."
    if [ -x "/usr/bin/apt-get" ]; then
        sudo apt-get update && sudo apt-get install -y clang-format
    elif [ -x "/usr/bin/yum" ]; then
        sudo yum install -y clang-tools-extra
    elif [ -x "/usr/bin/brew" ]; then
        brew install clang-format
    else
        echo "❌ Could not install clang-format. Please install it manually."
        exit 1
    fi
fi

# Check if goimports is installed, if not install it
if ! command -v goimports &> /dev/null; then
    echo "⚠️  goimports not found. Attempting to install via 'go install'..."
    if command -v go &> /dev/null; then
        go install golang.org/x/tools/cmd/goimports@latest
        # Add GOPATH/bin to PATH if necessary
        export PATH="$PATH:$(go env GOPATH)/bin"
    else
        echo "❌ Go not found. Please install Go and goimports manually."
        exit 1
    fi

    # Re-check
    if ! command -v goimports &> /dev/null; then
        echo "❌ goimports still not found after installation. Please check your GOPATH/bin."
        exit 1
    fi
fi

# Directories to format
TARGET_DIRS=("src" "src/include" "cmd" "pkg" "tests")
# Directories to exclude from formatting (relative to project root)
EXCLUDED_DIRS=("src/common" "src/nlohmann")

# Function to check if a path should be excluded
should_exclude() {
    local path=$1
    for excluded in "${EXCLUDED_DIRS[@]}"; do
        if [[ "$path" == *"$excluded"* ]]; then
            return 0  # true - should be excluded
        fi
    done
    return 1  # false - should not be excluded
}

format_cpp() {
    echo "🔧 Formatting C++ files..."
    for dir in "${TARGET_DIRS[@]}"; do
        if should_exclude "$dir"; then
            echo "  ⏩ Skipping excluded directory: $dir"
            continue
        fi
        
        # Find files and format them, excluding excluded directories
        find "$dir" -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) | while read -r file; do
            if ! should_exclude "$file"; then
                clang-format -i "$file"
            else
                echo "  ⏩ Skipping excluded file: $file"
            fi
        done
    done
}

format_go() {
    echo "🔧 Formatting Go files (goimports)..."
    for dir in "${TARGET_DIRS[@]}"; do
        if should_exclude "$dir"; then
            echo "  ⏩ Skipping excluded directory: $dir"
            continue
        fi
        
        # Find files and format them, excluding excluded directories
        find "$dir" -type f -name "*.go" | while read -r file; do
            if ! should_exclude "$file"; then
                goimports -w "$file"
            else
                echo "  ⏩ Skipping excluded file: $file"
            fi
        done
    done
}

main() {
    format_cpp
    format_go
    echo "✅ All code formatted."
}

main
