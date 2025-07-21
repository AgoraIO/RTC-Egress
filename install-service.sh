#!/bin/bash

set -e

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Error: This script must be run as root${NC}"
    exit 1
fi

# Create egress user if it doesn't exist
if ! id -u egress &>/dev/null; then
    echo -e "${GREEN}Creating egress user...${NC}"
    useradd --system --shell /bin/false --home-dir /nonexistent egress
fi

# Create installation directory
INSTALL_DIR="/opt/egress-recorder"
BIN_DIR="$INSTALL_DIR/bin"
CONFIG_DIR="/etc/egress/config"

# Create directories
echo -e "${GREEN}Creating directories...${NC}"
mkdir -p "$BIN_DIR"
mkdir -p "$CONFIG_DIR"
mkdir -p /var/log/egress
mkdir -p /recordings

# Copy files
echo -e "${GREEN}Copying files...${NC}"
cp bin/egress "$BIN_DIR/"
cp bin/egress-server "$BIN_DIR/"
cp config/egress_config.yaml "$CONFIG_DIR/"
cp egress-recorder.service /etc/systemd/system/

# Set permissions
echo -e "${GREEN}Setting permissions...${NC}"
chown -R egress:egress "$INSTALL_DIR"
chown -R egress:egress /var/log/egress
chown -R egress:egress /recordings
chmod 755 "$BIN_DIR/egress"
chmod 755 "$BIN_DIR/egress-server"
chmod 644 "$CONFIG_DIR/egress_config.yaml"
chmod 644 /etc/systemd/system/egress-recorder.service

# Reload systemd
echo -e "${GREEN}Reloading systemd...${NC}"
systemctl daemon-reload

# Enable and start service
echo -e "${GREEN}Enabling and starting service...${NC}"
systemctl enable egress-recorder.service
systemctl start egress-recorder.service

echo -e "\n${GREEN}Installation completed successfully!${NC}"
echo -e "Service is now running. You can check the status with: systemctl status egress-recorder"
echo -e "Logs can be viewed with: journalctl -u egress-recorder -f"

# Make the script executable
chmod +x install-service.sh
