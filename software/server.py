import asyncio
import struct
import logging
import json
import time
import io
import wave
import numpy as np
import joblib
import librosa
import aiohttp
from aiohttp import web
from collections import deque

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

TELEGRAM_BOT_TOKEN = "7644572719:AAHr0MIQVg7U5_2dibiUGIIfrVqIulMdI9c"
TELEGRAM_CHAT_ID = "949226271"
TELEGRAM_POLL_INTERVAL = 2
TELEGRAM_API_URL = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}"

MODEL_PATH = '/Users/hannakaliada/Desktop/БГУИР/cry_detector.pkl'
try:
    cry_detector = joblib.load(MODEL_PATH)
    logger.info(f"Модель детектора плача загружена из {MODEL_PATH}")
except FileNotFoundError:
    logger.error(f"Модель не найдена по пути: {MODEL_PATH}")
    cry_detector = None

SR = 16000
FRAME_SIZE = 0.025
HOP_SIZE = 0.010
MIN_SEGMENT_DURATION = 0.1
ENERGY_THRESHOLD_COEFF = 0.6
MIN_PEAK_TO_RMS_RATIO = 2.5

BANDS = [
    (200, 600, "Low (200-600 Hz)"),
    (600, 1500, "Mid (600-1500 Hz)"),
    (1500, 3000, "High (1500-3000 Hz)")
]

FEATURE_COLS = [
    'duration', 'prev_pause', 'next_pause', 'peak_amplitude', 'mean_amplitude',
    'rise_time', 'fall_time', 'mean_f0',
    'spec_Low (200-600 Hz)', 'spec_Mid (600-1500 Hz)', 'spec_High (1500-3000 Hz)',
    'mfcc_1', 'mfcc_2', 'mfcc_3', 'mfcc_4', 'mfcc_5',
    'mfcc_6', 'mfcc_7', 'mfcc_8', 'mfcc_9', 'mfcc_10', 'mfcc_11', 'mfcc_12', 'mfcc_13'
]

TEMP_MIN, TEMP_MAX = 18.0, 26.0
HUM_MIN, HUM_MAX = 40.0, 60.0
PRES_MIN, PRES_MAX = 738.0, 760.0

esp_clients = {}
browser_clients = set()
stream_clients = set()
last_image = None
last_sensor_data = {"temp": 0.0, "hum": 0.0, "pres": 0.0}
last_sensor_time = 0
audio_buffer = deque(maxlen=SR * 10)
last_cry_time = 0
CRY_ALERT_COOLDOWN = 30
last_telegram_update_id = 0

recording_audio = False
recording_samples = []
RECORDING_DURATION = 5
recording_start_time = 0

def extract_energy_segments(audio, sr):
    frame_len = int(FRAME_SIZE * sr)
    hop_len = int(HOP_SIZE * sr)
    frames = librosa.util.frame(audio, frame_length=frame_len, hop_length=hop_len)
    energy = np.sum(frames ** 2, axis=0)
    times = librosa.frames_to_time(np.arange(len(energy)), sr=sr,
                                   hop_length=hop_len, n_fft=frame_len)
    threshold = np.mean(energy) * ENERGY_THRESHOLD_COEFF
    voiced_mask = energy > threshold
    segments = []
    in_segment = False
    start_time = 0.0
    for i, val in enumerate(voiced_mask):
        if val and not in_segment:
            start_time = times[i]
            in_segment = True
        elif not val and in_segment:
            end_time = times[i - 1]
            if (end_time - start_time) >= MIN_SEGMENT_DURATION:
                segments.append((start_time, end_time))
            in_segment = False
    if in_segment:
        end_time = times[-1]
        if (end_time - start_time) >= MIN_SEGMENT_DURATION:
            segments.append((start_time, end_time))
    return segments

