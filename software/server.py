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

# ============================================================
# Настройка логирования
# ============================================================
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# ============================================================
# Конфигурация Telegram
# ============================================================
TELEGRAM_BOT_TOKEN = "7644572719:AAHr0MIQVg7U5_2dibiUGIIfrVqIulMdI9c"
TELEGRAM_CHAT_ID = "949226271"
TELEGRAM_POLL_INTERVAL = 2
TELEGRAM_API_URL = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}"

# ============================================================
# Загрузка модели детектора плача
# ============================================================
MODEL_PATH = '/Users/hannakaliada/Desktop/БГУИР/cry_detector.pkl'
try:
    cry_detector = joblib.load(MODEL_PATH)
    logger.info(f"Модель детектора плача загружена из {MODEL_PATH}")
except FileNotFoundError:
    logger.error(f"Модель не найдена по пути: {MODEL_PATH}")
    cry_detector = None

# ============================================================
# Параметры аудиоанализа
# ============================================================
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

# Пороги для датчиков
TEMP_MIN, TEMP_MAX = 18.0, 26.0
HUM_MIN, HUM_MAX = 40.0, 60.0
PRES_MIN, PRES_MAX = 738.0, 760.0

# ============================================================
# Глобальное состояние
# ============================================================
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
cry_audio_buffer = bytearray()  # Буфер для накопления аудио плача

# ============================================================
# Функции аудиоанализа (извлечение признаков)
# ============================================================
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
        if norm < 1e-10:
            continue
        autocorr_norm = autocorr / norm
        if len(autocorr_norm) <= max_lag:
            search = autocorr_norm[min_lag:]
        else:
            search = autocorr_norm[min_lag:max_lag + 1]
        if len(search) == 0:
            continue
        peak_idx = np.argmax(search)
        peak_val = search[peak_idx]
        if peak_val > 0.35:
            lag = min_lag + peak_idx
            f0 = sr / lag
            pitches.append(f0)
    return np.mean(pitches) if pitches else np.nan


def compute_spectral_band_energies(segment_audio, sr):
    n_fft = 1024
    D = np.abs(librosa.stft(segment_audio, n_fft=n_fft)) ** 2
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    total_energy = np.sum(D)
    energies = {}
    for low, high, name in BANDS:
        idx = np.where((freqs >= low) & (freqs <= high))[0]
        band_energy = np.sum(D[idx, :])
        energies[name] = band_energy / total_energy if total_energy > 1e-12 else 0.0
    return energies


def compute_rise_fall_times(segment_audio, sr):
    abs_audio = np.abs(segment_audio)
    win_samples = int(0.010 * sr)
    if win_samples < 1:
        win_samples = 1
    envelope = np.convolve(abs_audio, np.ones(win_samples) / win_samples, mode='same')
    max_val = np.max(envelope)
    if max_val < 1e-10:
        return 0.0, 0.0
    env_norm = envelope / max_val
    above_10 = np.where(env_norm >= 0.1)[0]
    above_90 = np.where(env_norm >= 0.9)[0]
    rise_time = (above_90[0] - above_10[0]) / sr if len(above_10) and len(above_90) else 0.0
    rev_env = env_norm[::-1]
    above_10_rev = np.where(rev_env >= 0.1)[0]
    above_90_rev = np.where(rev_env >= 0.9)[0]
    fall_time = (above_90_rev[0] - above_10_rev[0]) / sr if len(above_10_rev) and len(above_90_rev) else 0.0
    if fall_time < 0:
        fall_time = abs(fall_time)
    return rise_time, fall_time


