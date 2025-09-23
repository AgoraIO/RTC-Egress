# Flexible Recorder - Production Image (Pre-built binary)
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
COPY bin/flexible-recorder ./bin/
# Ensure binary is executable
RUN chmod +x ./bin/flexible-recorder && chmod 755 /opt /opt/bin || true
COPY config/ ./config/

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

# Default config directory for containers
ENV CONFIG_DIR=/opt/rtc_egress/config

# Default command (absolute path)
CMD ["/opt/bin/flexible-recorder"]
