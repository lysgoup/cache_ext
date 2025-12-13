#!/bin/bash
set -eu -o pipefail

echo "Installing dependencies..."
sudo apt-get update
sudo apt-get install -y rclone

SCRIPT_PATH=$(realpath $0)
BASE_DIR=$(dirname $SCRIPT_PATH)
# realpath is needed here due to an rclone quirk
DB_PATH=$(realpath $BASE_DIR/..)

BUCKET="cache-ext-artifact-data"

cd "$DB_PATH"

echo "Downloading LevelDB database from GCS (bucket: ${BUCKET})..."
echo "This will download to: ${DB_PATH}/leveldb/"
echo ""

# 크기 확인 (optional)
echo "Checking database size..."
rclone size --gcs-anonymous :gcs:${BUCKET}/leveldb || true
echo ""

read -p "Continue with download? (y/n) " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Download cancelled."
    exit 0
fi

echo "Downloading LevelDB database..."
rclone copy --progress --transfers 64 --checkers 64 --gcs-anonymous \
    :gcs:${BUCKET}/leveldb "${DB_PATH}/leveldb/"

echo ""
echo "Verifying download..."
rclone check --progress --transfers 64 --checkers 64 --gcs-anonymous \
    :gcs:${BUCKET}/leveldb "${DB_PATH}/leveldb/"

echo ""
echo "Download complete!"
echo "Database location: ${DB_PATH}/leveldb/"
echo ""
echo "You can now test adaptive policy with:"
echo "  --watch_dir ${DB_PATH}/leveldb/"