def compute_pitch_autocorrelation(segment_audio, sr, fmin=200, fmax=600):
    frame_len = int(0.050 * sr)
    hop_len = int(0.025 * sr)
    frames = librosa.util.frame(segment_audio, frame_length=frame_len, hop_length=hop_len)
    frames = frames * np.hamming(frame_len)[:, np.newaxis]
    pitches = []
    min_lag = int(sr / fmax)
    max_lag = int(sr / fmin)
    for i in range(frames.shape[1]):
        x = frames[:, i]
        autocorr = np.correlate(x, x, mode='full')
        autocorr = autocorr[len(x) - 1:]
        norm = autocorr[0]
        if norm < 1e-10: continue
        autocorr_norm = autocorr / norm
        s = autocorr_norm[min_lag:max_lag+1] if len(autocorr_norm) > max_lag else autocorr_norm[min_lag:]
        if len(s) == 0: continue
        pi, pv = np.argmax(s), s[np.argmax(s)]
        if pv > 0.35: pitches.append(sr / (min_lag + pi))
    return np.mean(pitches) if pitches else np.nan

def compute_spectral_band_energies(segment_audio, sr):
    n_fft = 1024
    D = np.abs(librosa.stft(segment_audio, n_fft=n_fft)) ** 2
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    total_energy = np.sum(D)
    energies = {}
    for low, high, name in BANDS:
        idx = np.where((freqs >= low) & (freqs <= high))[0]
        energies[name] = np.sum(D[idx, :]) / (total_energy + 1e-12)
    return energies

def compute_rise_fall_times(segment_audio, sr):
    abs_audio = np.abs(segment_audio)
    win = max(1, int(0.010 * sr))
    envelope = np.convolve(abs_audio, np.ones(win)/win, mode='same')
    if np.max(envelope) < 1e-10: return 0.0, 0.0
    env_norm = envelope / np.max(envelope)
    a10, a90 = np.where(env_norm >= 0.1)[0], np.where(env_norm >= 0.9)[0]
    rt = (a90[0] - a10[0]) / sr if len(a10) and len(a90) else 0.0
    ren = env_norm[::-1]
    a10r, a90r = np.where(ren >= 0.1)[0], np.where(ren >= 0.9)[0]
    ft = abs(a90r[0] - a10r[0]) / sr if len(a10r) and len(a90r) else 0.0
    return rt, ft

def extract_features(seg, sr, pp, np_):
    d = len(seg) / sr
    pa, ma = np.max(np.abs(seg)), np.mean(np.abs(seg))
    rt, ft = compute_rise_fall_times(seg, sr)
    f0 = compute_pitch_autocorrelation(seg, sr)
    se = compute_spectral_band_energies(seg, sr)
    mf = np.mean(librosa.feature.mfcc(y=seg, sr=sr, n_mfcc=13, n_fft=int(0.050*sr), hop_length=int(0.025*sr)), axis=1)
    fe = {'duration': d, 'prev_pause': pp, 'next_pause': np_,
          'peak_amplitude': pa, 'mean_amplitude': ma,
          'rise_time': rt, 'fall_time': ft,
          'mean_f0': f0 if not np.isnan(f0) else 0.0}
    fe.update({f'spec_{k}': v for k, v in se.items()})
    fe.update({f'mfcc_{i+1}': mf[i] for i in range(13)})
    return fe

async def telegram_api_call(method: str, params: dict = None, files: dict = None):
    url = f"{TELEGRAM_API_URL}/{method}"
    try:
        async with aiohttp.ClientSession() as session:
            if files:
                form = aiohttp.FormData()
                for k, v in (params or {}).items(): form.add_field(k, str(v))
                for fn, fd in files.items(): form.add_field(fn, fd['data'], filename=fd.get('fn','file'), content_type=fd.get('ct','application/octet-stream'))
                async with session.post(url, data=form) as r: return await r.json()
            async with session.post(url, json=params) as r: return await r.json()
    except Exception as e: logger.error(f"Ошибка Telegram API: {e}"); return None

async def send_telegram_message(text: str):
    await telegram_api_call("sendMessage", {"chat_id": TELEGRAM_CHAT_ID, "text": text, "parse_mode": "HTML"})

async def send_telegram_photo(image_bytes: bytes, caption: str = ""):
    await telegram_api_call("sendPhoto", {"chat_id": TELEGRAM_CHAT_ID, "caption": caption},
                           {"photo": {"data": image_bytes, "fn": "photo.jpg", "ct": "image/jpeg"}})

