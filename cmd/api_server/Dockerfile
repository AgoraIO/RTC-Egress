# API Server - Production Image (Pre-built binary)
FROM ubuntu:24.04

# Set timezone and avoid interactive prompts
ENV TZ=UTC
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Copy pre-built binary and config
COPY bin/api-server ./bin/
COPY config/ ./config/

# Create non-root user
RUN groupadd -r rtc_egress && \
    useradd -r -g rtc_egress -s /bin/false rtc_egress

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