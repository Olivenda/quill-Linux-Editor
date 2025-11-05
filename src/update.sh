`#!/usr/bin/env bash
# Quill Updater - downloads and installs the specified release
# Hardcoded to V0.1.1 for reproducibility
set -euo pipefail
IFS=$'\n\t'

# === Configuration ===
UPDATE_URL="https://github.com/Olivenda/quill-Linux-Editor/releases/download/V0.1.1/quill.tar"
INSTALL_DIR="/usr/local/bin"
TMPDIR="$(mktemp -d)"
TARBALL_NAME="quill.tar"

# === Colors ===
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No color

cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

log()  { echo -e "${GREEN}✔${NC} $*"; }
warn() { echo -e "${YELLOW}!${NC} $*"; }
err()  { echo -e "${RED}✖${NC} $*" >&2; exit 1; }

echo -e "${YELLOW}🔄 Starting Quill update...${NC}"
echo -e "${YELLOW}🌐 Download URL:${NC} $UPDATE_URL"

cd "$TMPDIR"

# --- Download ---
if ! wget -q --show-progress "$UPDATE_URL" -O "$TARBALL_NAME"; then
    err "Failed to download update from GitHub."
fi
log "Download complete."

# --- Backup old installation ---
if [[ -d "$INSTALL_DIR" ]]; then
    BACKUP_DIR="${INSTALL_DIR}_backup_$(date +%Y%m%d_%H%M%S)"
    warn "Backing up existing installation to: $BACKUP_DIR"
    sudo cp -a "$INSTALL_DIR" "$BACKUP_DIR"
fi

# --- Extract new version ---
warn "Extracting update into $INSTALL_DIR..."
sudo tar -xf "$TARBALL_NAME" -C "$INSTALL_DIR" --strip-components=0
cd /usr/local/bin
sudo gcc -o quill editor.cpp -lncurses

log "Extraction complete."

# --- Cleanup ---
rm -f "$TARBALL_NAME"

log "✅ Quill updated successfully!"
log "Version: V0.1.1"