async def send_telegram_voice(audio_data: np.ndarray, caption: str = ""):
    audio_int16 = (audio_data * 32767).astype(np.int16).tobytes()
    wav_buf = io.BytesIO()
    with wave.open(wav_buf, 'wb') as wf:
        wf.setnchannels(1); wf.setsampwidth(2); wf.setframerate(SR); wf.writeframes(audio_int16)
    wav_buf.seek(0)
    await telegram_api_call("sendVoice", {"chat_id": TELEGRAM_CHAT_ID, "caption": caption},
                           {"voice": {"data": wav_buf.read(), "fn": "cry.ogg", "ct": "audio/ogg"}})

async def check_telegram_updates():
    global last_telegram_update_id
    r = await telegram_api_call("getUpdates", {"timeout": 5, "offset": last_telegram_update_id + 1})
    if not r or not r.get("ok"): return
    for u in r.get("result", []):
        if u.get("update_id", 0) > last_telegram_update_id: last_telegram_update_id = u["update_id"]
        msg = u.get("message")
        if msg and msg.get("text"): await handle_telegram_command(str(msg["chat"]["id"]), msg["text"].strip())

async def handle_telegram_command(chat_id: str, text: str):
    t = text.lower()
    if t in ["/start", "/help"]:
        await send_telegram_message("Команды:\n/photo - фото\n/stream_on - включить стрим\n/stream_off - выключить стрим\n/audio_on - включить аудио\n/audio_off - выключить аудио\n/sensors - датчики\n/status - статус\n/light_on - свет\n/reboot - перезагрузка")
    elif t == "/photo": await send_to_esp("photo"); await send_telegram_message("Фото...")
    elif t == "/stream_on": await send_to_esp("stream_on"); await send_telegram_message("Стрим включен")
    elif t == "/stream_off": await send_to_esp("stream_off"); await send_telegram_message("Стрим выключен")
    elif t == "/audio_on": await send_to_esp("audio_on"); await send_telegram_message("Аудио включено")
    elif t == "/audio_off": await send_to_esp("audio_off"); await send_telegram_message("Аудио выключено")
    elif t == "/sensors":
        await send_telegram_message(f"Температура: {last_sensor_data['temp']:.1f} C\nВлажность: {last_sensor_data['hum']:.1f}%\nДавление: {last_sensor_data['pres']:.1f} мм")
    elif t == "/status":
        await send_telegram_message(f"ESP клиентов: {len(esp_clients)}\nВеб клиентов: {len(browser_clients)}")
    elif t == "/reboot": await send_to_esp("reboot")
    elif t.startswith("/light"): await send_to_esp(t[1:])

async def check_sensor_thresholds(temp, hum, pres):
    alerts = []
    if temp < TEMP_MIN: alerts.append(f"Низкая температура: {temp:.1f} C")
    elif temp > TEMP_MAX: alerts.append(f"Высокая температура: {temp:.1f} C")
    if hum < HUM_MIN: alerts.append(f"Низкая влажность: {hum:.1f}%")
    elif hum > HUM_MAX: alerts.append(f"Высокая влажность: {hum:.1f}%")
    if pres < PRES_MIN: alerts.append(f"Низкое давление: {pres:.1f}")
    elif pres > PRES_MAX: alerts.append(f"Высокое давление: {pres:.1f}")
    if alerts: await send_telegram_message("Предупреждение:\n"+"\n".join(alerts)); await send_to_esp("photo")

