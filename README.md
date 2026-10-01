# sl_open

sl_open is a portable C++20 reimplementation of StarLancer, based on reverse
engineering of the original executable. It uses SDL3 for platform integration,
bgfx for rendering, SDL_emfs for file access and saves, and OpenAL for audio.

A legally obtained retail StarLancer installation is required. Original game
assets and executables are not included. The runtime implements the frontend,
campaign, mission simulation, and multiplayer; work on fidelity and cleanup
is ongoing.

## Native build

CMake 3.25 or newer, Ninja, and a C++20 compiler are required. Linux with GCC is
the primary development platform. Windows and Emscripten build paths are also
present.

```sh
git submodule update --init --recursive
cmake --preset linux-release
cmake --build --preset linux-release --target sl_open -j 24
./build/linux-release/sl_open --data /path/to/game
```

Point `--data` at the retail directory containing `resource.hog`, `CD1.HOG`,
`CD2.HOG`, and `LANGUAGE.DLL`, with the rest of the installation's files and
subdirectories in place. Without `--data`, assets are read from the current
working directory. `--loose-first` gives loose files priority over archives.

For a debug build, use the `linux-debug` presets. The optional
`sl_open_inspect` target inspects mission, language, sprite, and model data.

Configuration and campaign saves use a separate SDL preference directory;
see [configuration and saves](docs/configuration.md).

## Browser build

Activate an Emscripten environment, then build the native shader compiler and
cross-compile the game:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug --target shaderc -j 24
emcmake cmake --preset web-release
cmake --build --preset web-release --target sl_open -j 24
```

Set `SL_OPEN_HOST_SHADERC` during configuration to use a host shader compiler
from a different location. The default is the `linux-debug` build's compiler.

The browser build produces `sl_open.html`, `sl_open.js`, and `sl_open.wasm`
in `build/web-release`. Serve them beside the retail game files with the
SDL_emfs range-aware asset server:

```sh
python3 third_party/SDL_emfs/tools/emscripten_asset_server.py \
  /path/to/web-and-game-files --port 8000 --no-isolation
```

Open `http://127.0.0.1:8000/sl_open.html`. Browser settings and saves persist
in IndexedDB.

## Container and Kubernetes

Build the `engine` image to package only the browser runtime and asset server:

```sh
docker build --target engine -t starlancer:latest .
```

Mount a directory containing the browser output and retail files at `/srv` to
serve a complete installation. Alternatively, create a private image including
your retail data:

```sh
./tools/build-image.sh registry.example.com/starlancer /path/to/game
docker run --rm -p 127.0.0.1:8080:8080 registry.example.com/starlancer:latest
```

Open `http://127.0.0.1:8080/`; the server redirects to `/sl_open.html`. Images
built with `build-image.sh` contain retail game data and must stay private.

Docker images and Kubernetes deployments retain the `starlancer` name.
The chart is in `helm/starlancer`. Supply your image repository and a Secret
with `username` and `password` keys through `image.repository` and
`authSecretName`. The default Secret name is `starlancer-web-basic-auth`.

```sh
helm upgrade --install starlancer-web helm/starlancer \
  --set image.repository=registry.example.com/starlancer \
  --set authSecretName=starlancer-web-basic-auth
```

The chart always pulls `latest`; after pushing a replacement image, restart
the Deployment to pull it. Ingress is optional and configured through the
chart's `ingress` values. Deployment-specific values belong in an untracked
`values-local.yaml` file. The server needs no persistent volume for saves.

## Dependencies and notices

Dependencies are pinned Git submodules: SDL3, SDL_emfs, bgfx.cmake (including
bgfx, bx, bimg, and shaderc), GLM, OpenAL Soft, and dr_libs. The browser uses
Emscripten's OpenAL implementation. Bink decoding includes adapted FFmpeg
code; its provenance and LGPL notice are in
[third_party/ffmpeg-bink](third_party/ffmpeg-bink/README.md). Upstream copyright
and license notices remain with their code.

## License

sl_open is licensed under the GNU General Public License, version 3 or
(at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE).
Third-party code retains its own copyright notices and licenses; SDL_emfs
is also licensed under GPL-3.0-or-later.