def extract_features(segment_audio, sr, prev_pause, next_pause):
    duration = len(segment_audio) / sr
    peak_amp = np.max(np.abs(segment_audio))
    mean_amp = np.mean(np.abs(segment_audio))
    rise_t, fall_t = compute_rise_fall_times(segment_audio, sr)
    mean_f0 = compute_pitch_autocorrelation(segment_audio, sr)
    spec_energies = compute_spectral_band_energies(segment_audio, sr)
    mfcc = librosa.feature.mfcc(y=segment_audio, sr=sr, n_mfcc=13,
                                n_fft=int(0.050 * sr), hop_length=int(0.025 * sr))
    mfcc_means = np.mean(mfcc, axis=1)
    features = {
        'duration': duration, 'prev_pause': prev_pause, 'next_pause': next_pause,
        'peak_amplitude': peak_amp, 'mean_amplitude': mean_amp,
        'rise_time': rise_t, 'fall_time': fall_t,
        'mean_f0': mean_f0 if not np.isnan(mean_f0) else 0.0
    }
    for name, val in spec_energies.items():
        features[f'spec_{name}'] = val
    for i in range(13):
        features[f'mfcc_{i + 1}'] = mfcc_means[i]
    return features


# ============================================================
# Telegram API функции
# ============================================================
async def telegram_api_call(method: str, params: dict = None, files: dict = None):
    url = f"{TELEGRAM_API_URL}/{method}"
    try:
        async with aiohttp.ClientSession() as session:
            if files:
                form = aiohttp.FormData()
                for key, value in (params or {}).items():
                    form.add_field(key, str(value))
                for field_name, file_data in files.items():
                    form.add_field(field_name, file_data['data'],
                                   filename=file_data.get('filename', 'file'),
                                   content_type=file_data.get('content_type', 'application/octet-stream'))
                async with session.post(url, data=form) as resp:
                    return await resp.json()
            else:
                async with session.post(url, json=params) as resp:
                    return await resp.json()
    except Exception as e:
        logger.error(f"Telegram API call failed ({method}): {e}")
        return None


async def send_telegram_message(text: str):
    result = await telegram_api_call("sendMessage", {
        "chat_id": TELEGRAM_CHAT_ID,
        "text": text,
        "parse_mode": "HTML"
    })
    if result and result.get("ok"):
        logger.info("Telegram message sent")
    else:
        logger.error(f"Telegram sendMessage failed: {result}")


async def send_telegram_photo(image_bytes: bytes, caption: str = ""):
    result = await telegram_api_call("sendPhoto",
                                     {"chat_id": TELEGRAM_CHAT_ID, "caption": caption},
                                     {"photo": {"data": image_bytes, "filename": "photo.jpg",
                                                "content_type": "image/jpeg"}}
                                     )
    if result and result.get("ok"):
        logger.info("Telegram photo sent")
    else:
        logger.error(f"Telegram sendPhoto failed: {result}")