async def analyze_audio_for_cry(audio_data):
    global last_cry_time, last_image, recording_audio, recording_samples, recording_start_time

    if cry_detector is None or len(audio_data) < SR * 0.5:
        return False

    audio = np.array(audio_data, dtype=np.float32)
    audio = audio / (np.max(np.abs(audio)) + 1e-10)
    segments = extract_energy_segments(audio, SR)
    cry_segments, total_segments, max_cry_prob = 0, 0, 0.0

    for start, end in segments:
        seg_audio = audio[int(start*SR):int(end*SR)]
        rms = np.sqrt(np.mean(seg_audio**2))
        peak = np.max(np.abs(seg_audio))
        if peak / (rms + 1e-10) < MIN_PEAK_TO_RMS_RATIO or (end - start) < 0.2: continue
        total_segments += 1
        features = extract_features(seg_audio, SR, 0.5, 0.5)
        X = np.array([[features.get(col, 0.0) for col in FEATURE_COLS]])
        proba = cry_detector.predict_proba(X)
        if proba[0][1] > 0.5:
            cry_segments += 1
            max_cry_prob = max(max_cry_prob, proba[0][1])

    current_time = time.time()

    if recording_audio and (current_time - recording_start_time >= RECORDING_DURATION):
        recording_audio = False
        if recording_samples:
            recorded = np.array(recording_samples, dtype=np.float32)
            logger.info(f"Запись завершена: {len(recorded)/SR:.1f} сек, отправка в Telegram")
            await send_telegram_voice(recorded, caption=f"Аудио плача ({len(recorded)/SR:.1f} сек)")
            recording_samples = []

    if total_segments > 0 and cry_segments > 0 and cry_segments / total_segments > 0.3:
        if current_time - last_cry_time > CRY_ALERT_COOLDOWN:
            last_cry_time = current_time
            logger.warning(f"Обнаружен детский плач! ({cry_segments}/{total_segments})")

            recording_audio = True
            recording_samples = []
            recording_start_time = current_time
            logger.info(f"Начата запись {RECORDING_DURATION} секунд аудио после обнаружения плача")

            await send_telegram_message(
                f"Обнаружен детский плач!\n\n"
                f"Вероятность: {max_cry_prob:.1%}\n"
                f"Сегментов плача: {cry_segments} из {total_segments}\n"
                f"Записываю {RECORDING_DURATION} сек аудио..."
            )
            if last_image:
                await send_telegram_photo(last_image, caption=f"Фото при обнаружении плача\nВероятность: {max_cry_prob:.1%}")
            await notify_browsers({"type": "cry_alert", "probability": max_cry_prob})
            return True
    return False

async def send_to_esp(cmd: str):
    dead = []
    for esp_id, ws in esp_clients.items():
        try: await ws.send_str(cmd)
        except: dead.append(esp_id)
    for d in dead: del esp_clients[d]

async def notify_browsers(data: dict):
    dead = []
    for ws in browser_clients:
        try: await ws.send_str(json.dumps(data))
        except: dead.append(ws)
    for d in dead: browser_clients.discard(d)

async def telegram_polling_task():
    while True:
        await asyncio.sleep(TELEGRAM_POLL_INTERVAL)
        try: await check_telegram_updates()
        except: pass

async def heartbeat_task():
    while True:
        await asyncio.sleep(5)
        dead = []
        for esp_id, ws in esp_clients.items():
            try: await ws.send_str("heartbeat")
            except: dead.append(esp_id)
        for d in dead: del esp_clients[d]

