#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 IMAGE_REPOSITORY RETAIL_GAME_DIRECTORY" >&2
    exit 2
fi

image_repository=$1
game_dir=$2
docker buildx build --load \
    --build-context "game-data=$game_dir" \
    --target with-data \
    --tag "$image_repository:latest" .
