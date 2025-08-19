#!/bin/bash
set -e

# Create necessary directories
ensure_directories() {
    mkdir -p /recordings /snapshots /var/log/ag_egress /tmp/egress
    chown -R egress:egress /recordings /snapshots /var/log/ag_egress /tmp/egress 2>/dev/null || true
}

# Main execution
main() {
    echo "Starting Agora RTC Egress service..."

    # Ensure directories exist
    ensure_directories

    # Shared libraries are installed at build time

    echo "Environment variables will be processed by the application"

    # Execute the command
    echo "Executing: $@"
    exec "$@"
}

# Run main function
main "$@"