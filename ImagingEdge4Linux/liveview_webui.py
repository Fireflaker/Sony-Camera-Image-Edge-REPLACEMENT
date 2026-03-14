#!/bin/python3

import argparse
import json
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import requests


class SonyCameraClient:
    def __init__(self, address: str, port: int):
        self.base = f"http://{address}:{port}/sony/camera"

    def call(self, method: str, params=None, version="1.0"):
        if params is None:
            params = []

        response = requests.post(
            self.base,
            json={
                "method": method,
                "params": params,
                "id": 1,
                "version": version,
            },
            timeout=8,
        )
        response.raise_for_status()
        return response.json()

    def has_error(self, result):
        return isinstance(result, dict) and "error" in result


class AppState:
    def __init__(self, camera: SonyCameraClient):
        self.camera = camera
        self.lock = threading.RLock()
        self.liveview_url = None
        self.streaming_enabled = False
        self.movie_recording = False
        self.latest_frame = None
        self.frame_count = 0
        self.grabber_thread = None
        self.stop_grabber = threading.Event()

    def _ensure_grabber_thread(self):
        if self.grabber_thread and self.grabber_thread.is_alive():
            return

        self.stop_grabber.clear()
        self.grabber_thread = threading.Thread(target=self._grabber_loop, daemon=True)
        self.grabber_thread.start()

    def _grabber_loop(self):
        while not self.stop_grabber.is_set():
            with self.lock:
                enabled = self.streaming_enabled
                url = self.liveview_url

            if not enabled or not url:
                self.stop_grabber.wait(0.2)
                continue

            try:
                with requests.get(url, stream=True, timeout=(8, 20)) as resp:
                    resp.raise_for_status()
                    for frame in extract_jpeg_frames(resp.iter_content(chunk_size=32768)):
                        if self.stop_grabber.is_set():
                            break

                        with self.lock:
                            if not self.streaming_enabled:
                                break

                            self.latest_frame = frame
                            self.frame_count += 1
            except Exception:
                self.stop_grabber.wait(0.5)

    def start_liveview(self):
        with self.lock:
            try:
                # harmless if camera is already in recording mode or does not need it
                self.camera.call("startRecMode")
            except Exception:
                pass

            result = self.camera.call("startLiveview")
            if self.camera.has_error(result):
                return False, result

            self.liveview_url = result.get("result", [None])[0]
            self.streaming_enabled = True
            self._ensure_grabber_thread()
            return bool(self.liveview_url), result

    def stop_liveview(self):
        with self.lock:
            result = self.camera.call("stopLiveview")
            if self.camera.has_error(result):
                return False, result

            self.streaming_enabled = False
            self.latest_frame = None
            return True, result

    def start_movie_rec(self):
        with self.lock:
            result = self.camera.call("startMovieRec")
            if self.camera.has_error(result):
                return False, result

            self.movie_recording = True
            return True, result

    def stop_movie_rec(self):
        with self.lock:
            result = self.camera.call("stopMovieRec")
            if self.camera.has_error(result):
                return False, result

            self.movie_recording = False
            return True, result

    def available_api_list(self):
        with self.lock:
            return self.camera.call("getAvailableApiList")

    def health(self):
        with self.lock:
            versions = self.camera.call("getVersions")
            apis = self.camera.call("getAvailableApiList")
            return {
                "versions": versions,
                "apis": apis,
                "liveviewUrl": self.liveview_url,
                "streamingEnabled": self.streaming_enabled,
                "frameCount": self.frame_count,
            }


def extract_jpeg_frames(raw_iter, chunk_size=32768):
    """
    Convert Sony liveview stream payload into plain JPEG frames by scanning for
    JPEG start/end markers.
    """
    buffer = bytearray()

    for chunk in raw_iter:
        if not chunk:
            continue

        buffer.extend(chunk)

        while True:
            soi = buffer.find(b"\xff\xd8")
            if soi == -1:
                # keep buffer bounded
                if len(buffer) > 4 * 1024 * 1024:
                    del buffer[:-1024]
                break

            eoi = buffer.find(b"\xff\xd9", soi + 2)
            if eoi == -1:
                # keep bytes from SOI onward while waiting for frame end
                if soi > 0:
                    del buffer[:soi]
                break

            frame = bytes(buffer[soi : eoi + 2])
            del buffer[: eoi + 2]
            yield frame


