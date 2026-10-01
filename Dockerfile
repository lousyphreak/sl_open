# syntax=docker/dockerfile:1
FROM emscripten/emsdk:6.0.10 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        ninja-build libx11-dev libgl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN CC=gcc CXX=g++ cmake -S third_party/bgfx.cmake -B /build/host -G Ninja \
       -DCMAKE_BUILD_TYPE=Release -DBGFX_BUILD_EXAMPLES=OFF -DBGFX_BUILD_TESTS=OFF \
       -DBGFX_BUILD_TOOLS_BIN2C=OFF -DBGFX_BUILD_TOOLS_GEOMETRY=OFF \
       -DBGFX_BUILD_TOOLS_TEXTURE=OFF \
    && cmake --build /build/host --target shaderc -j 24 \
    && emcmake cmake -S . -B /build/web -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DSL_OPEN_HOST_SHADERC=/build/host/cmake/bgfx/shaderc \
    && cmake --build /build/web --target sl_open -j 24

FROM python:3.13-slim-bookworm AS engine
WORKDIR /srv
COPY --from=build /build/web/sl_open.html /build/web/sl_open.js /build/web/sl_open.wasm ./
COPY third_party/SDL_emfs/tools/emscripten_asset_server.py /usr/local/bin/emscripten_asset_server.py
COPY tools/starlancer_server.py /usr/local/bin/starlancer_server.py
EXPOSE 8080
CMD ["python3", "-u", "/usr/local/bin/starlancer_server.py", "/srv", "--bind", "0.0.0.0", "--port", "8080", "--no-isolation"]

FROM engine AS with-data
COPY --from=game-data / /srv/
