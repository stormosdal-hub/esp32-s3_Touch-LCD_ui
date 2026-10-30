#!/usr/bin/env python3
"""
Mock of the device's web API, for working on the editor without hardware.

    python3 tools/mock_server.py [--port 8080]

then open http://localhost:8080. Serves main/www, keeps the layout in memory,
and simulates a few signals over the WebSocket at /ws. Standard library only.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WWW = os.path.join(ROOT, 'main', 'www')
with open(os.path.join(ROOT, 'main', 'default_layout.json'), encoding='utf-8') as f:
    DEFAULT_LAYOUT = f.read()

state = {'layout': DEFAULT_LAYOUT, 'backup': None, 'signals': {}, 'started': time.time()}
clients = []
lock = threading.Lock()


def ws_send(sock, text):
    data = text.encode()
    head = bytes([0x81])
    n = len(data)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += bytes([126]) + struct.pack('>H', n)
    else:
        head += bytes([127]) + struct.pack('>Q', n)
    sock.sendall(head + data)


def ws_recv(sock):
    def exact(n):
        buf = b''
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf
    b0, b1 = exact(2)
    opcode, n = b0 & 0x0F, b1 & 0x7F
    if n == 126:
        n = struct.unpack('>H', exact(2))[0]
    elif n == 127:
        n = struct.unpack('>Q', exact(8))[0]
    mask = exact(4) if b1 & 0x80 else b'\0\0\0\0'
    payload = bytes(c ^ mask[i % 4] for i, c in enumerate(exact(n)))
    return opcode, payload


def broadcast(obj):
    text = json.dumps(obj)
    with lock:
        for c in list(clients):
            try:
                ws_send(c, text)
            except OSError:
                clients.remove(c)


def set_signals(values):
    state['signals'].update(values)
    broadcast({'signals': values})


def simulator():
    while True:
        t = time.time()
        set_signals({
            'sys.time': time.strftime('%H:%M:%S'),
            'sys.date': time.strftime('%Y-%m-%d'),
            'sys.uptime': int(t - state['started']),
            'sys.rssi': -55 + int(5 * math.sin(t / 7)),
            'sys.heap': 180,
            'sys.wifi': 'MockNet  http://localhost  (127.0.0.1)',
            'demo.sine': round(50 + 40 * math.sin(t / 5), 2),
        })
        time.sleep(1)


def bmp(width=800, height=480):
    """Gray placeholder screenshot."""
    row = b'\x40\x42' * width
    header = b'BM' + struct.pack('<IHHI', 66 + len(row) * height, 0, 0, 66)
    dib = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 16, 3, len(row) * height, 0, 0, 0, 0)
    masks = struct.pack('<III', 0xF800, 0x07E0, 0x001F)
    return header + dib + masks + row * height


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype='application/json'):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n else b''

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/ws':
            return self.websocket()
        if path == '/api/layout':
            return self.send(200, state['layout'])
        if path == '/api/layout/default':
            return self.send(200, DEFAULT_LAYOUT)
        if path == '/api/signals':
            return self.send(200, state['signals'])
        if path == '/api/info':
            return self.send(200, {
                'project': 'lcd_ui_designer', 'version': 'mock', 'idf': 'mock', 'built': time.ctime(),
                'uptime_s': int(time.time() - state['started']), 'heap_internal': 180000, 'heap_psram': 6500000,
                'width': 800, 'height': 480, 'user_gpios': [6, 15, 16, 19, 20, 43, 44], 'partition': 'ota_0',
                'wifi': {'ssid': 'MockNet', 'connected': True, 'ip': '127.0.0.1', 'hostname': 'lcd-ui', 'ap_active': False, 'ap_ssid': 'LCD-UI-0000'},
                'storage': {'total': 1966080, 'used': 16384}, 'inputs_active': 0,
                'ports': {'rs485': {'running': True, 'config': '9600 8N1 raw', 'rx_lines': 0, 'tx_bytes': 0, 'modbus_ok': 0, 'modbus_errors': 0},
                          'can': {'running': False, 'bitrate': 0, 'rx': 0, 'tx': 0, 'tx_errors': 0},
                          'mqtt': {'running': False, 'connected': False, 'rx': 0, 'tx': 0}}})
        if path == '/api/wifi/scan':
            return self.send(200, [{'ssid': 'MockNet', 'rssi': -48, 'secure': True}, {'ssid': 'Neighbour', 'rssi': -80, 'secure': True}])
        if path == '/api/screenshot.bmp':
            return self.send(200, bmp(), 'image/bmp')
        if path == '/':
            path = '/index.html'
        file = os.path.normpath(os.path.join(WWW, path.lstrip('/')))
        if file.startswith(WWW) and os.path.isfile(file):
            ctype = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'}.get(os.path.splitext(file)[1], 'application/octet-stream')
            with open(file, 'rb') as f:
                return self.send(200, f.read(), ctype)
        self.send(404, {'ok': False, 'error': 'not found'})

    def do_POST(self):
        path = self.path.split('?')[0]
        data = self.body()
        if path == '/api/layout':
            try:
                layout = json.loads(data)
                if not isinstance(layout.get('tabs'), list):
                    raise ValueError('layout needs a "tabs" array')
            except ValueError as e:
                return self.send(400, {'ok': False, 'error': str(e)})
            state['backup'], state['layout'] = state['layout'], data.decode()
            print(f'deployed layout: {len(data)} bytes, {len(layout["tabs"])} tabs')
            return self.send(200, {'ok': True})
        if path == '/api/layout/rollback':
            if not state['backup']:
                return self.send(400, {'ok': False, 'error': 'no previous layout saved'})
            state['layout'], state['backup'] = state['backup'], state['layout']
            return self.send(200, {'ok': True})
        if path == '/api/signals':
            set_signals(json.loads(data or b'{}'))
            return self.send(200, {'ok': True})
        if path in ('/api/wifi', '/api/reboot', '/api/ota'):
            return self.send(200, {'ok': True})
        self.send(404, {'ok': False, 'error': 'not found'})

    def websocket(self):
        key = self.headers.get('Sec-WebSocket-Key', '')
        accept = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        sock = self.connection
        with lock:
            clients.append(sock)
            ws_send(sock, json.dumps({'signals': state['signals']}))
        try:
            while True:
                opcode, payload = ws_recv(sock)
                if opcode == 8:
                    break
                if opcode != 1:
                    continue
                text = payload.decode(errors='replace')
                print('ws <-', text)
                try:
                    msg = json.loads(text)
                except ValueError:
                    msg = None
                if isinstance(msg, dict) and isinstance(msg.get('set'), dict):
                    set_signals(msg['set'])
                elif isinstance(msg, dict) and 'get' in msg:
                    ws_send(sock, json.dumps({'signals': state['signals']}))
                elif msg is None and '=' in text:
                    k, v = text.split('=', 1)
                    try:
                        v = float(v)
                    except ValueError:
                        pass
                    set_signals({k.strip(): v})
                else:
                    set_signals({'ws.rx': text})
        except (ConnectionError, OSError):
            pass
        finally:
            with lock:
                if sock in clients:
                    clients.remove(sock)
        self.close_connection = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8080)
    args = ap.parse_args()
    threading.Thread(target=simulator, daemon=True).start()
    srv = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    srv.daemon_threads = True
    print(f'Mock device at http://localhost:{args.port}')
    srv.serve_forever()


if __name__ == '__main__':
    main()