async def send_telegram_voice(audio_bytes: bytes, caption: str = ""):
    """Отправка голосового сообщения в Telegram."""
    # Конвертируем PCM 16-bit в WAV
    wav_buf = io.BytesIO()
    with wave.open(wav_buf, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(SR)
        wf.writeframes(audio_bytes)
    wav_buf.seek(0)

    result = await telegram_api_call("sendVoice",
                                     {"chat_id": TELEGRAM_CHAT_ID, "caption": caption},
                                     {"voice": {"data": wav_buf.read(), "filename": "cry_audio.ogg",
                                                "content_type": "audio/ogg"}}
                                     )
    if result and result.get("ok"):
        logger.info("Telegram voice sent")
    else:
        logger.error(f"Telegram sendVoice failed: {result}")


async def check_telegram_updates():
    global last_telegram_update_id
    params = {"timeout": 5, "offset": last_telegram_update_id + 1}
    result = await telegram_api_call("getUpdates", params)
    if not result or not result.get("ok"):
        return
    for update in result.get("result", []):
        update_id = update.get("update_id", 0)
        if update_id > last_telegram_update_id:
            last_telegram_update_id = update_id
        message = update.get("message")
        if not message:
            continue
        chat_id = str(message.get("chat", {}).get("id", ""))
        text = message.get("text", "")
        if text:
            await handle_telegram_command(chat_id, text.strip())


async def handle_telegram_command(chat_id: str, text: str):
    text_lower = text.lower()
    logger.info(f"Telegram command from {chat_id}: {text}")

    if text_lower in ["/start", "/help"]:
        help_text = (
            "🤖 <b>Сістэма маніторынгу ESP32</b>\n\n"
            "📷 <b>Камера:</b>\n"
            "/photo — Зрабіць фота\n"
            "/stream_on — Уключыць стрым\n"
            "/stream_off — Выключыць стрым\n\n"
            "🎤 <b>Аўдыё:</b>\n"
            "/audio_on — Уключыць аўдыё\n"
            "/audio_off — Выключыць аўдыё\n\n"
            "🌡 <b>Датчыкі:</b>\n"
            "/sensors — Паказанні датчыкаў\n\n"
            "💡 <b>Начнік:</b>\n"
            "/light_on — Уключыць\n"
            "/light_off — Выключыць\n"
            "/light_soft — Цёплае святло\n"
            "/light_white — Белае святло\n"
            "/light_rainbow — Вясёлка\n"
            "/light_bright_50 — Яркасць 50%\n\n"
            "📊 <b>Сістэма:</b>\n"
            "/status — Статус сістэмы\n"
            "/reboot — Перазагрузка ESP"
        )
        await send_telegram_message(help_text)

    elif text_lower == "/photo":
        await send_to_esp("photo")
        await send_telegram_message("📸 Раблю фота...")

    elif text_lower == "/stream_on":
        await send_to_esp("stream_on")
        await send_telegram_message("✅ Стрым уключаны")

    elif text_lower == "/stream_off":
        await send_to_esp("stream_off")
        await send_telegram_message("✅ Стрым выключаны")

    elif text_lower == "/audio_on":
        await send_to_esp("audio_on")
        await send_telegram_message("✅ Аўдыё ўключана")

    elif text_lower == "/audio_off":
        await send_to_esp("audio_off")
        await send_telegram_message("✅ Аўдыё выключана")

    elif text_lower == "/sensors":
        t = last_sensor_data["temp"]
        h = last_sensor_data["hum"]
        p = last_sensor_data["pres"]
        msg = f"🌡 <b>Паказанні датчыкаў:</b>\nТэмпература: {t:.1f}°C\nВільготнасць: {h:.1f}%\nЦіск: {p:.1f} мм рт.сл."
        await send_telegram_message(msg)

    elif text_lower == "/status":
        esp_count = len(esp_clients)
        browser_count = len(browser_clients)
        msg = (f"📊 <b>Статус сістэмы:</b>\n"
               f"ESP32 падключана: {esp_count}\n"
               f"Браўзераў падключана: {browser_count}\n"
               f"Дэтэктар плачу: {'актыўны' if cry_detector else 'не загружаны'}")
        await send_telegram_message(msg)

    elif text_lower == "/reboot":
        await send_to_esp("reboot")
        await send_telegram_message("🔄 Каманда перазагрузкі адпраўлена")

    elif text_lower.startswith("/light"):
        await send_to_esp(text_lower.replace("/", ""))
        await send_telegram_message(f"💡 Каманда адпраўлена: {text}")

    else:
        await send_telegram_message("Невядомая каманда. Выкарыстоўвайце /help для спісу каманд.")


# ============================================================
# Проверка порогов датчиков
# ============================================================
async def check_sensor_thresholds(temp: float, hum: float, pres: float):
    alerts = []
    if temp < TEMP_MIN:
        alerts.append(f"❄️ Нізкая тэмпература: {temp:.1f}°C (норма: {TEMP_MIN}-{TEMP_MAX}°C)")
    elif temp > TEMP_MAX:
        alerts.append(f"🔥 Высокая тэмпература: {temp:.1f}°C (норма: {TEMP_MIN}-{TEMP_MAX}°C)")
    if hum < HUM_MIN:
        alerts.append(f"💧 Нізкая вільготнасць: {hum:.1f}% (норма: {HUM_MIN}-{HUM_MAX}%)")
    elif hum > HUM_MAX:
        alerts.append(f"💦 Высокая вільготнасць: {hum:.1f}% (норма: {HUM_MIN}-{HUM_MAX}%)")
    if pres < PRES_MIN:
        alerts.append(f"📉 Нізкі ціск: {pres:.1f} мм рт.сл. (норма: {PRES_MIN}-{PRES_MAX})")
    elif pres > PRES_MAX:
        alerts.append(f"📈 Высокі ціск: {pres:.1f} мм рт.сл. (норма: {PRES_MIN}-{PRES_MAX})")

    if alerts:
        alert_msg = "⚠️ <b>Увага! Выяўлены адхіленні:</b>\n" + "\n".join(alerts)
        await send_telegram_message(alert_msg)
        await send_to_esp("photo")


# ============================================================
# Детектор плача
# ============================================================
async def analyze_audio_for_cry(audio_data, raw_audio_bytes=None):
    global last_cry_time, last_image, cry_audio_buffer

    if cry_detector is None or len(audio_data) < SR * 0.5:
        return False

    # Накапливаем сырые аудиоданные для отправки при обнаружении плача
    if raw_audio_bytes:
        cry_audio_buffer.extend(raw_audio_bytes)
        # Держим буфер не более 15 секунд
        max_bytes = SR * 15 * 2  # 15 сек * 2 байта на семпл
        if len(cry_audio_buffer) > max_bytes:
            cry_audio_buffer = cry_audio_buffer[-max_bytes:]

    audio = np.array(audio_data, dtype=np.float32)
    audio = audio / (np.max(np.abs(audio)) + 1e-10)
    segments = extract_energy_segments(audio, SR)
    cry_segments, total_segments, max_cry_prob = 0, 0, 0.0

    for start, end in segments:
        seg_audio = audio[int(start * SR):int(end * SR)]
        rms = np.sqrt(np.mean(seg_audio ** 2))
        peak = np.max(np.abs(seg_audio))
        peak_to_rms = peak / rms if rms > 1e-10 else 1.0
        if peak_to_rms < MIN_PEAK_TO_RMS_RATIO or (end - start) < 0.2:
            continue
        total_segments += 1
        features = extract_features(seg_audio, SR, 0.5, 0.5)
        X = np.array([[features.get(col, 0.0) for col in FEATURE_COLS]])
        proba = cry_detector.predict_proba(X)
        if proba[0][1] > 0.5:
            cry_segments += 1
            max_cry_prob = max(max_cry_prob, proba[0][1])

    if total_segments > 0 and cry_segments > 0:
        cry_ratio = cry_segments / total_segments
        current_time = asyncio.get_event_loop().time()
        if cry_ratio > 0.3 and current_time - last_cry_time > CRY_ALERT_COOLDOWN:
            last_cry_time = current_time
            logger.warning(f"ДЗІЦЯЧЫ ПЛАЧ! ({cry_segments}/{total_segments} сегментаў)")

            # Отправляем текстовое уведомление
            await send_telegram_message(
                f"<b>⚠️ Заўважаны дзіцячы плач!</b>\n\n"
                f"Верагоднасць: {max_cry_prob:.1%}\n"
                f"Сегментаў плачу: {cry_segments} з {total_segments}"
            )

            # Отправляем фото
            if last_image is not None:
                await send_telegram_photo(last_image,
                                          caption=f"📸 Фота пры выяўленні плачу\nВерагоднасць: {max_cry_prob:.1%}")

            # Отправляем аудиофрагмент (последние 5+ секунд из буфера)
            if len(cry_audio_buffer) > 0:
                # Берём последние 5 секунд аудио
                five_sec_bytes = SR * 5 * 2  # 5 сек * 2 байта/семпл
                audio_chunk = bytes(cry_audio_buffer[-five_sec_bytes:]) if len(
                    cry_audio_buffer) > five_sec_bytes else bytes(cry_audio_buffer)
                await send_telegram_voice(audio_chunk,
                                          caption=f"🔊 Аўдыё плачу\nВерагоднасць: {max_cry_prob:.1%}\nПрацягласць: ~{len(audio_chunk) / (SR * 2):.1f}с")
                cry_audio_buffer = bytearray()  # Очищаем буфер после отправки

            await notify_browsers({"type": "cry_alert", "probability": max_cry_prob})
            return True
    return False


# ============================================================
# Отправка команд на ESP32 и уведомлений браузерам
# ============================================================
async def send_to_esp(cmd: str):
    dead = []
    for esp_id, ws in esp_clients.items():
        try:
            await ws.send_str(cmd)
        except Exception as e:
            logger.warning(f"Failed to send command to ESP {esp_id}: {e}")
            dead.append(esp_id)
    for d in dead:
        del esp_clients[d]


async def notify_browsers(data: dict):
    dead = []
    for ws in browser_clients:
        try:
            await ws.send_str(json.dumps(data))
        except Exception as e:
            logger.warning(f"Failed to notify browser: {e}")
            dead.append(ws)
    for d in dead:
        browser_clients.discard(d)


# ============================================================
# Фоновые задачи
# ============================================================
async def telegram_polling_task():
    while True:
        await asyncio.sleep(TELEGRAM_POLL_INTERVAL)
        try:
            await check_telegram_updates()
        except Exception as e:
            logger.error(f"Telegram polling error: {e}")


async def heartbeat_task():
    while True:
        await asyncio.sleep(5)
        dead = []
        for esp_id, ws in esp_clients.items():
            try:
                await ws.send_str("heartbeat")
            except Exception:
                dead.append(esp_id)
        for d in dead:
            logger.warning(f"ESP32 {d} disconnected (heartbeat failed)")
            del esp_clients[d]


# ============================================================
# HTML страница на белорусском языке с работающим звуком
# ============================================================
HTML_PAGE = """
<!DOCTYPE html>
<html lang="be">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Baby Monitor — Сачэнне за дзіцём</title>
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
            <div class="header-title">Сачэнне за дзіцём</div>
            <div class="header-subtitle"><span class="status-dot"></span> Сістэма актыўная</div>
        </div>
    </div>
    <div style="text-align:right">
        <div style="font-size:24px" id="clock">--:--</div>
        <div style="font-size:11px;color:var(--text-secondary)" id="date">--</div>
    </div>
</div>

<div class="alert-overlay" id="alertOverlay">
    <span class="alert-icon">⚠️</span>
    <span>Заўважаны дзіцячы плач!</span>
</div>

<div class="video-container">
    <img src="/stream" id="videoFeed" alt="Відэа">
</div>

<div class="stats-bar">
    <div class="stat-card">
        <div class="stat-value" id="statTemp">--°</div>
        <div class="stat-label">Тэмпература</div>
    </div>
    <div class="stat-card">
        <div class="stat-value" id="statHum">--%</div>
        <div class="stat-label">Вільготнасць</div>
    </div>
    <div class="stat-card">
        <div class="stat-value" id="statPres">--</div>
        <div class="stat-label">Ціск</div>
    </div>
    <div class="stat-card">
        <div class="stat-value" id="statCry">Добра</div>
        <div class="stat-label">Статус</div>
    </div>
</div>

<div class="control-panel">
    <button class="btn btn-primary" onclick="sendCmd('photo')">📸 Фота</button>
    <button class="btn" onclick="sendCmd('stream_on')">▶️ Стрым</button>
    <button class="btn" onclick="sendCmd('stream_off')">⏹️ Стоп</button>
    <button class="btn" onclick="sendCmd('audio_on')">🎤 Аўдыё</button>
    <button class="btn" onclick="sendCmd('audio_off')">🔇 Глушыць</button>
    <button class="btn btn-danger" onclick="sendCmd('light_on')">💡 Святло</button>
</div>

<script>
function sendCmd(cmd) {
    fetch('/cmd?c=' + encodeURIComponent(cmd)).catch(function(e) {});
}

function updateClock() {
    var now = new Date();
    document.getElementById('clock').textContent = 
        now.toLocaleTimeString('be-BY', {hour:'2-digit', minute:'2-digit'});
    document.getElementById('date').textContent = 
        now.toLocaleDateString('be-BY', {day:'numeric', month:'long', weekday:'short'});
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
        document.getElementById('statCry').textContent = 'Добра';
        document.getElementById('statCry').style.color = '';
        document.getElementById('statCry').style.webkitTextFillColor = '';
    }, 5000);
}

// ============================================================
// WebSocket и аудио
// ============================================================
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


# ============================================================
# HTTP и WebSocket обработчики
# ============================================================
async def index(request):
    return web.Response(text=HTML_PAGE, content_type="text/html")


async def cmd(request):
    c = request.query.get("c")
    if c:
        await send_to_esp(c)
    return web.Response(text="ok")


async def stream(request):
    resp = web.StreamResponse(status=200, headers={
        "Content-Type": "multipart/x-mixed-replace; boundary=frame",
        "Cache-Control": "no-cache", "Pragma": "no-cache"
    })
    await resp.prepare(request)
    stream_clients.add(resp)
    logger.info("Stream client connected")
    if last_image:
        try:
            await resp.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + last_image + b"\r\n")
        except Exception as e:
            logger.error(f"Error sending last frame: {e}")
    try:
        while True:
            await asyncio.sleep(3600)
    finally:
        stream_clients.discard(resp)


async def browser_ws(request):
    ws = web.WebSocketResponse(heartbeat=60)
    await ws.prepare(request)
    browser_clients.add(ws)
    logger.info("Browser connected")
    try:
        async for _ in ws:
            pass
    finally:
        browser_clients.discard(ws)
    return ws


async def esp_ws(request):
    global last_image, last_sensor_data, last_sensor_time
    ws = web.WebSocketResponse(heartbeat=60, max_msg_size=0, protocols=("arduino",))
    await ws.prepare(request)
    esp_id = str(int(time.time() * 1000))
    esp_clients[esp_id] = ws
    logger.info(f"ESP32 connected (ID: {esp_id})")
    await send_telegram_message("✅ ESP32 падключана да сервера")
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
                        audio_samples = np.frombuffer(payload, dtype=np.int16).astype(np.float32) / 32768.0
                        audio_buffer.extend(audio_samples)
                        # Передаём сырые байты для накопления в буфере плача
                        await analyze_audio_for_cry(list(audio_buffer), raw_audio_bytes=payload)
                        dead = []
                        for b in browser_clients:
                            try:
                                await b.send_bytes(payload)
                            except Exception:
                                dead.append(b)
                        for d in dead:
                            browser_clients.discard(d)

            elif msg.type == web.WSMsgType.TEXT:
                text = msg.data
                logger.info(f"ESP text: {text}")

                if text == "loud_sound":
                    logger.info("Loud sound detected by ESP32")
                    await send_telegram_message("🔊 <b>Заўважаны гучны гук!</b>")
                    if last_image:
                        await send_telegram_photo(last_image, "📸 Фота пры гучным гуку")

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
                        logger.error(f"Failed to parse sensor data: {e}")

            elif msg.type == web.WSMsgType.ERROR:
                logger.error(f"WebSocket error: {ws.exception()}")

    except Exception as e:
        logger.error(f"ESP handler error: {e}")
    finally:
        if esp_id in esp_clients:
            del esp_clients[esp_id]
        logger.info(f"ESP32 disconnected (ID: {esp_id})")
        await send_telegram_message("❌ ESP32 адключана ад сервера")
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


# ============================================================
# Запуск приложения
# ============================================================
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
    logger.info("Запуск сервера на порце 8080...")
    web.run_app(app, host="0.0.0.0", port=8080)
