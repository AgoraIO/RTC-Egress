# API Server - Production Image (Pre-built binary)
FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Copy pre-built binary from local build
COPY bin/api-server ./bin/

# Create non-root user
RUN addgroup --gid 1000 rtc_egress && \
    adduser --disabled-password --gecos '' --uid 1000 --gid 1000 rtc_egress

# Create directories
RUN mkdir -p /opt/config /var/log/api_server && \
    chown -R rtc_egress:rtc_egress /opt /var/log/api_server

USER rtc_egress

# Expose ports (dynamic from config)
EXPOSE 8091 8191

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8191/health || exit 1

# Default command
CMD ["./bin/api-server", "--config", "/opt/config/api_server_config.yaml"]