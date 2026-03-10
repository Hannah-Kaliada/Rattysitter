import asyncio
from aiohttp import web


clients = set()          # ESP websocket clients
browser_clients = set()  # Browser websocket clients
stream_clients = set()   # MJPEG stream clients
last_image = None




# ================= HTML =================


HTML_PAGE = """
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Camera</title>


<style>
:root{
 --bg:#0b0f14;
 --panel:#121821;
 --accent:#4da3ff;
 --accent-2:#7cffc4;
 --text:#e6edf3;
 --muted:#8b949e;
 --danger:#ff6b6b;
}


*{box-sizing:border-box}


body{
 margin:0;
 background:var(--bg);
 color:var(--text);
 font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
 display:flex;
 flex-direction:column;
 height:100vh;
}


/* Video area */
.viewer{
 position:relative;
 flex:1;
 background:black;
 display:flex;
 align-items:center;
 justify-content:center;
 overflow:hidden;
}


.viewer img{
 width:100%;
 height:100%;
 object-fit:contain;
}


/* Overlay controls */
.controls{
 position:absolute;
 bottom:18px;
 left:50%;
 transform:translateX(-50%);
 display:flex;
 gap:12px;
 background:rgba(18,24,33,.75);
 backdrop-filter:blur(10px);
 border:1px solid rgba(255,255,255,.06);
 padding:10px 14px;
 border-radius:16px;
}


.btn{
 border:none;
 padding:10px 14px;
 font-size:14px;
 border-radius:12px;
 cursor:pointer;
 color:white;
 background:#1f2937;
 transition:.15s transform,.15s background,.15s box-shadow;
 box-shadow:0 2px 10px rgba(0,0,0,.35);
}


.btn:hover{ transform:translateY(-1px); }
.btn:active{ transform:translateY(0); }


.btn-accent{ background:var(--accent); color:#081018; font-weight:600; }
.btn-accent:hover{ box-shadow:0 0 0 3px rgba(77,163,255,.25); }


.btn-green{ background:#16a34a; }
.btn-blue{ background:#2563eb; }
.btn-red{ background:#dc2626; }
.btn-white{ background:#e5e7eb; color:#111; }


/* Night light panel */
.night-panel{
 position:absolute;
 top:18px;
 right:18px;
 display:flex;
 gap:8px;
 background:rgba(18,24,33,.75);
 backdrop-filter:blur(10px);
 border:1px solid rgba(255,255,255,.06);
 padding:10px;
 border-radius:14px;
}


.night-title{
 position:absolute;
 top:-22px;
 right:0;
 font-size:12px;
 color:var(--muted);
}


/* Status dot */
.status{
 position:absolute;
 top:18px;
 left:18px;
 display:flex;
 align-items:center;
 gap:8px;
 background:rgba(18,24,33,.75);
 backdrop-filter:blur(10px);
 border:1px solid rgba(255,255,255,.06);
 padding:8px 12px;
 border-radius:999px;
 font-size:13px;
 color:var(--muted);
}


.dot{
 width:10px;
 height:10px;
 border-radius:50%;
 background:#999;
}
.dot.on{ background:#22c55e; box-shadow:0 0 8px #22c55e; }
.dot.off{ background:#ef4444; box-shadow:0 0 8px #ef4444; }


/* Log drawer */
.log{
 height:140px;
 overflow:auto;
 background:var(--panel);
 border-top:1px solid rgba(255,255,255,.06);
 padding:10px 14px;
 font-size:12px;
 color:#c9d1d9;
}
</style>
</head>


<body>


<div class="viewer">
 <img src="/stream" id="cam">


 <div class="status">
   <div class="dot off" id="wsDot"></div>
   <div id="wsText">connecting</div>
 </div>


 <div class="night-panel">
   <div class="night-title">Night light</div>
   <button class="btn btn-red"   onclick="sendCmd('rgb 4095 0 0')">Red</button>
   <button class="btn btn-green" onclick="sendCmd('rgb 0 4095 0')">Green</button>
   <button class="btn btn-blue"  onclick="sendCmd('rgb 0 0 4095')">Blue</button>
   <button class="btn btn-white" onclick="sendCmd('rgb 4095 4095 4095')">White</button>
   <button class="btn"           onclick="sendCmd('rgb 0 0 0')">Off</button>
 </div>


 <div class="controls">
   <button class="btn btn-accent" onclick="sendCmd('photo')">📸 Photo</button>
   <button class="btn" onclick="sendCmd('get_sensors')">Sensors</button>
   <button class="btn" onclick="sendCmd('stream_on')">Stream ON</button>
   <button class="btn" onclick="sendCmd('stream_off')">Stream OFF</button>
 </div>
</div>


<div id="log" class="log"></div>


<script>
let wsDot  = document.getElementById("wsDot");
let wsText = document.getElementById("wsText");
let logEl  = document.getElementById("log");


let ws = new WebSocket("ws://" + location.host + "/browser");


ws.onopen = () => {
 wsDot.className = "dot on";
 wsText.innerText = "connected";
 sendCmd("stream_on");
};


ws.onclose = () => {
 wsDot.className = "dot off";
 wsText.innerText = "disconnected";
};


ws.onerror = () => {
 wsDot.className = "dot off";
 wsText.innerText = "error";
};


ws.onmessage = (event) => {
 if(event.data.startsWith("log:")){
   logEl.innerHTML += event.data.substring(4) + "<br>";
   logEl.scrollTop = logEl.scrollHeight;
 }
}


function sendCmd(cmd){
 fetch("/cmd?c=" + encodeURIComponent(cmd));
}


window.onbeforeunload = () => sendCmd("stream_off");
</script>


</body>
</html>
"""




