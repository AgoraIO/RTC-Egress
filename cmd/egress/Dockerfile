# Egress - Production Image (Pre-built binaries)
FROM ubuntu:24.04

# Set timezone and avoid interactive prompts
ENV TZ=UTC
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies only
RUN sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g' /etc/apt/sources.list && \
    apt-get update && apt-get install -y \
    curl \
    libavcodec60 \
    libavformat60 \
    libswscale7 \
    libyaml-cpp0.8 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Copy pre-built binaries and config
COPY bin/egress ./bin/
COPY bin/eg_worker ./bin/
# Ensure binaries are executable
RUN chmod +x ./bin/egress ./bin/eg_worker && chmod 755 /opt /opt/bin || true
COPY config/ ./config/

# Copy Agora SDK runtime libraries
COPY bin/*.so ./lib/

# Create directory structure
RUN mkdir -p /opt/config /opt/logs /recordings /snapshots /web_recorder

# Set library path for Agora SDK
ENV LD_LIBRARY_PATH=/opt/lib:$LD_LIBRARY_PATH

# Create non-root user
RUN groupadd -r rtc_egress && \
    useradd -r -g rtc_egress -s /bin/false rtc_egress && \
    chown -R rtc_egress:rtc_egress /opt /recordings /snapshots /web_recorder

USER rtc_egress

# Expose ports (dynamic from config)
EXPOSE 8182

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8182/health || exit 1

# Default command (absolute path for reliability)
CMD ["/opt/bin/egress", "--config", "/opt/config/egress_config.yaml"]
