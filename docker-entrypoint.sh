#!/bin/bash
set -e

# Create necessary directories
ensure_directories() {
    mkdir -p /recordings /snapshots /web_recorder /var/log/rtc_egress /tmp/egress
    chown -R rtc_egress:rtc_egress /recordings /snapshots /web_recorder /var/log/rtc_egress /tmp/egress 2>/dev/null || true
}

# Check if this is a monolithic container (has all 5 binaries)
is_monolithic() {
    [ -f "./bin/api-server" ] && [ -f "./bin/egress" ] && [ -f "./bin/flexible-recorder" ] && [ -f "./bin/uploader" ] && [ -f "./bin/webhook-notifier" ]
}

# Start all services for monolithic deployment
start_all_services() {
    echo "🚀 Starting all RTC Egress services in monolithic mode..."
    
    # Ensure directories exist
    ensure_directories
    
    # Set library path
    export LD_LIBRARY_PATH="/opt/rtc_egress/bin:/usr/local/lib:$LD_LIBRARY_PATH"
    
    # Start services in background
    echo "📡 Starting API Server..."
    ./bin/api-server --config /opt/rtc_egress/config/api_server_config.yaml > /var/log/rtc_egress/api-server.log 2>&1 &
    API_PID=$!
    
    echo "🎥 Starting Egress (native recording)..."
    ./bin/egress --config /opt/rtc_egress/config/egress_config.yaml > /var/log/rtc_egress/egress.log 2>&1 &
    EGRESS_PID=$!
    
    echo "🌐 Starting Flexible Recorder (web recording)..."
    ./bin/flexible-recorder --config /opt/rtc_egress/config/flexible_recorder_config.yaml > /var/log/rtc_egress/flexible-recorder.log 2>&1 &
    FLEXIBLE_PID=$!
    
    echo "☁️ Starting Uploader..."
    ./bin/uploader --config /opt/rtc_egress/config/uploader_config.yaml > /var/log/rtc_egress/uploader.log 2>&1 &
    UPLOADER_PID=$!
    
    echo "🔔 Starting Webhook Notifier..."
    ./bin/webhook-notifier --config /opt/rtc_egress/config/webhook_notifier_config.yaml > /var/log/rtc_egress/webhook-notifier.log 2>&1 &
    WEBHOOK_PID=$!
    
    # Store PIDs for cleanup
    echo "$API_PID $EGRESS_PID $FLEXIBLE_PID $UPLOADER_PID $WEBHOOK_PID" > /tmp/service.pids
    
    echo "✅ All services started successfully!"
    echo "📋 Service PIDs: API($API_PID) Egress($EGRESS_PID) Flexible($FLEXIBLE_PID) Uploader($UPLOADER_PID) Webhook($WEBHOOK_PID)"
    echo "📄 Logs available in /var/log/rtc_egress/"
    
    # Function to handle shutdown gracefully
    shutdown_services() {
        echo "🛑 Shutting down all services..."
        if [ -f /tmp/service.pids ]; then
            PIDS=$(cat /tmp/service.pids)
            for pid in $PIDS; do
                if kill -0 $pid 2>/dev/null; then
                    echo "Stopping process $pid..."
                    kill $pid
                fi
            done
            # Wait a bit for graceful shutdown
            sleep 3
            # Force kill if still running
            for pid in $PIDS; do
                if kill -0 $pid 2>/dev/null; then
                    echo "Force stopping process $pid..."
                    kill -9 $pid
                fi
            done
            rm -f /tmp/service.pids
        fi
        exit 0
    }
    
    # Set up signal handlers
    trap shutdown_services SIGTERM SIGINT
    
    # Wait for all services (keep container running)
    wait
}

# Main execution
main() {
    # If no arguments provided and this is monolithic, start all services
    if [ $# -eq 0 ] && is_monolithic; then
        start_all_services
    # If specific arguments provided or single service image
    else
        echo "Starting single service: $@"
        
        # Ensure directories exist
        ensure_directories
        
        # Set library path
        export LD_LIBRARY_PATH="/opt/rtc_egress/bin:/usr/local/lib:$LD_LIBRARY_PATH"
        
        # Execute the specified command
        echo "Executing: $@"
        exec "$@"
    fi
}

# Run main function
main "$@"