# ================= HTTP =================


async def index(request):
   return web.Response(text=HTML_PAGE, content_type="text/html")




async def image(request):
   global last_image
   if last_image is None:
       return web.Response(status=404)
   return web.Response(body=last_image, content_type="image/jpeg")




async def cmd(request):
   command = request.query.get("c")
   if command:
       await send_to_esp(command)
   return web.Response(text="ok")




# ================= MJPEG STREAM =================


async def stream(request):


   resp = web.StreamResponse(
       status=200,
       headers={
           "Content-Type": "multipart/x-mixed-replace; boundary=frame",
           "Cache-Control": "no-cache",
           "Pragma": "no-cache",
       },
   )


   await resp.prepare(request)
   stream_clients.add(resp)
   print("Stream client connected")


   try:
       while True:
           await asyncio.sleep(3600)
   except asyncio.CancelledError:
       pass
   finally:
       stream_clients.discard(resp)
       print("Stream client disconnected")


   return resp




async def push_frame(frame: bytes):
   dead = []
   for resp in stream_clients:
       try:
           await resp.write(
               b"--frame\r\n"
               b"Content-Type: image/jpeg\r\n\r\n" +
               frame + b"\r\n"
           )
       except:
           dead.append(resp)
   for d in dead:
       stream_clients.discard(d)




# ================= WEBSOCKET =================


async def browser_ws(request):


   ws = web.WebSocketResponse()
   await ws.prepare(request)
   browser_clients.add(ws)
   print("Browser WS connected")


   try:
       async for _ in ws:
           pass
   finally:
       browser_clients.discard(ws)
       print("Browser WS disconnected")


   return ws




async def esp_ws(request):


   global last_image


   ws = web.WebSocketResponse(max_msg_size=20*1024*1024)
   await ws.prepare(request)


   clients.add(ws)
   print("ESP connected")


   try:
       async for msg in ws:


           if msg.type == web.WSMsgType.BINARY:
               last_image = msg.data
               await push_frame(msg.data)
               print("Image:", len(msg.data), "bytes")


           elif msg.type == web.WSMsgType.TEXT:
               text = msg.data
               print("ESP:", text)


               dead = []
               for b in browser_clients:
                   try:
                       await b.send_str("log:" + text)
                   except:
                       dead.append(b)


               for d in dead:
                   browser_clients.discard(d)


   finally:
       clients.discard(ws)
       print("ESP disconnected")


   return ws




async def send_to_esp(cmd: str):
   dead = []
   for ws in clients:
       try:
           await ws.send_str(cmd)
       except:
           dead.append(ws)
   for d in dead:
       clients.discard(d)




# ================= AUTO PHOTO LOOP =================
# Эмуляция стрима без изменения ESP


async def auto_photo_loop():
   while True:
       if clients:                     # если ESP подключён
           await send_to_esp("photo")  # просим кадр
       await asyncio.sleep(0.2)        # ~5 FPS




# ================= APP =================


app = web.Application()


app.router.add_get("/", index)
app.router.add_get("/image", image)
app.router.add_get("/cmd", cmd)
app.router.add_get("/stream", stream)
app.router.add_get("/browser", browser_ws)
app.router.add_get("/ws", esp_ws)


loop = asyncio.get_event_loop()
loop.create_task(auto_photo_loop())


web.run_app(app, host="0.0.0.0", port=8080)



