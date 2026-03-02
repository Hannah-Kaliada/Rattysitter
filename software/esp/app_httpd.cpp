#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_timer.h"

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-CAM</title>
<style>
body{margin:0;background:#111;color:#fff;text-align:center;font-family:Arial}
img{width:100%;max-width:640px;margin-top:10px}
button,select,input{margin:5px;padding:6px;font-size:14px}
</style>
</head>
<body>

<h2>ESP32-CAM</h2>

<img id="stream">

<br>
<button onclick="startStream()">Start Streaming</button>
<button onclick="stopStream()">Stop</button>
<button onclick="capture()">Capture</button>

<br><br>

<label>Resolution:</label>
<select onchange="setResolution(this.value)">
<option value="10">UXGA</option>
<option value="9">SXGA</option>
<option value="8">XGA</option>
<option value="7">SVGA</option>
<option value="6">VGA</option>
<option value="4">QVGA</option>
<option value="0">QQVGA</option>
</select>

<br><br>

<label>Quality:</label><br>
<input type="range" min="10" max="63" value="10"
onchange="setQuality(this.value)">

<br><br>

<label>Auto White Balance:</label><br>
<select onchange="setAWB(this.value)">
<option value="1">Enabled</option>
<option value="0">Disabled</option>
</select>

<br><br>

<label>WB Mode:</label><br>
<select onchange="setWBMode(this.value)">
<option value="0">Auto</option>
<option value="1">Sunny</option>
<option value="2">Cloudy</option>
<option value="3">Office</option>
<option value="4">Home</option>
</select>

<script>
function startStream(){
    const img = document.getElementById('stream');
    img.src = "http://" + window.location.hostname + ":81/stream";
}
function stopStream(){
    document.getElementById('stream').src="";
}
function capture(){
    window.open('/capture','_blank');
}
function setResolution(val){
    fetch('/control?var=framesize&val='+val);
}
function setQuality(val){
    fetch('/control?var=quality&val='+val);
}
function setAWB(val){
    fetch('/control?var=awb&val='+val);
}
function setWBMode(val){
    fetch('/control?var=wb_mode&val='+val);
}
</script>

</body>
</html>
)rawliteral";

    return httpd_resp_send(req, html, strlen(html));
}


static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb)
        return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}


static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while(true)
    {
        fb = esp_camera_fb_get();
        if(!fb){
            res = ESP_FAIL;
            break;
        }

        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if(res == ESP_OK){
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);
        if(res != ESP_OK)
            break;
    }

    return res;
}


static esp_err_t cmd_handler(httpd_req_t *req)
{
    char buf[100];
    char variable[32];
    char value[32];

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK)
        return httpd_resp_send_404(req);

    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK)
        return httpd_resp_send_404(req);

    sensor_t *s = esp_camera_sensor_get();
    if (!s)
        return httpd_resp_send_500(req);

    int val = atoi(value);
    int res = 0;

    if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "framesize"))
        res = s->set_framesize(s, (framesize_t)val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else
        return httpd_resp_send_404(req);

    if(res != 0)
        return httpd_resp_send_500(req);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}


void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t index_uri = { "/", HTTP_GET, index_handler, NULL };
    httpd_uri_t cmd_uri = { "/control", HTTP_GET, cmd_handler, NULL };
    httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
    httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}
