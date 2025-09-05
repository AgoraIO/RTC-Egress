# Webhook Notifier - Production Image (Pre-built binary)
FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# Copy pre-built binary from local build
COPY bin/webhook-notifier ./bin/

# Create non-root user
RUN groupadd -r rtc_egress && \
    useradd -r -g rtc_egress -s /bin/false rtc_egress

# Create directories
RUN mkdir -p /opt/config /var/log/webhook_notifier && \
    chown -R rtc_egress:rtc_egress /opt /var/log/webhook_notifier

USER rtc_egress

# Expose ports (dynamic from config)
EXPOSE 8195

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=30s --retries=3 \
    CMD curl -f http://localhost:8195/health || exit 1

# Default command
CMD ["./bin/webhook-notifier", "--config", "/opt/config/webhook_notifier_config.yaml"]