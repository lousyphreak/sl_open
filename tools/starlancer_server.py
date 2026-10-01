#!/usr/bin/env python3

import argparse
import base64
import hmac
import http
import http.server
import os
import pathlib
import urllib.parse

from emscripten_asset_server import AssetServer, build_index, build_manifest, etag_for


class StarLancerServer(AssetServer):
    def serve(self, send_body):
        expected = self.server.auth_token
        if expected and not hmac.compare_digest(self.headers.get('Authorization', ''), expected):
            self.send_response(http.HTTPStatus.UNAUTHORIZED)
            self.send_header('WWW-Authenticate', 'Basic realm="StarLancer"')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return

        if urllib.parse.urlsplit(self.path).path in ('/', '/index.html'):
            self.send_response(http.HTTPStatus.FOUND)
            self.send_header('Location', '/sl_open.html')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return

        super().serve(send_body)


def main():
    parser = argparse.ArgumentParser(description='Serve sl_open with SDL_emfs assets')
    parser.add_argument('asset_root', type=pathlib.Path)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--no-isolation', action='store_true')
    arguments = parser.parse_args()

    root = arguments.asset_root.resolve()
    files = build_index(root)
    server = http.server.ThreadingHTTPServer((arguments.bind, arguments.port), StarLancerServer)
    server.files = files
    server.etag_function = etag_for
    server.manifest = build_manifest(files)
    server.isolation = not arguments.no_isolation
    username = os.environ.get('HTTP_BASIC_AUTH_USERNAME')
    password = os.environ.get('HTTP_BASIC_AUTH_PASSWORD')
    credentials = f'{username}:{password}'.encode('utf-8')
    server.auth_token = 'Basic ' + base64.b64encode(credentials).decode('ascii') if username and password else None
    print(f'Serving {len(files)} SDL_emfs assets from {root} at http://{arguments.bind}:{arguments.port}/')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