class LiveviewHandler(BaseHTTPRequestHandler):
    server_version = "SonyLiveviewWebUI/1.0"

    @property
    def state(self) -> AppState:
        return self.server.state

    def _send_json(self, payload, code=HTTPStatus.OK):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html):
        body = html.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/":
            self._send_html(INDEX_HTML)
            return

        if parsed.path == "/api/status":
            with self.state.lock:
                self._send_json(
                    {
                        "liveviewUrl": self.state.liveview_url,
                        "streamingEnabled": self.state.streaming_enabled,
                        "movieRecording": self.state.movie_recording,
                        "frameCount": self.state.frame_count,
                    }
                )
            return

        if parsed.path == "/api/health":
            try:
                self._send_json({"ok": True, "result": self.state.health()})
            except Exception as exc:
                self._send_json({"ok": False, "error": str(exc)}, HTTPStatus.INTERNAL_SERVER_ERROR)
            return

        if parsed.path == "/frame.jpg":
            self._send_latest_frame()
            return

        if parsed.path == "/stream":
            self._stream_liveview()
            return

        self.send_error(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self):
        parsed = urlparse(self.path)

        try:
            if parsed.path == "/api/start_liveview":
                ok, result = self.state.start_liveview()
                self._send_json({"ok": ok, "result": result}, HTTPStatus.OK if ok else HTTPStatus.BAD_REQUEST)
                return

            if parsed.path == "/api/stop_liveview":
                ok, result = self.state.stop_liveview()
                self._send_json({"ok": ok, "result": result}, HTTPStatus.OK if ok else HTTPStatus.BAD_REQUEST)
                return

            if parsed.path == "/api/start_movie":
                ok, result = self.state.start_movie_rec()
                self._send_json({"ok": ok, "result": result}, HTTPStatus.OK if ok else HTTPStatus.BAD_REQUEST)
                return

            if parsed.path == "/api/stop_movie":
                ok, result = self.state.stop_movie_rec()
                self._send_json({"ok": ok, "result": result}, HTTPStatus.OK if ok else HTTPStatus.BAD_REQUEST)
                return

            if parsed.path == "/api/apis":
                result = self.state.available_api_list()
                self._send_json({"ok": True, "result": result})
                return

            self.send_error(HTTPStatus.NOT_FOUND, "Not found")
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, HTTPStatus.INTERNAL_SERVER_ERROR)

    def _stream_liveview(self):
        with self.state.lock:
            need_start = (not self.state.liveview_url) or (not self.state.streaming_enabled)

        if need_start:
            ok, result = self.state.start_liveview()
            if not ok:
                self._send_json({"ok": False, "error": "Could not start liveview", "result": result}, HTTPStatus.BAD_REQUEST)
                return

        with self.state.lock:
            liveview_url = self.state.liveview_url

        self.send_response(HTTPStatus.OK)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()

        try:
            with requests.get(liveview_url, stream=True, timeout=(8, 20)) as resp:
                resp.raise_for_status()

                for frame in extract_jpeg_frames(resp.iter_content(chunk_size=32768)):
                    with self.state.lock:
                        if not self.state.streaming_enabled:
                            break

                    self.wfile.write(b"--frame\r\n")
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(f"Content-Length: {len(frame)}\r\n\r\n".encode("ascii"))
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError):
            # browser disconnected
            pass
        except Exception:
            pass

    def _send_latest_frame(self):
        with self.state.lock:
            frame = self.state.latest_frame

        if not frame:
            self.send_error(HTTPStatus.SERVICE_UNAVAILABLE, "No frame yet")
            return

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        self.send_header("Content-Length", str(len(frame)))
        self.end_headers()
        self.wfile.write(frame)