HTML_PAGE = """
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Baby Monitor - Наблюдение за ребенком</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
:root {
    --bg-primary: #0a0a0f; --bg-secondary: #141420; --bg-card: #1a1a2e;
    --text-primary: #e8e8f0; --text-secondary: #9898b0;
    --accent: #6c5ce7; --danger: #e74c3c; --success: #2ecc71;
    --border: #2a2a40; --radius: 16px; --radius-sm: 10px;
}
body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    background: var(--bg-primary); color: var(--text-primary);
    min-height: 100vh; overflow-x: hidden;
}
.header {
    background: var(--bg-secondary); border-bottom: 1px solid var(--border);
    padding: 12px 20px; display: flex; align-items: center;
    justify-content: space-between; position: sticky; top: 0; z-index: 100;
}
.logo {
    width: 36px; height: 36px;
    background: linear-gradient(135deg, var(--accent), #a855f7);
    border-radius: 10px; display: flex; align-items: center;
    justify-content: center; font-size: 20px;
}
.header-title { font-size: 18px; font-weight: 600; }
.header-subtitle { font-size: 12px; color: var(--text-secondary); }
.status-dot {
    width: 10px; height: 10px; border-radius: 50%;
    background: var(--success); display: inline-block;
    animation: pulse-dot 2s infinite;
}
@keyframes pulse-dot {
    0%,100% { opacity: 1; box-shadow: 0 0 0 0 rgba(46,204,113,0.6); }
    50% { opacity: 0.8; box-shadow: 0 0 0 8px rgba(46,204,113,0); }
}
.video-container { position: relative; background: #000; border-bottom: 1px solid var(--border); }
.video-container img { width: 100%; display: block; min-height: 300px; object-fit: contain; }
.alert-overlay {
    position: fixed; top: 20px; left: 50%;
    transform: translateX(-50%) translateY(-120px);
    background: linear-gradient(135deg, #e74c3c, #c0392b);
    color: white; padding: 16px 28px; border-radius: var(--radius);
    font-size: 16px; font-weight: 600; z-index: 1000;
    display: flex; align-items: center; gap: 10px;
    box-shadow: 0 12px 40px rgba(231,76,60,0.5);
    transition: transform 0.3s;
}
.alert-overlay.show { transform: translateX(-50%) translateY(0); }
.alert-icon { font-size: 28px; animation: shake 0.5s infinite; }
@keyframes shake {
    0%,100% { transform: rotate(0); }
    25% { transform: rotate(-10deg); }
    75% { transform: rotate(10deg); }
}
.control-panel {
    background: var(--bg-secondary); border-top: 1px solid var(--border);
    padding: 16px 20px; display: flex; flex-wrap: wrap; gap: 10px;
    justify-content: center; position: sticky; bottom: 0; z-index: 100;
}
.btn {
    display: flex; align-items: center; gap: 6px;
    padding: 10px 16px; border-radius: var(--radius-sm);
    border: 1px solid var(--border); background: var(--bg-card);
    color: var(--text-primary); font-size: 13px; font-weight: 500;
    cursor: pointer; transition: all 0.3s; font-family: inherit;
}
.btn:hover { background: var(--border); transform: translateY(-1px); }
.btn-primary { background: var(--accent); border-color: var(--accent); }
.btn-danger { background: var(--danger); border-color: var(--danger); }
.stats-bar {
    display: flex; gap: 12px; padding: 12px 20px;
    background: var(--bg-secondary); overflow-x: auto;
    border-bottom: 1px solid var(--border);
}
.stat-card {
    background: var(--bg-card); border-radius: var(--radius-sm);
    padding: 10px 16px; flex: 1; min-width: 90px;
    text-align: center; border: 1px solid var(--border);
}
.stat-value {
    font-size: 20px; font-weight: 700;
    background: linear-gradient(135deg, var(--accent), #a855f7);
    -webkit-background-clip: text; -webkit-text-fill-color: transparent;
}
.stat-label { font-size: 11px; color: var(--text-secondary); margin-top: 2px; }
@media (max-width: 768px) {
    .header { padding: 10px 14px; }
    .btn { padding: 8px 12px; font-size: 11px; }
    .stat-card { padding: 8px 10px; }
    .stat-value { font-size: 16px; }
}
</style>
</head>
<body>

<div class="header">
    <div style="display:flex;align-items:center;gap:12px">
        <div class="logo">👶</div>
        <div>
            <div class="header-title">Наблюдение за ребенком</div>
            <div class="header-subtitle"><span class="status-dot"></span> Система активна</div>
        </div>
    </div>
    <div style="text-align:right">
        <div style="font-size:24px" id="clock">--:--</div>
        <div style="font-size:11px;color:var(--text-secondary)" id="date">--</div>
    </div>
</div>

<div class="alert-overlay" id="alertOverlay">
    <span class="alert-icon">⚠️</span>
    <span>Обнаружен детский плач!</span>
</div>

<div class="video-container">
    <img src="/stream" id="videoFeed" alt="Видео">
</div>

<div class="stats-bar">
    <div class="stat-card">
        <div class="stat-value" id="statTemp">--°</div>
        <div class="stat-label">Температура</div>
    </div>
    <div class="stat-card">
        <div class="stat-value" id="statHum">--%</div>
        <div class="stat-label">Влажность</div>
    </div>
    <div class="stat-card">
        <div class="stat-value" id="statPres">--</div>
        <div class="stat-label">Давление</div>
    </div>
    <div class="stat-card">
        <div class="stat-value" id="statCry">Хорошо</div>
        <div class="stat-label">Статус</div>
    </div>
</div>

<div class="control-panel">
    <button class="btn btn-primary" onclick="sendCmd('photo')">📸 Фото</button>
    <button class="btn" onclick="sendCmd('stream_on')">▶️ Стрим</button>
    <button class="btn" onclick="sendCmd('stream_off')">⏹️ Стоп</button>
    <button class="btn" onclick="sendCmd('audio_on')">🎤 Аудио</button>
    <button class="btn" onclick="sendCmd('audio_off')">🔇 Выкл</button>
    <button class="btn btn-danger" onclick="sendCmd('light_on')">💡 Свет</button>
</div>

<script>
function sendCmd(cmd) {
    fetch('/cmd?c=' + encodeURIComponent(cmd)).catch(function(e) {});
}

function updateClock() {
    var now = new Date();
    document.getElementById('clock').textContent = 
        now.toLocaleTimeString('ru-RU', {hour:'2-digit', minute:'2-digit'});
    document.getElementById('date').textContent = 
        now.toLocaleDateString('ru-RU', {day:'numeric', month:'long', weekday:'short'});
}
updateClock();
setInterval(updateClock, 10000);

function updateSensors(msg) {
    if (msg.temp) document.getElementById('statTemp').textContent = msg.temp.toFixed(1) + '°';
    if (msg.hum) document.getElementById('statHum').textContent = msg.hum.toFixed(1) + '%';
    if (msg.pres) document.getElementById('statPres').textContent = msg.pres.toFixed(1);
}

function showCryAlert() {
    var overlay = document.getElementById('alertOverlay');
    overlay.classList.add('show');
    document.getElementById('statCry').textContent = 'ПЛАЧ!';
    document.getElementById('statCry').style.color = '#e74c3c';
    document.getElementById('statCry').style.webkitTextFillColor = '#e74c3c';
    setTimeout(function() {
        overlay.classList.remove('show');
        document.getElementById('statCry').textContent = 'Хорошо';
        document.getElementById('statCry').style.color = '';
        document.getElementById('statCry').style.webkitTextFillColor = '';
    }, 5000);
}

var ws;
var audioCtx = null;
var audioTime = 0;

function ensureAudioContext() {
    if (!audioCtx) {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (audioCtx.state !== 'running') {
        audioCtx.resume();
    }
}

document.body.addEventListener('click', function() {
    ensureAudioContext();
});

function connectWebSocket() {
    var protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    ws = new WebSocket(protocol + '//' + location.host + '/browser');

    ws.onmessage = async function(e) {
        if (typeof e.data === 'string') {
            try {
                var msg = JSON.parse(e.data);
                if (msg.type === 'cry_alert') {
                    showCryAlert();
                } else if (msg.type === 'sensor_data') {
                    updateSensors(msg);
                }
            } catch(err) {}
        } else {
            try {
                ensureAudioContext();
                var buf = await e.data.arrayBuffer();
                var samples = new Int16Array(buf);
                var f32 = new Float32Array(samples.length);
                for (var i = 0; i < samples.length; i++) {
                    var v = samples[i] / 32768;
                    v = Math.max(-1, Math.min(1, v * 3.0));
                    f32[i] = v;
                }
                var audioBuffer = audioCtx.createBuffer(1, f32.length, 16000);
                audioBuffer.copyToChannel(f32, 0);
                var src = audioCtx.createBufferSource();
                src.buffer = audioBuffer;
                src.connect(audioCtx.destination);
                if (audioTime < audioCtx.currentTime) {
                    audioTime = audioCtx.currentTime;
                }
                src.start(audioTime);
                audioTime += audioBuffer.duration;
            } catch(err) {
                console.error('Audio error:', err);
            }
        }
    };

    ws.onclose = function() {
        setTimeout(connectWebSocket, 2000);
    };
}

connectWebSocket();
</script>
</body>
</html>
"""

