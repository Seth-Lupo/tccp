#!/bin/sh
set -e

# tccp installer for macOS and Linux

REPO="Seth-Lupo/tccp"

# detect platform
OS=$(uname -s)
ARCH=$(uname -m)

case "$OS" in
    Darwin)
        case "$ARCH" in
            arm64) BINARY="tccp-macos-arm64" ;;
            *)     echo "Unsupported architecture: $ARCH (only Apple Silicon is supported)"; exit 1 ;;
        esac
        ;;
    Linux)
        case "$ARCH" in
            x86_64) BINARY="tccp-linux-x64" ;;
            *)      echo "Unsupported architecture: $ARCH (only x64 is supported)"; exit 1 ;;
        esac
        ;;
    *)
        echo "Unsupported OS: $OS"; exit 1
        ;;
esac

URL="https://github.com/$REPO/releases/latest/download/$BINARY"
INSTALL_DIR="/usr/local/bin"

echo "Downloading $BINARY..."
curl -fSL -o tccp "$URL"
chmod +x tccp

echo "Installing to $INSTALL_DIR/tccp (may need sudo)..."
if [ -w "$INSTALL_DIR" ]; then
    mv tccp "$INSTALL_DIR/tccp"
else
    sudo mv tccp "$INSTALL_DIR/tccp"
fi

echo "Done. Run 'tccp --version' to verify."
