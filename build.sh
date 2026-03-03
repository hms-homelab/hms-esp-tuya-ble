#!/bin/bash

# ESP-IDF Build Script for Tuya BLE MQTT Bridge

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Tuya BLE MQTT Bridge Build Script${NC}"
echo "=================================="

# Check if ESP-IDF is installed
if [ ! -d "$HOME/esp/esp-idf" ]; then
    echo -e "${RED}Error: ESP-IDF not found at ~/esp/esp-idf${NC}"
    echo "Please install ESP-IDF first:"
    echo "  mkdir -p ~/esp"
    echo "  cd ~/esp"
    echo "  git clone --recursive https://github.com/espressif/esp-idf.git"
    echo "  cd esp-idf"
    echo "  ./install.sh esp32c3"
    exit 1
fi

# Source ESP-IDF environment
echo -e "${YELLOW}Setting up ESP-IDF environment...${NC}"
source ~/esp/esp-idf/export.sh

# Parse command line arguments
case "$1" in
    "clean")
        echo -e "${YELLOW}Cleaning build directory...${NC}"
        idf.py fullclean
        ;;
    "menuconfig")
        echo -e "${YELLOW}Opening configuration menu...${NC}"
        idf.py menuconfig
        ;;
    "flash")
        echo -e "${YELLOW}Building and flashing...${NC}"
        idf.py build flash monitor
        ;;
    "monitor")
        echo -e "${YELLOW}Starting monitor...${NC}"
        idf.py monitor
        ;;
    "size")
        echo -e "${YELLOW}Analyzing binary size...${NC}"
        idf.py size
        ;;
    *)
        echo -e "${YELLOW}Building project...${NC}"
        idf.py build
        
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}Build successful!${NC}"
            echo ""
            echo "Available commands:"
            echo "  ./build.sh          - Build project"
            echo "  ./build.sh clean    - Clean build"
            echo "  ./build.sh menuconfig - Configure project"
            echo "  ./build.sh flash    - Build, flash and monitor"
            echo "  ./build.sh monitor  - Start serial monitor"
            echo "  ./build.sh size     - Analyze binary size"
        else
            echo -e "${RED}Build failed!${NC}"
            exit 1
        fi
        ;;
esac