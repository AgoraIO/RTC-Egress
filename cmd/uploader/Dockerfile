# Uploader - Production Image (Pre-built binary)
FROM ubuntu:24.04

# Set timezone and avoid interactive prompts
ENV TZ=UTC
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies
RUN sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g' /etc/apt/sources.list && \
    apt-get update && apt-get install -y \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Copy pre-built binary and config
COPY bin/uploader ./bin/
COPY config/ ./config/

# Create directory structure
RUN mkdir -p /opt/config /opt/logs /recordings /snapshots /web_recorder

# Create non-root user
RUN groupadd -r rtc_egress && \
    useradd -r -g rtc_egress -s /bin/false rtc_egress && \
    chown -R rtc_egress:rtc_egress /opt /recordings /snapshots /web_recorder

USER rtc_egress

# Expose health port (dynamic from config)
EXPOSE 8194

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8194/health || exit 1

# Default command
CMD ["./bin/uploader", "--config", "/opt/config/uploader_config.yaml"]