async def index(request):
    return web.Response(text=HTML_PAGE, content_type="text/html")

async def cmd(request):
    c = request.query.get("c")
    if c: await send_to_esp(c)
    return web.Response(text="ok")

async def stream(request):
    resp = web.StreamResponse(status=200, headers={
        "Content-Type": "multipart/x-mixed-replace; boundary=frame",
        "Cache-Control": "no-cache", "Pragma": "no-cache"
    })
    await resp.prepare(request)
    stream_clients.add(resp)
    if last_image:
        try: await resp.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + last_image + b"\r\n")
        except: pass
    try:
        while True: await asyncio.sleep(3600)
    finally: stream_clients.discard(resp)

async def browser_ws(request):
    ws = web.WebSocketResponse(heartbeat=60)
    await ws.prepare(request)
    browser_clients.add(ws)
    try: await asyncio.sleep(3600)
    finally: browser_clients.discard(ws)
    return ws

async def esp_ws(request):
    global last_image, last_sensor_data, last_sensor_time, recording_samples
    ws = web.WebSocketResponse(heartbeat=60, max_msg_size=0, protocols=("arduino",))
    await ws.prepare(request)
    esp_id = str(int(time.time() * 1000))
    esp_clients[esp_id] = ws
    logger.info(f"ESP32 подключен (ID: {esp_id})")
    await send_telegram_message("ESP32 подключена к серверу")
    buffer = bytearray()

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.BINARY:
                buffer.extend(msg.data)
                while len(buffer) >= 4:
                    pkt_type, pkt_len = struct.unpack_from("<HH", buffer, 0)
                    if pkt_len <= 0 or pkt_len > 200_000:
                        buffer.pop(0)
                        continue
                    if len(buffer) < 4 + pkt_len:
                        break
                    payload = bytes(buffer[4:4 + pkt_len])
                    del buffer[:4 + pkt_len]

                    if pkt_type == 1:
                        last_image = payload
                        await push_frame(payload)

                    elif pkt_type == 2:
                        dead = []
                        for b in browser_clients:
                            try: await b.send_bytes(payload)
                            except: dead.append(b)
                        for d in dead: browser_clients.discard(d)

                        audio_samples = np.frombuffer(payload, dtype=np.int16).astype(np.float32) / 32768.0
                        audio_buffer.extend(audio_samples)

                        if recording_audio:
                            recording_samples.extend(audio_samples.tolist())

                        await analyze_audio_for_cry(list(audio_buffer))

            elif msg.type == web.WSMsgType.TEXT:
                text = msg.data
                logger.info(f"Текст ESP: {text}")

                if text == "loud_sound":
                    logger.info("Громкий звук обнаружен ESP32")
                    await send_telegram_message("Обнаружен громкий звук!")
                    if last_image:
                        await send_telegram_photo(last_image, "Фото при громком звуке")

                elif text.startswith("temp:"):
                    try:
                        parts = text.split()
                        temp = float(parts[0].split(":")[1])
                        hum = float(parts[1].split(":")[1])
                        pres = float(parts[2].split(":")[1])
                        last_sensor_data = {"temp": temp, "hum": hum, "pres": pres}
                        last_sensor_time = time.time()
                        await notify_browsers({
                            "type": "sensor_data",
                            "temp": temp, "hum": hum, "pres": pres
                        })
                        await check_sensor_thresholds(temp, hum, pres)
                    except Exception as e:
                        logger.error(f"Ошибка парсинга данных датчиков: {e}")

            elif msg.type == web.WSMsgType.ERROR:
                logger.error(f"Ошибка WebSocket: {ws.exception()}")

    except Exception as e:
        logger.error(f"Ошибка обработчика ESP: {e}")
    finally:
        if esp_id in esp_clients:
            del esp_clients[esp_id]
        logger.info(f"ESP32 отключена (ID: {esp_id})")
        await send_telegram_message("ESP32 отключена от сервера")
    return ws

async def push_frame(frame: bytes):
    dead = []
    for resp in stream_clients:
        try:
            await resp.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + frame + b"\r\n")
        except Exception:
            dead.append(resp)
    for d in dead:
        stream_clients.discard(d)

app = web.Application()
app.router.add_get("/", index)
app.router.add_get("/cmd", cmd)
app.router.add_get("/stream", stream)
app.router.add_get("/browser", browser_ws)
app.router.add_get("/ws", esp_ws)

async def start_background_tasks(app):
    asyncio.create_task(telegram_polling_task())
    asyncio.create_task(heartbeat_task())

app.on_startup.append(start_background_tasks)

if __name__ == "__main__":
    logger.info("Запуск сервера на порту 8080...")
    web.run_app(app, host="0.0.0.0", port=8080)
