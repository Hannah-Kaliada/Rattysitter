import asyncio
import socket
import time
import struct
import math
import ssl
from collections import deque
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

from aiohttp import web, TCPConnector, ClientSession
import aiohttp

ESP32_IP = "172.20.10.4"
ESP32_UDP_PORT = 12346
PORT_HTTP = 8080
UDP_PORT_AUDIO = 12345
UDP_PORT_TEXT = 12347

BOT_TOKEN = "7644572719:AAEuGAp2bb0q-oBG8Tnc3KHx3gb9qTtOlcE"
CHAT_ID = "949226271"
VOLUME_THRESHOLD = 0.07
ALERT_INTERVAL = 30

LATITUDE = 53.9006
LONGITUDE = 27.5590  # Минск
TIMEZONE = ZoneInfo("Europe/Minsk")
YELLOW_COLOR_CMD = "R:255;G:200;B:0"
SUN_API_URL = "https://api.sunrise-sunset.org/json"

LIGHT_TIMEOUT = 900

HISTORY_DURATION = 3600

clients = set()
routes = web.RouteTableDef()
latest_sensor_data = ""
sensor_history = deque()
last_nonzero_time = None
light_off_task = None

def schedule_light_off():
    global light_off_task
    if light_off_task and not light_off_task.done():
        light_off_task.cancel()
    light_off_task = asyncio.create_task(light_off_timer())

async def light_off_timer():
    await asyncio.sleep(LIGHT_TIMEOUT)
    cmd = "R:0;G:0;B:0"
    print(f"[LightOff] Turning off light after timeout")
    send_udp_command(cmd)

def send_udp_command(cmd):
    try:
        print(f"[UDP->ESP32] Sending: {cmd}")
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(cmd.encode(), (ESP32_IP, ESP32_UDP_PORT))
        sock.close()
        print(f"[UDP->ESP32] Sent successfully")
    except Exception as e:
        print(f"[UDP->ESP32] Error sending command: {e}")

async def send_telegram_alert(volume):
    url = f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage"
    text = f"🔊 В комнате громко! Проверьте малыша."

    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    connector = TCPConnector(ssl=ssl_context)

    async with ClientSession(connector=connector) as session:
        resp = await session.post(url, json={
            "chat_id": CHAT_ID,
            "text": text
        })
        body = await resp.text()
        print(f"[Telegram] Status: {resp.status}, Body: {body}")

@routes.get('/')
async def index(request):
    print("[HTTP] Serving index.html")
    return web.FileResponse('index.html')
async def schedule_sunrise_yellow():

    async with ClientSession() as session:
        while True:
            params = {"lat": LATITUDE, "lng": LONGITUDE, "formatted": 0}
            try:
                resp = await session.get(SUN_API_URL, params=params)
                data = await resp.json()
                sunrise_utc = datetime.fromisoformat(data['results']['sunrise'])

                sunrise_local = sunrise_utc.astimezone(TIMEZONE)
                event_time = sunrise_local - timedelta(minutes=15)
                now = datetime.now(TIMEZONE)
                delay = (event_time - now).total_seconds()
                if delay < 0:

                    delay += 86400
                print(f"[Sunrise] Scheduled yellow at {event_time.isoformat()} local (in {delay:.0f}s)")
                await asyncio.sleep(delay)
                print("[Sunrise] Activating yellow nightlight 15m before sunrise")
                send_udp_command(YELLOW_COLOR_CMD)
                now2 = datetime.now(TIMEZONE)
                secs_to_mid = ((now2.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(days=1)) - now2).total_seconds()
                await asyncio.sleep(secs_to_mid)
            except Exception as e:
                print(f"[Sunrise] Error scheduling: {e}, retrying in 1h")
                await asyncio.sleep(3600)

@routes.get('/setColor')
async def set_color(request):
    global last_nonzero_time
    params = request.rel_url.query
    r = int(params.get("r", "0"))
    g = int(params.get("g", "0"))
    b = int(params.get("b", "0"))
    cmd = f"R:{r};G:{g};B:{b}"
    print(f"[HTTP] /setColor: r={r}, g={g}, b={b}")
    send_udp_command(cmd)
    if r or g or b:
        last_nonzero_time = time.time()
        schedule_light_off()
    return web.Response(text="OK")

@routes.get('/ws')
async def websocket_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    clients.add(ws)
    print(f"[WS] Client connected. Total clients: {len(clients)}")

    if latest_sensor_data:
        await ws.send_str(latest_sensor_data)

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                print(f"[WS] Received TEXT: {msg.data}")
            elif msg.type == web.WSMsgType.BINARY:
                print(f"[WS] Received BINARY: {len(msg.data)} bytes")
            elif msg.type == web.WSMsgType.ERROR:
                print(f"[WS] Connection closed with exception: {ws.exception()}")
    except Exception as e:
        print(f"[WS] Exception: {e}")
    finally:
        clients.discard(ws)
        print(f"[WS] Client disconnected. Total clients: {len(clients)}")

    return ws

@routes.get('/getSensorHistory')
async def get_sensor_history(request):
    now = time.time()
    recent_data = [
        {"timestamp": ts, "data": data}
        for ts, data in sensor_history
        if ts >= now - HISTORY_DURATION
    ]
    return web.json_response(recent_data)

class AudioUDPProtocol:
    def __init__(self):
        self.last_alert = 0

    def connection_made(self, transport):
        self.transport = transport
        print(f"[UDP-AUDIO] Listening on 0.0.0.0:{UDP_PORT_AUDIO}")

    def datagram_received(self, data, addr):
        for ws in list(clients):
            try:
                asyncio.create_task(ws.send_bytes(data))
            except Exception:
                clients.discard(ws)

        count = len(data) // 2
        if count == 0:
            return
        fmt = f"<{count}h"
        samples = struct.unpack(fmt, data)
        sum_squares = sum((s / 32768) ** 2 for s in samples)
        rms = math.sqrt(sum_squares / count)

        print(f"[UDP-AUDIO] RMS={rms:.3f}")

        now = time.time()
        if rms > VOLUME_THRESHOLD and now - self.last_alert > ALERT_INTERVAL:
            self.last_alert = now
            asyncio.create_task(send_telegram_alert(rms))

class TextUDPProtocol:
    def connection_made(self, transport):
        self.transport = transport
        print(f"[UDP-TEXT] Listening on 0.0.0.0:{UDP_PORT_TEXT}")

    def datagram_received(self, data, addr):
        global latest_sensor_data
        text = data.decode(errors='ignore').strip()
        latest_sensor_data = text

        timestamp = time.time()
        sensor_history.append((timestamp, text))
        while sensor_history and sensor_history[0][0] < timestamp - HISTORY_DURATION:
            sensor_history.popleft()

        print(f"[UDP-TEXT] Received: {latest_sensor_data}")
        for ws in list(clients):
            try:
                asyncio.create_task(ws.send_str(latest_sensor_data))
            except Exception:
                clients.discard(ws)

async def start_udp_servers():
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(
        lambda: AudioUDPProtocol(),
        local_addr=('0.0.0.0', UDP_PORT_AUDIO)
    )
    await loop.create_datagram_endpoint(
        lambda: TextUDPProtocol(),
        local_addr=('0.0.0.0', UDP_PORT_TEXT)
    )

async def main():
    app = web.Application()
    app.add_routes(routes)
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, '0.0.0.0', PORT_HTTP).start()
    print(f"[HTTP] Server running at http://localhost:{PORT_HTTP}")

    await start_udp_servers()

    while True:
        await asyncio.sleep(3600)

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Server stopped.")