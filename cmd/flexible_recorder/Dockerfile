# Flexible Recorder - Production Image (Pre-built binary)
FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Copy pre-built binary from local build
COPY bin/flexible-recorder ./bin/

# Create non-root user
RUN groupadd -r rtc_egress && \
    useradd -r -g rtc_egress -s /bin/false rtc_egress

# Create directories
RUN mkdir -p /opt/config /var/log/flexible_recorder /web_recorder && \
    chown -R rtc_egress:rtc_egress /opt /var/log/flexible_recorder /web_recorder

USER rtc_egress

# Expose ports (dynamic from config)
EXPOSE 8193

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8193/health || exit 1

# Default command
CMD ["./bin/flexible-recorder", "--config", "/opt/config/flexible_recorder_config.yaml"]