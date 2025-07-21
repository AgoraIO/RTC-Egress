# Build stage
FROM golang:1.21 AS go-builder
WORKDIR /app

# Install Go dependencies
COPY go.mod go.sum ./
RUN go mod download

# Copy source code
COPY cmd ./cmd
COPY internal ./internal

# Build Go server
RUN CGO_ENABLED=0 GOOS=linux go build -o /bin/egress-server ./cmd/egress-server

# C++ build stage
FROM ubuntu:22.04 AS cpp-builder

# Set up environment
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libyaml-cpp-dev \
    pkg-config \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Copy Agora SDK (assuming it's pre-downloaded)
COPY agora_sdk /agora_sdk

# Copy source code
WORKDIR /app
COPY src /app/src
COPY CMakeLists.txt /app/

# Build C++ application
RUN mkdir -p /app/build && \
    cd /app/build && \
    cmake .. && \
    make -j$(nproc) && \
    mkdir -p /app/bin && \
    cp egress /app/bin/

# Final stage
FROM ubuntu:22.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libavcodec58 \
    libavformat58 \
    libavutil56 \
    libswscale5 \
    libswresample3 \
    libyaml-cpp0.7 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy binaries and configs
COPY --from=go-builder /bin/egress-server /usr/local/bin/
COPY --from=cpp-builder /app/bin/egress /usr/local/bin/
COPY config /etc/egress/config
COPY web /app/web

# Create necessary directories
RUN mkdir -p /var/log/egress /var/run/egress /recordings \
    && chmod 777 /recordings

# Expose ports
EXPOSE 8080 8182 3000

# Set working directory
WORKDIR /app

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8182/health || exit 1

# Default command
CMD ["/usr/local/bin/egress-server"]