INDEX_HTML = """<!doctype html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\" />
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
  <title>Sony Liveview Control</title>
  <style>
    body { font-family: Segoe UI, Arial, sans-serif; margin: 16px; background: #111; color: #f1f1f1; }
    .toolbar { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 12px; }
    button { padding: 10px 14px; border: 0; border-radius: 8px; cursor: pointer; }
    button.primary { background: #1f6feb; color: white; }
    button.warn { background: #f59e0b; color: #111; }
    button.danger { background: #ef4444; color: white; }
    #status { margin: 8px 0 14px; color: #b4b4b4; }
    #stream { width: min(100%, 1280px); border-radius: 10px; background: #000; border: 1px solid #333; }
    code { color: #8bd3ff; }
  </style>
</head>
<body>
  <h2>Sony Liveview Control</h2>
  <div class=\"toolbar\">
    <button class=\"primary\" onclick=\"startLiveview()\">Start Liveview</button>
    <button onclick=\"stopLiveview()\">Stop Liveview</button>
    <button class=\"warn\" onclick=\"startMovie()\">Start Recording</button>
    <button class=\"danger\" onclick=\"stopMovie()\">Stop Recording</button>
    <button onclick=\"refreshStatus()\">Refresh Status</button>
  </div>

  <div id=\"status\">Status: initializing...</div>
  <img id=\"stream\" alt=\"Live stream\" />

  <script>
    const statusEl = document.getElementById('status');
    const streamEl = document.getElementById('stream');
        let pollTimer = null;

    async function post(url) {
      const res = await fetch(url, { method: 'POST' });
      return await res.json();
    }

    function setStatus(text) {
      statusEl.textContent = 'Status: ' + text;
    }

    async function refreshStatus() {
            try {
                const res = await fetch('/api/status');
                if (!res.ok) {
                    setStatus('status endpoint not ready: HTTP ' + res.status);
                    return;
                }
                const s = await res.json();
                setStatus(`streaming=${s.streamingEnabled}, recording=${s.movieRecording}, frames=${s.frameCount}, liveviewUrl=${s.liveviewUrl || 'n/a'}`);
            } catch (e) {
                setStatus('cannot reach backend: ' + e);
            }
        }

        function startPollingFrames() {
            stopPollingFrames();
            pollTimer = setInterval(() => {
                streamEl.src = '/frame.jpg?t=' + Date.now();
            }, 150);
        }

        function stopPollingFrames() {
            if (pollTimer) {
                clearInterval(pollTimer);
                pollTimer = null;
            }
    }

    async function startLiveview() {
      const r = await post('/api/start_liveview');
      if (r.ok) {
                startPollingFrames();
        setStatus('liveview started');
      } else {
        setStatus('failed to start liveview: ' + JSON.stringify(r.result || r.error));
      }
      await refreshStatus();
    }

    async function stopLiveview() {
      await post('/api/stop_liveview');
            stopPollingFrames();
      streamEl.src = '';
      setStatus('liveview stopped');
      await refreshStatus();
    }

    async function startMovie() {
      const r = await post('/api/start_movie');
      setStatus(r.ok ? 'camera recording started' : ('start recording failed: ' + JSON.stringify(r.result || r.error)));
      await refreshStatus();
    }

    async function stopMovie() {
      const r = await post('/api/stop_movie');
      setStatus(r.ok ? 'camera recording stopped' : ('stop recording failed: ' + JSON.stringify(r.result || r.error)));
      await refreshStatus();
    }

    refreshStatus();
        setInterval(refreshStatus, 2000);
  </script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description="Watch Sony camera liveview in browser with recording controls")
    parser.add_argument("--address", default="192.168.122.1", help="Camera IP address")
    parser.add_argument("--camera-port", type=int, default=10000, help="Sony JSON API port")
    parser.add_argument("--listen", default="127.0.0.1", help="Web UI bind address")
    parser.add_argument("--port", type=int, default=8765, help="Web UI port")
    args = parser.parse_args()

    camera = SonyCameraClient(args.address, args.camera_port)
    state = AppState(camera)

    httpd = ThreadingHTTPServer((args.listen, args.port), LiveviewHandler)
    httpd.state = state

    print(f"Web UI: http://{args.listen}:{args.port}")
    print(f"Camera API: http://{args.address}:{args.camera_port}/sony/camera")
    print("Put camera in Ctrl w/ Smartphone mode, then open the Web UI.")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
