#!/bin/bash
set -e

TAG=${1}

if [ -z "$TAG" ]; then
    echo "Usage: ./release.sh <tag> (e.g. ./release.sh v0.2.0)"
    exit 1
fi

REPO="Edu_Coder/esp-airsync"
ESPHOME_LIB_DIR="$(cd "$(dirname "$0")" && pwd)/components/raop_receiver/libs"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
fi

if [ -z "$CODEBERG_TOKEN" ]; then
    echo "Error: CODEBERG_TOKEN not set. Add it to .env or set it in your environment."
    exit 1
fi

echo "Building esp32..."
unset IDF_TARGET
idf.py fullclean && idf.py set-target esp32 && idf.py build
cp build/esp-airsync.bin esp-airsync-esp32.bin
cp build/esp-idf/esp-raop-receiver/libesp-raop-receiver.a "$ESPHOME_LIB_DIR/esp32/libesp-raop-receiver.a"

echo "Building esp32s3..."
unset IDF_TARGET
idf.py fullclean && idf.py set-target esp32s3 && idf.py build
cp build/esp-airsync.bin esp-airsync-esp32s3.bin
cp build/esp-idf/esp-raop-receiver/libesp-raop-receiver.a "$ESPHOME_LIB_DIR/esp32s3/libesp-raop-receiver.a"

echo "Committing updated libraries..."
git add "$ESPHOME_LIB_DIR"
git commit -m "release: update precompiled libraries for $TAG"
git tag "$TAG"
git push origin main
git push origin "$TAG"

echo "Creating release $TAG..."
RELEASE=$(curl -s -X POST \
    -H "Authorization: token $CODEBERG_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"tag_name\": \"$TAG\", \"name\": \"$TAG\", \"draft\": false}" \
    https://codeberg.org/api/v1/repos/$REPO/releases)

RELEASE_ID=$(echo $RELEASE | grep -o '"id":[0-9]*' | head -1 | cut -d: -f2)

if [ -z "$RELEASE_ID" ]; then
    echo "Error: Failed to create release. Response: $RELEASE"
    exit 1
fi

echo "Uploading binaries..."
curl -s -X POST \
    -H "Authorization: token $CODEBERG_TOKEN" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @esp-airsync-esp32.bin \
    "https://codeberg.org/api/v1/repos/$REPO/releases/$RELEASE_ID/assets?name=esp-airsync-esp32.bin"

curl -s -X POST \
    -H "Authorization: token $CODEBERG_TOKEN" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @esp-airsync-esp32s3.bin \
    "https://codeberg.org/api/v1/repos/$REPO/releases/$RELEASE_ID/assets?name=esp-airsync-esp32s3.bin"

echo "Cleaning up..."
rm esp-airsync-esp32.bin esp-airsync-esp32s3.bin

echo "Done! Release $TAG created."