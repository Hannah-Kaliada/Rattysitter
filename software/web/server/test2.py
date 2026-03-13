import asyncio
import struct
from aiohttp import web




esp_clients = set()
browser_clients = set()
stream_clients = set()


last_image = None


HTML_PAGE = """
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Camera</title>
</head>
<body style="margin:0;background:black;color:white;font-family:sans-serif">


<img src="/stream" width="100%">


<div style="position:fixed;bottom:10px;left:10px">
<button onclick="cmd('photo')">📸 Photo</button>
<button onclick="cmd('stream_on')">Stream ON</button>
<button onclick="cmd('stream_off')">Stream OFF</button>
<button onclick="cmd('audio_on')">Audio ON</button>
<button onclick="cmd('audio_off')">Audio OFF</button>
</div>


<script>
function cmd(c){
 fetch("/cmd?c="+encodeURIComponent(c));
}


let ws = new WebSocket("ws://"+location.host+"/browser");


const audioCtx = new (window.AudioContext || window.webkitAudioContext)();
let audioTime = 0;


document.body.addEventListener("click", ()=>{
 if(audioCtx.state !== "running") audioCtx.resume();
}, {once:true});


function playPCM16(buffer){
 const samples = new Int16Array(buffer);
 const f32 = new Float32Array(samples.length);


 for(let i=0;i<samples.length;i++)
   f32[i] = samples[i] / 32768;


 const audioBuffer = audioCtx.createBuffer(1, f32.length, 16000);
 audioBuffer.copyToChannel(f32, 0);


 const src = audioCtx.createBufferSource();
 src.buffer = audioBuffer;
 src.connect(audioCtx.destination);


 if(audioTime < audioCtx.currentTime)
   audioTime = audioCtx.currentTime;


 src.start(audioTime);
 audioTime += audioBuffer.duration;
}


ws.onmessage = async (e)=>{
 if(typeof e.data !== "string"){
   const buf = await e.data.arrayBuffer();
   playPCM16(buf);
 }
};
</script>


</body>
</html>
"""


async def index(request):
   return web.Response(text=HTML_PAGE, content_type="text/html")


async def cmd(request):
   c = request.query.get("c")
   if c:
       await send_to_esp(c)
   return web.Response(text="ok")




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
   finally:
       stream_clients.discard(resp)
       print("Stream client disconnected")


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




async def browser_ws(request):
   ws = web.WebSocketResponse(heartbeat=60)
   await ws.prepare(request)
   browser_clients.add(ws)
   print("Browser connected")


   try:
       async for _ in ws:
           pass
   finally:
       browser_clients.discard(ws)
       print("Browser disconnected")


   return ws


async def esp_ws(request):
   global last_image


   ws = web.WebSocketResponse(
       heartbeat=60,
       max_msg_size=0,
       protocols=("arduino",)   # ⭐ ВАЖНО
   )
   await ws.prepare(request)


   esp_clients.add(ws)
   print("ESP connected")


   buffer = bytearray()


   try:
       async for msg in ws:


           if msg.type == web.WSMsgType.BINARY:
               buffer.extend(msg.data)


               while True:
                   if len(buffer) < 4:
                       break


                   pkt_type, pkt_len = struct.unpack_from("<HH", buffer, 0)


                   if pkt_len <= 0 or pkt_len > 200_000:
                       buffer.pop(0)
                       continue


                   if len(buffer) < 4 + pkt_len:
                       break


                   payload = bytes(buffer[4:4+pkt_len])
                   del buffer[:4+pkt_len]


                   if pkt_type == 1:
                       last_image = payload
                       await push_frame(payload)


                   elif pkt_type == 2:
                       print("AUDIO OK:", len(payload))
                       dead = []
                       for b in browser_clients:
                           try:
                               await b.send_bytes(payload)
                           except:
                               dead.append(b)
                       for d in dead:
                           browser_clients.discard(d)


           elif msg.type == web.WSMsgType.TEXT:
               print("ESP:", msg.data)


   finally:
       esp_clients.discard(ws)
       print("ESP disconnected", ws.close_code)


   return ws




async def send_to_esp(cmd: str):
   dead = []
   for ws in esp_clients:
       try:
           await ws.send_str(cmd)
       except:
           dead.append(ws)
   for d in dead:
       esp_clients.discard(d)


app = web.Application()
app.router.add_get("/", index)
app.router.add_get("/cmd", cmd)
app.router.add_get("/stream", stream)
app.router.add_get("/browser", browser_ws)
app.router.add_get("/ws", esp_ws)


web.run_app(app, host="0.0.0.0", port=8080)



