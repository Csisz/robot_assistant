#include "camera_web_server.h"
#include "camera_driver.h"
#include "face_enroll.h"
#include "face_status.h"
#include "face_detect.h"
#include "face_recognition.h"
#include "robot_state.h"
#include "robot_chat.h"
#include "robot_voice.h"
#include "led_effects.h"
#include "wifi_manager.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "cam_server";

static httpd_handle_t    s_server  = NULL;

/* Last successfully encoded JPEG — served as a stale frame while audio is
   playing so frame2jpg() never races SDMMC DMA during MP3 playback. */
static uint8_t *s_cached_jpg     = NULL;
static size_t   s_cached_jpg_len = 0;

static bool s_last_was_speaking = false;

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static void send_json_ok(httpd_req_t *req, const char *extra)
{
    char buf[256];
    if (extra && extra[0])
        snprintf(buf, sizeof(buf), "{\"ok\":true,%s}", extra);
    else
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void send_json_error(httpd_req_t *req, const char *error)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}",
             error ? error : "unknown error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static int read_body(httpd_req_t *req, char *buf, size_t bufsize)
{
    if (!req->content_len) { buf[0] = '\0'; return 0; }
    size_t to_read = req->content_len < bufsize - 1
                     ? req->content_len : bufsize - 1;
    int n = httpd_req_recv(req, buf, to_read);
    if (n < 0) return n;
    buf[n] = '\0';
    return n;
}

static void json_str(const char *body, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!body) return;

    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return;
    p++;

    size_t pos = 0;
    while (*p && *p != '"' && pos + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            out[pos++] = (*p == 'n') ? ' ' : *p;
            p++;
        } else {
            out[pos++] = *p++;
        }
    }
    out[pos] = '\0';
}

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    size_t pos = 0;
    if (!src) { dst[0] = '\0'; return; }
    for (const char *s = src; *s && pos + 1 < dst_len; s++) {
        if ((*s == '"' || *s == '\\') && pos + 2 < dst_len) {
            dst[pos++] = '\\';
            dst[pos++] = *s;
        } else if (*s == '\n' && pos + 2 < dst_len) {
            dst[pos++] = '\\';
            dst[pos++] = 'n';
        } else if (*s != '\r') {
            dst[pos++] = *s;
        }
    }
    dst[pos] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTML page                                                           */
/* ------------------------------------------------------------------ */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Robot Assistant</title>"
    "<style>"
    "body{background:#111;color:#eee;font-family:sans-serif;margin:0;padding:12px;"
    "max-width:500px;margin:0 auto}"
    "h2{text-align:center;margin:8px 0 12px;font-size:1.2em}"
    "img{max-width:100%;border:2px solid #555;border-radius:6px;display:block;margin:0 auto}"
    ".card{background:#1e1e1e;border:1px solid #444;border-radius:8px;padding:12px;margin:10px 0}"
    ".card h3{margin:0 0 8px;font-size:.85em;color:#bbb;border-bottom:1px solid #333;padding-bottom:4px}"
    "label{font-size:.78em;color:#999;display:block;margin-top:5px}"
    "input[type=text]{background:#252525;border:1px solid #555;color:#eee;padding:5px 8px;"
    "border-radius:4px;width:100%;box-sizing:border-box;font-size:.88em;margin:2px 0}"
    "textarea{background:#252525;border:1px solid #555;color:#eee;padding:6px 8px;"
    "border-radius:4px;width:100%;box-sizing:border-box;font-size:.88em;margin:2px 0;"
    "min-height:62px;resize:vertical}"
    ".row{display:flex;gap:5px;flex-wrap:wrap;margin-top:8px}"
    "button{padding:6px 11px;border:none;border-radius:4px;cursor:pointer;font-size:.8em;font-weight:600}"
    ".bs{background:#2563eb;color:#fff}.bc{background:#16a34a;color:#fff}"
    ".bf{background:#7c3aed;color:#fff}.bx{background:#dc2626;color:#fff}"
    ".bn{background:#374151;color:#eee}.by{background:#b45309;color:#fff}"
    "button:disabled{opacity:.35;cursor:not-allowed}"
    ".msg{margin-top:7px;padding:5px 8px;background:#1a1a1a;border-radius:4px;"
    "font-size:.78em;color:#888;min-height:1.4em}"
    ".ok{color:#4ade80}.er{color:#f87171}.warn{color:#fbbf24}"
    "table{width:100%;font-size:.8em;border-collapse:collapse}"
    "td{padding:3px 6px;border-bottom:1px solid #222}"
    "td:first-child{color:#888;width:44%}"
    "pre{background:#141414;padding:8px;border-radius:4px;font-size:.72em;"
    "overflow:auto;max-height:180px;white-space:pre-wrap;margin:0}"
    "#camst{text-align:center;font-size:.75em;color:#555;margin-top:3px}"
    "</style></head><body>"
    "<h2>Robot Assistant</h2>"

    /* Camera */
    "<div class='card'>"
    "<h3>Camera</h3>"
    "<img id='cam' src='/capture.jpg'>"
    "<div id='camst'>Connecting...</div>"
    "</div>"

    /* Kids Chat */
    "<div class='card'>"
    "<h3>Kids Chat Test</h3>"
    "<label>Question for the backend</label>"
    "<textarea id='chatmsg'>Mi lesz, ha a pirosat es a sargat osszekeverem?</textarea>"
    "<div class='row'>"
    "<button class='bs' onclick='chatSend()'>Send</button>"
#if CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
    "<button class='bc' onclick='voiceTest()'>Record 2.5s voice</button>"
    "<button class='bx' onclick='voiceReset()'>Reset voice</button>"
    "<a class='bn' href='/api/voice/last.wav' target='_blank'>Download last WAV</a>"
    "<a class='bn' href='/api/voice/debug/slot0.wav' target='_blank'>Slot0</a>"
    "<a class='bn' href='/api/voice/debug/slot1.wav' target='_blank'>Slot1</a>"
    "<a class='bn' href='/api/voice/debug/slot2.wav' target='_blank'>Slot2</a>"
    "<a class='bn' href='/api/voice/debug/slot3.wav' target='_blank'>Slot3</a>"
#endif
    "</div>"
    "<div id='chatmeta' class='msg'>Conversation: none | Mode: kids_chat | Profile: guest</div>"
    "<div id='voicemeta' class='msg'>Voice: unknown</div>"
    "<div id='chatout' class='msg'>ESP32 proxy: /api/chat</div>"
    "</div>"

    /* Detection & Recognition */
    "<div class='card'>"
    "<h3>Detection &amp; Recognition</h3>"
    "<div id='rdet' style='font-size:.95em;padding:2px 0'>Loading...</div>"
    "<div id='rinfo' style='font-size:.76em;color:#666;margin-top:2px'></div>"
    "<div id='reng' style='font-size:.76em;margin-top:4px'></div>"
    "</div>"

    /* Robot State */
    "<div class='card'>"
    "<h3>Robot State</h3>"
    "<table>"
    "<tr><td>State</td><td id='trst'></td></tr>"
    "<tr><td>LED</td><td id='trled'></td></tr>"
    "<tr><td>Enrollment</td><td id='tren'></td></tr>"
    "<tr><td>Known people</td><td id='trkp'></td></tr>"
    "<tr><td>Last audio</td><td id='traud'></td></tr>"
    "<tr><td>Last error</td><td id='trerr'></td></tr>"
    "</table>"
    "</div>"

    /* Known People */
    "<div class='card'>"
    "<h3>Known People</h3>"
    "<div id='plist' style='font-size:.82em'>Loading...</div>"
    "<div class='row'>"
    "<button class='bn' onclick='loadPeople()'>Refresh</button>"
    "</div>"
    "</div>"

    /* Enrollment */
    "<div class='card'>"
    "<h3>Face Enrollment</h3>"
    "<label>Person ID (A-Z, 0-9, max 8)</label>"
    "<input id='eid' type='text' placeholder='apa'>"
    "<label>Display Name</label>"
    "<input id='ename' type='text' placeholder='Apa'>"
    "<label>Audio File (on SD card)</label>"
    "<input id='eaudio' type='text' placeholder='/sdcard/APA.MP3'>"
    "<div class='row'>"
    "<button class='bs' id='bst' onclick='eStart()'>Start</button>"
    "<button class='bc' id='bca' onclick='eCap()' disabled>Capture</button>"
    "<button class='bf' id='bfi' onclick='eFin()' disabled>Finish</button>"
    "<button class='bx' id='bcn' onclick='eCancel()' disabled>Cancel</button>"
    "</div>"
    "<div id='estat' class='msg'>Idle</div>"
    "</div>"

    /* Audio Test */
    "<div class='card'>"
    "<h3>Audio Test</h3>"
    "<label>File path (must start with /sdcard/ and end with .mp3)</label>"
    "<input id='afile' type='text' placeholder='/sdcard/APA.MP3'>"
    "<div class='row'>"
    "<button class='bs' onclick='playAudio()'>Play</button>"
    "</div>"
    "<div id='amsg' class='msg'></div>"
    "</div>"

    /* LED Test */
    "<div class='card'>"
    "<h3>LED Test</h3>"
    "<div class='row'>"
    "<button class='bn' onclick='led(\"idle\")'>Idle</button>"
    "<button class='bs' onclick='led(\"rainbow\")'>Rainbow</button>"
    "<button class='bc' onclick='led(\"recognized\")'>Recognized</button>"
    "<button class='bx' onclick='led(\"error\")'>Error</button>"
    "<button class='bf' onclick='led(\"speaking\")'>Speaking</button>"
    "</div>"
    "<div id='lmsg' class='msg'></div>"
    "</div>"

    /* Mock State */
    "<div class='card'>"
    "<h3>Mock State <span style='font-weight:normal;color:#666;font-size:.85em'>"
    "(requires ROBOT_MOCK_MODE build flag)</span></h3>"
    "<div class='row'>"
    "<button class='bn' onclick='mock(\"no_face\")'>No face</button>"
    "<button class='bn' onclick='mock(\"face\")'>Face</button>"
    "<button class='bn' onclick='mock(\"recognized\")'>Recognized</button>"
    "<button class='by' onclick='mock(\"speaking\")'>Speaking</button>"
    "</div>"
    "<div id='mmsg' class='msg'>"
    "Recompile with -DROBOT_MOCK_MODE to enable mock injection.</div>"
    "</div>"

    /* System Diagnostics */
    "<div class='card'>"
    "<h3>System Diagnostics</h3>"
    "<pre id='diag'>{}</pre>"
    "</div>"

    "<script>"
    /* Camera refresh */
    "let cok=0,cerr=0;"
    "setInterval(()=>{"
    "  const i=new Image(),t=Date.now();"
    "  i.onload=()=>{document.getElementById('cam').src=i.src;"
    "    document.getElementById('camst').textContent='Frame OK ('+cok+')';"
    "    cok++;};"
    "  i.onerror=()=>{document.getElementById('camst').textContent='ERR '+cerr++;};  "
    "  i.src='/capture.jpg?t='+t;},1000);"

    /* Status poll */
    "setInterval(async()=>{"
    "  try{"
    "    const d=await(await fetch('/status')).json();"
    "    const det=document.getElementById('rdet');"
    "    const inf=document.getElementById('rinfo');"
    "    const eng=document.getElementById('reng');"
    "    if(!d.face_present){"
    "      det.textContent='No face detected';det.className='';inf.textContent='';}"
    "    else if(!d.recognition_available){"
    "      det.textContent='Face detected — recognition engine not available';"
    "      det.className='warn';inf.textContent='';}"
    "    else if(!d.recognized){"
    "      det.textContent='Face detected — unknown person';"
    "      det.className='warn';inf.textContent='';}"
    "    else{"
    "      det.textContent='Recognized: '+d.display_name;"
    "      det.className='ok';"
    "      inf.textContent='ID: '+d.person_id"
    "        +'  |  Confidence: '+Math.round(d.confidence*100)+'%';}"
    "    eng.textContent=(d.recognition_available?'✓ Recognition engine ready'"
    "      :'✗ Recognition engine not available')"
    "      +'  |  Known: '+d.known_people_count;"
    "    eng.className=d.recognition_available?'ok':'er';"
    "    document.getElementById('trst').textContent=d.robot_state||'';"
    "    document.getElementById('trled').textContent=d.led_state||'';"
    "    document.getElementById('tren').textContent=d.enrollment_state||'idle';"
    "    document.getElementById('trkp').textContent=d.known_people_count||0;"
    "    document.getElementById('traud').textContent=d.last_audio||'—';"
    "    const te=document.getElementById('trerr');"
    "    te.textContent=d.last_error||'—';te.className=d.last_error?'er':'';"
    "    document.getElementById('diag').textContent=JSON.stringify(d,null,2);"
    "  }catch(e2){}},750);"

    /* Known people */
    "async function loadPeople(){"
    "  const el=document.getElementById('plist');"
    "  el.textContent='Loading...';"
    "  try{"
    "    const arr=await(await fetch('/people')).json();"
    "    if(!Array.isArray(arr)||!arr.length){"
    "      el.innerHTML=\"<span class='warn'>No people enrolled or DB.TXT missing</span>\";"
    "      return;}"
    "    el.innerHTML=arr.map(p=>"
    "      '<div style=\"padding:3px 0;border-bottom:1px solid #252525\">'"
    "      +'<b>'+(p.display_name||p.name||'?')+'</b>'"
    "      +' <span style=\"color:#666\">('+p.id+')</span>'"
    "      +' — '+(p.audio_file||p.audio||'')"
    "      +' — '+(p.sample_count||p.samples||0)+' samples</div>'"
    "    ).join('');"
    "  }catch(e){"
    "    el.textContent='Error loading people ('+e.message+')';}}"
    "loadPeople();"

    /* Enrollment */
    "function enrBtn(on){"
    "  document.getElementById('bst').disabled=on;"
    "  ['bca','bfi','bcn'].forEach(id=>document.getElementById(id).disabled=!on);}"
    "function estat(msg,cls){"
    "  const e=document.getElementById('estat');e.textContent=msg;e.className='msg '+(cls||'');}"
    "async function jpost(u,b){"
    "  const o={method:'POST'};"
    "  if(b){o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(b);}"
    "  return(await fetch(u,o)).json();}"
    "async function eStart(){"
    "  const id=document.getElementById('eid').value.trim();"
    "  const nm=document.getElementById('ename').value.trim();"
    "  const au=document.getElementById('eaudio').value.trim();"
    "  if(!id||!nm){estat('ID and Name are required','er');return;}"
    "  const r=await jpost('/enroll/start',{id,display_name:nm,audio_file:au});"
    "  if(r.ok){enrBtn(true);estat('Enrolling '+nm+' — capture ≥3 samples','ok');}"
    "  else estat('Error: '+r.error,'er');}"
    "async function eCap(){"
    "  estat('Capturing...','');"
    "  const r=await jpost('/enroll/capture');"
    "  if(r.ok)estat('Sample '+r.sample_count+(r.sample_count<3"
    "    ?' (need '+(3-r.sample_count)+' more)':''),'ok');"
    "  else estat('Error: '+r.error,'er');}"
    "async function eFin(){"
    "  const r=await jpost('/enroll/finish');"
    "  if(r.ok){enrBtn(false);estat('Saved: '+r.id+', '+r.samples+' samples','ok');loadPeople();}"
    "  else estat('Error: '+r.error,'er');}"
    "async function eCancel(){"
    "  await jpost('/enroll/cancel');enrBtn(false);estat('Cancelled','');}"
    "setInterval(async()=>{"
    "  try{"
    "    const s=await(await fetch('/enroll/status')).json();"
    "    if(s.active){enrBtn(true);estat('Enrolling: '+s.display_name"
    "      +' | '+s.sample_count+(s.sample_count<3"
    "      ?' (need '+(3-s.sample_count)+' more)':''),'ok');}}"
    "  catch(e){}},4000);"

    /* Kids chat */
    "let chatConversationId='';"
    "let chatProfileId='';"
    "let chatActiveMode='kids_chat';"
    "function chatMeta(){"
    "  document.getElementById('chatmeta').textContent='Conversation: '"
    "    +(chatConversationId||'none')+' | Mode: '+(chatActiveMode||'kids_chat')"
    "    +' | Profile: '+(chatProfileId||'guest');}"
    "async function chatSend(){"
    "  const m=document.getElementById('chatmsg').value.trim();"
    "  const out=document.getElementById('chatout');"
    "  if(!m){out.textContent='Enter a question';out.className='msg er';return;}"
    "  out.textContent='Thinking...';out.className='msg';"
    "  try{"
    "    const body={message:m};"
    "    if(chatConversationId)body.conversation_id=chatConversationId;"
    "    const r=await jpost('/api/chat',body);"
    "    if(r.conversation_id)chatConversationId=r.conversation_id;"
    "    if(r.profile_id)chatProfileId=r.profile_id;"
    "    if(r.active_mode)chatActiveMode=r.active_mode;"
    "    chatMeta();"
    "    out.textContent=(r.reply_text||r.error||'No reply')"
    "      +(r.reply_audio_url?'\\nAudio: '+r.reply_audio_url:'')"
    "      +'\\nmode='+(r.active_mode||'')+' profile='+(r.profile_id||'')"
    "      +'\\nconversation_id='+(r.conversation_id||'');"
    "    out.className='msg '+(r.safe?'ok':(r.ok?'warn':'er'));"
    "  }catch(e){out.textContent='Request failed';out.className='msg er';}}"
#if CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
    "async function voiceTest(){"
    "  const out=document.getElementById('chatout');"
    "  out.textContent='Recording 2.5 seconds on robot...';out.className='msg';"
    "  try{const r=await jpost('/api/voice/record',{});"
    "    out.textContent=r.ok?'Voice recording started':'Voice start failed: '+(r.error||'unknown');"
    "    out.className='msg '+(r.ok?'ok':'er');"
    "    voiceStatus();"
    "  }catch(e){out.textContent='Voice request failed';out.className='msg er';}}"
    "async function voiceReset(){try{const r=await jpost('/api/voice/reset',{});"
    "  document.getElementById('chatout').textContent=r.ok?'Voice reset':'Voice reset failed';"
    "  voiceStatus();}catch(e){}}"
    "async function voiceStatus(){try{const r=await j('/api/voice/status');"
    "  document.getElementById('voicemeta').textContent='Voice: '+(r.voice_state||'?')"
    "    +' | available='+(r.voice_available?'yes':'no')"
    "    +' | size='+(r.last_recording_size||0)"
    "    +' | peak='+(r.peak_abs||0)+' | rms='+(r.rms||0)"
    "    +' | dBFS='+(r.dbfs||'?')"
    "    +' | selected='+(r.selected_slot!==undefined?r.selected_slot:'?')+'/'+(r.selected_mode||'?')"
    "    +' | total_ms='+(r.total_ms||0)"
    "    +' | upload_ms='+(r.upload_ms||0)"
    "    +' | silence='+(r.silence_ratio_per_mille||0)+'/1000'"
    "    +(r.appears_silent?' | appears silent':'')"
    "    +(r.last_voice_error?' | error='+r.last_voice_error:'');"
    "}catch(e){document.getElementById('voicemeta').textContent='Voice: status unavailable';}}"
    "setInterval(voiceStatus,4000);voiceStatus();"
#endif

    /* Audio test — do NOT use encodeURIComponent; /sdcard/ paths are safe as-is */
    "async function playAudio(){"
    "  const f=document.getElementById('afile').value.trim();"
    "  if(!f){document.getElementById('amsg').textContent='Enter a file path';return;}"
    "  try{"
    "    const r=await jpost('/api/play?file='+f);"
    "    const m=document.getElementById('amsg');"
    "    m.textContent=r.ok?'Playing: '+f:'Error: '+r.error;"
    "    m.className='msg '+(r.ok?'ok':'er');"
    "  }catch(e){document.getElementById('amsg').textContent='Request failed';}}"

    /* LED test */
    "async function led(s){"
    "  try{"
    "    const r=await jpost('/api/led/'+s);"
    "    const m=document.getElementById('lmsg');"
    "    m.textContent=r.ok?'LED → '+s:'Error: '+(r.error||'?');"
    "    m.className='msg '+(r.ok?'ok':'er');"
    "  }catch(e){}}"

    /* Mock state */
    "async function mock(s){"
    "  const m=document.getElementById('mmsg');"
    "  try{"
    "    const resp=await fetch('/api/mock/state?scenario='+s,{method:'POST'});"
    "    if(resp.status===404||resp.status===405){"
    "      m.textContent='Not enabled — recompile with -DROBOT_MOCK_MODE';"
    "      m.className='msg warn';return;}"
    "    const r=await resp.json();"
    "    m.textContent=r.ok?'Mock → '+s:'Error: '+r.error;"
    "    m.className='msg '+(r.ok?'ok':'er');"
    "  }catch(e){m.textContent='Request failed';m.className='msg er';}}"
    "</script></body></html>";

/* ------------------------------------------------------------------ */
/* Route: GET /                                                        */
/* ------------------------------------------------------------------ */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Route: GET /capture.jpg                                            */
/* ------------------------------------------------------------------ */
static esp_err_t capture_handler(httpd_req_t *req)
{
    if (robot_is_speaking()) {
        if (!s_last_was_speaking) {
            ESP_LOGI(TAG, "capture: speaking — serving cached frame");
            s_last_was_speaking = true;
        }
        if (s_cached_jpg && s_cached_jpg_len > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, (const char *)s_cached_jpg,
                                   (ssize_t)s_cached_jpg_len);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "speaking, no cached frame");
        return ESP_FAIL;
    }
    s_last_was_speaking = false;

    camera_fb_t *fb = NULL;
    if (camera_capture_frame(&fb) != ESP_OK || !fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }
    if (robot_is_speaking()) {
        camera_release_frame(fb);
        s_last_was_speaking = true;
        if (s_cached_jpg && s_cached_jpg_len > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, (const char *)s_cached_jpg,
                                   (ssize_t)s_cached_jpg_len);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "speaking");
        return ESP_FAIL;
    }

    uint8_t *jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    camera_release_frame(fb);

    if (!ok || !jpg_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg conv failed");
        free(jpg_buf);
        return ESP_FAIL;
    }
    free(s_cached_jpg);
    s_cached_jpg     = jpg_buf;
    s_cached_jpg_len = jpg_len;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)s_cached_jpg,
                           (ssize_t)s_cached_jpg_len);
}

/* ------------------------------------------------------------------ */
/* Route: GET /status                                                  */
/* ------------------------------------------------------------------ */
static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[2600];
    char face_json[2048];
    char ssid_json[80];
    char backend_json[240];
    face_status_get_json(face_json, sizeof(face_json));
    json_escape(wifi_manager_ssid(), ssid_json, sizeof(ssid_json));
    json_escape(wifi_manager_backend_base_url(), backend_json, sizeof(backend_json));
    size_t len = strlen(face_json);
    if (len > 0 && face_json[len - 1] == '}') {
        face_json[len - 1] = '\0';
        snprintf(buf, sizeof(buf),
                 "%s,\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,"
                 "\"backend_base_url\":\"%s\"}",
                 face_json,
                 ssid_json,
                 wifi_manager_rssi(),
                 backend_json);
    } else {
        strlcpy(buf, face_json, sizeof(buf));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Route: GET /people  — parse /sdcard/FACES/DB.TXT → JSON array     */
/* ------------------------------------------------------------------ */
static esp_err_t people_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    FILE *f = fopen("/sdcard/FACES/DB.TXT", "r");
    if (!f) {
        ESP_LOGI(TAG, "people_handler: DB.TXT missing -> returning []");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    /* Heap-allocate: 3500 bytes on the httpd task stack would overflow it. */
    const size_t RBUF = 3500;
    char *resp = (char *)malloc(RBUF);
    if (!resp) {
        fclose(f);
        ESP_LOGE(TAG, "people_handler: malloc failed");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    size_t pos = 0;
    resp[pos++] = '[';

    char line[160];
    bool first = true;
    int  count = 0;
    while (fgets(line, sizeof(line), f) && pos < RBUF - 300) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        char pid[16] = {0}, name[64] = {0}, afile[64] = {0};
        int  samples = 0;
        if (sscanf(line, "%15[^,],%63[^,],%63[^,],%d",
                   pid, name, afile, &samples) != 4) continue;

        /* Guard against NULL fields (sscanf partial match) */
        if (!pid[0] || !name[0]) continue;

        pos += snprintf(resp + pos, RBUF - pos,
            "%s{\"id\":\"%s\",\"display_name\":\"%s\","
            "\"audio_file\":\"%s\",\"sample_count\":%d}",
            first ? "" : ",", pid, name, afile[0] ? afile : "", samples);
        first = false;
        count++;
    }
    fclose(f);

    if (pos < RBUF - 2) {
        resp[pos++] = ']';
        resp[pos]   = '\0';
    } else {
        resp[RBUF - 2] = ']';
        resp[RBUF - 1] = '\0';
        pos = RBUF - 1;
    }

    ESP_LOGI(TAG, "people_handler: loaded %d people, returning JSON length %d",
             count, (int)pos);
    httpd_resp_send(req, resp, (ssize_t)pos);
    free(resp);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Route: POST /api/play?file=/sdcard/FOO.MP3                        */
/* ------------------------------------------------------------------ */
static bool is_valid_audio_path(const char *p)
{
    if (!p || strncmp(p, "/sdcard/", 8) != 0) return false;
    if (strstr(p, "..")) return false;          /* no path traversal */
    size_t len = strlen(p);
    if (len < 12) return false;
    const char *e = p + len - 4;
    return (e[0] == '.' &&
            (e[1] == 'm' || e[1] == 'M') &&
            (e[2] == 'p' || e[2] == 'P') &&
            e[3] == '3');
}

static esp_err_t api_play_handler(httpd_req_t *req)
{
    char file_raw[80] = {0};

    /* Check query string first */
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 120) {
        char query[128];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "file", file_raw, sizeof(file_raw));
    }
    /* Fall back to form-encoded body */
    if (!file_raw[0]) {
        char body[128];
        read_body(req, body, sizeof(body));
        httpd_query_key_value(body, "file", file_raw, sizeof(file_raw));
    }

    if (!file_raw[0]) {
        send_json_error(req, "missing 'file' parameter");
        return ESP_FAIL;
    }

    /* URL-decode: encodeURIComponent('/') -> '%2F'; decode before validation */
    char file_param[80] = {0};
    {
        const char *s = file_raw;
        char *d = file_param;
        char *end = file_param + sizeof(file_param) - 1;
        while (*s && d < end) {
            if (s[0] == '%' && s[1] && s[2]) {
                char hex[3] = {s[1], s[2], '\0'};
                *d++ = (char)strtol(hex, NULL, 16);
                s += 3;
            } else {
                *d++ = *s++;
            }
        }
        *d = '\0';
    }

    if (!is_valid_audio_path(file_param)) {
        ESP_LOGW(TAG, "api_play: rejected path '%s' (raw='%s')", file_param, file_raw);
        send_json_error(req, "invalid path: must start with /sdcard/ and end with .mp3 or .MP3");
        return ESP_FAIL;
    }

    /* /sdcard/APA.MP3 → file://sdcard/APA.MP3 */
    char url[88];
    snprintf(url, sizeof(url), "file://sdcard/%s", file_param + 8);

    face_status_set_last_audio(file_param);
    robot_say_file("Audio test", url);

    send_json_ok(req, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Route: POST /api/chat                                              */
/* ------------------------------------------------------------------ */
static esp_err_t api_chat_handler(httpd_req_t *req)
{
    typedef struct {
        char body[768];
        char message[512];
        char reply[768];
        char audio[320];
        char error[128];
        char reason[80];
        char conversation[128];
        char profile[64];
        char active[48];
        char resp[1800];
    } chat_handler_buf_t;

    chat_handler_buf_t *buf = heap_caps_calloc(1, sizeof(*buf), MALLOC_CAP_8BIT);
    if (!buf) {
        send_json_error(req, "chat buffer allocation failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "api_chat_handler: received chat request");
    if (read_body(req, buf->body, sizeof(buf->body)) < 0) {
        free(buf);
        send_json_error(req, "body recv failed");
        return ESP_OK;
    }

    json_str(buf->body, "message", buf->message, sizeof(buf->message));
    if (!buf->message[0]) {
        free(buf);
        send_json_error(req, "missing message");
        return ESP_OK;
    }

    robot_chat_response_t chat = {0};
    ESP_LOGI(TAG, "api_chat_handler: queued chat request");
    esp_err_t err = robot_chat_send_text(buf->message, &chat);

    json_escape(chat.reply_text, buf->reply, sizeof(buf->reply));
    json_escape(chat.reply_audio_url, buf->audio, sizeof(buf->audio));
    json_escape(chat.error, buf->error, sizeof(buf->error));
    json_escape(chat.blocked_reason, buf->reason, sizeof(buf->reason));
    json_escape(chat.conversation_id, buf->conversation, sizeof(buf->conversation));
    json_escape(chat.profile_id, buf->profile, sizeof(buf->profile));
    json_escape(chat.active_mode, buf->active, sizeof(buf->active));

    snprintf(buf->resp, sizeof(buf->resp),
        "{\"ok\":%s,\"safe\":%s,\"http_status\":%d,"
        "\"conversation_id\":\"%s\",\"profile_id\":\"%s\","
        "\"reply_text\":\"%s\",\"reply_audio_url\":\"%s\","
        "\"blocked_reason\":\"%s\",\"active_mode\":\"%s\",\"error\":\"%s\"}",
        err == ESP_OK ? "true" : "false",
        chat.safe ? "true" : "false",
        chat.http_status,
        buf->conversation,
        buf->profile,
        buf->reply,
        buf->audio,
        buf->reason,
        buf->active,
        buf->error);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf->resp, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t api_voice_test_handler(httpd_req_t *req)
{
    char body[32];
    (void)read_body(req, body, sizeof(body));
    char error[160] = {0};
    esp_err_t err = robot_voice_start_push_to_talk_ex(error, sizeof(error));
    if (err != ESP_OK) {
        char escaped[220];
        char resp[280];
        json_escape(error[0] ? error : esp_err_to_name(err), escaped, sizeof(escaped));
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"started\":false,\"error\":\"%s\"}", escaped);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    send_json_ok(req, "\"started\":true,\"message\":\"Recording started\"");
    return ESP_OK;
}

static esp_err_t api_voice_status_handler(httpd_req_t *req)
{
    char resp[1400];
    robot_voice_status_json(resp, sizeof(resp));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t serve_voice_wav_file(httpd_req_t *req, const char *path, const char *download_name)
{
    struct stat st = {0};
    if (stat(path, &st) != 0 || st.st_size <= 44) {
        ESP_LOGW(TAG, "voice last.wav unavailable path=%s errno=%d (%s)",
                 path, errno, strerror(errno));
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"WAV not found\"}", HTTPD_RESP_USE_STRLEN);
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "voice last.wav open failed path=%s errno=%d (%s)",
                 path, errno, strerror(errno));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"failed to open WAV\"}", HTTPD_RESP_USE_STRLEN);
    }

    uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"buffer allocation failed\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "serving voice WAV path=%s size=%ld", path, (long)st.st_size);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"",
             download_name ? download_name : "VOICE.WAV");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    esp_err_t err = ESP_OK;
    while (true) {
        size_t n = fread(buf, 1, 512, f);
        if (n > 0) {
            err = httpd_resp_send_chunk(req, (const char *)buf, n);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "voice last.wav send failed: %s", esp_err_to_name(err));
                break;
            }
        }
        if (n < 512) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "voice last.wav read failed errno=%d (%s)", errno, strerror(errno));
                err = ESP_FAIL;
            }
            break;
        }
    }

    free(buf);
    fclose(f);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t api_voice_last_wav_handler(httpd_req_t *req)
{
    return serve_voice_wav_file(req, ROBOT_VOICE_WAV_PATH, "VOICE.WAV");
}

static esp_err_t api_voice_debug_slot0_handler(httpd_req_t *req)
{
    return serve_voice_wav_file(req, ROBOT_VOICE_DEBUG_SLOT0_PATH, "SLOT0.WAV");
}

static esp_err_t api_voice_debug_slot1_handler(httpd_req_t *req)
{
    return serve_voice_wav_file(req, ROBOT_VOICE_DEBUG_SLOT1_PATH, "SLOT1.WAV");
}

static esp_err_t api_voice_debug_slot2_handler(httpd_req_t *req)
{
    return serve_voice_wav_file(req, ROBOT_VOICE_DEBUG_SLOT2_PATH, "SLOT2.WAV");
}

static esp_err_t api_voice_debug_slot3_handler(httpd_req_t *req)
{
    return serve_voice_wav_file(req, ROBOT_VOICE_DEBUG_SLOT3_PATH, "SLOT3.WAV");
}

static esp_err_t api_voice_reset_handler(httpd_req_t *req)
{
    char body[32];
    (void)read_body(req, body, sizeof(body));
    robot_voice_force_reset();
    send_json_ok(req, "\"reset\":true");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Routes: POST /api/led/<state>                                      */
/* ------------------------------------------------------------------ */
#define LED_TEST_TICKS 60   /* 60 × 50 ms = 3 s */

static esp_err_t api_led_idle_handler(httpd_req_t *req)
{
    led_test_force(LED_IDLE, LED_TEST_TICKS);
    send_json_ok(req, NULL);
    return ESP_OK;
}
static esp_err_t api_led_rainbow_handler(httpd_req_t *req)
{
    led_test_force(LED_SPEAKING, LED_TEST_TICKS);   /* rainbow = SPEAKING animation */
    send_json_ok(req, NULL);
    return ESP_OK;
}
static esp_err_t api_led_speaking_handler(httpd_req_t *req)
{
    led_test_force(LED_SPEAKING, LED_TEST_TICKS);
    send_json_ok(req, NULL);
    return ESP_OK;
}
static esp_err_t api_led_recognized_handler(httpd_req_t *req)
{
    led_set_state(LED_RECOGNIZED);
    send_json_ok(req, NULL);
    return ESP_OK;
}
static esp_err_t api_led_error_handler(httpd_req_t *req)
{
    led_set_state(LED_ERROR);
    send_json_ok(req, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Routes: GET /enroll/status  POST /enroll/start|capture|finish|cancel */
/* ------------------------------------------------------------------ */
static esp_err_t enroll_status_handler(httpd_req_t *req)
{
#if !CONFIG_ROBOT_ENABLE_FACE_ENROLLMENT
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        "{\"active\":false,\"disabled\":true,\"id\":\"\",\"display_name\":\"\",\"sample_count\":0}",
        HTTPD_RESP_USE_STRLEN);
#else
    enroll_status_t st = face_enroll_get_status();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"id\":\"%s\",\"display_name\":\"%s\",\"sample_count\":%d}",
             st.active ? "true" : "false",
             st.id, st.display_name, st.sample_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
#endif
}

static esp_err_t enroll_start_handler(httpd_req_t *req)
{
#if !CONFIG_ROBOT_ENABLE_FACE_ENROLLMENT
    send_json_error(req, "face enrollment disabled");
    return ESP_FAIL;
#else
    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        send_json_error(req, "body recv failed"); return ESP_FAIL;
    }
    esp_err_t err = face_enroll_start_json(body);
    if (err == ESP_ERR_INVALID_ARG) {
        send_json_error(req, "invalid JSON or missing id/display_name"); return ESP_FAIL;
    }
    if (err != ESP_OK) { send_json_error(req, "start failed"); return ESP_FAIL; }
    send_json_ok(req, NULL);
    return ESP_OK;
#endif
}

static esp_err_t enroll_capture_handler(httpd_req_t *req)
{
#if !CONFIG_ROBOT_ENABLE_FACE_ENROLLMENT
    send_json_error(req, "face enrollment disabled");
    return ESP_FAIL;
#else
    int count = 0;
    esp_err_t err = face_enroll_capture(&count);
    if (err == ESP_ERR_NOT_SUPPORTED)  { send_json_error(req, "maximum samples reached"); return ESP_FAIL; }
    if (err == ESP_ERR_NOT_FOUND)      { send_json_error(req, "no face detected"); return ESP_FAIL; }
    if (err == ESP_ERR_INVALID_STATE)  { send_json_error(req, "audio playing or not enrolling"); return ESP_FAIL; }
    if (err == ESP_ERR_NO_MEM)         { send_json_error(req, "out of memory"); return ESP_FAIL; }
    if (err != ESP_OK)                 { send_json_error(req, "capture or SD write failed"); return ESP_FAIL; }
    char extra[48];
    snprintf(extra, sizeof(extra), "\"sample_count\":%d", count);
    send_json_ok(req, extra);
    return ESP_OK;
#endif
}

static esp_err_t enroll_finish_handler(httpd_req_t *req)
{
#if !CONFIG_ROBOT_ENABLE_FACE_ENROLLMENT
    send_json_error(req, "face enrollment disabled");
    return ESP_FAIL;
#else
    int  samples = 0;
    char id[32]  = {0};
    esp_err_t err = face_enroll_finish(&samples, id, sizeof(id));
    if (err == ESP_ERR_INVALID_STATE) {
        send_json_error(req, "not enrolling or need at least 3 samples"); return ESP_FAIL;
    }
    if (err != ESP_OK) { send_json_error(req, "failed to save DB.TXT"); return ESP_FAIL; }
    face_recog_refresh_known_count();
    face_status_set_known_people_count(face_recog_get_known_count());
    char extra[80];
    snprintf(extra, sizeof(extra), "\"samples\":%d,\"id\":\"%s\"", samples, id);
    send_json_ok(req, extra);
    return ESP_OK;
#endif
}

static esp_err_t enroll_cancel_handler(httpd_req_t *req)
{
#if !CONFIG_ROBOT_ENABLE_FACE_ENROLLMENT
    send_json_ok(req, "\"disabled\":true");
    return ESP_OK;
#else
    face_enroll_cancel();
    send_json_ok(req, NULL);
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* Route: POST /api/mock/state?scenario=...  (ROBOT_MOCK_MODE only)  */
/* ------------------------------------------------------------------ */
#ifdef ROBOT_MOCK_MODE
static esp_err_t api_mock_handler(httpd_req_t *req)
{
    char scenario[32] = {0};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 80) {
        char query[96];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
            httpd_query_key_value(query, "scenario", scenario, sizeof(scenario));
    }

    if (!scenario[0]) {
        send_json_error(req,
            "missing scenario: no_face | face | recognized | speaking");
        return ESP_FAIL;
    }

    face_status_set_mock(scenario);

    if (strcmp(scenario, "speaking") == 0) {
        robot_set_speaking("Mock speaking");
        led_test_force(LED_SPEAKING, 120);   /* 6 s */
    } else {
        robot_set_idle();
    }

    send_json_ok(req, NULL);
    return ESP_OK;
}
#endif

/* ------------------------------------------------------------------ */
/* Route: GET /debug/detect — pixel-mode probe + face count           */
/*                                                                    */
/* Tests all four 16-bit pixel interpretations of the current live    */
/* RGB565 frame.  Uses face_detect_run_ex2() so global s_byte_swap    */
/* is never mutated mid-test; only the winning mode is persisted.     */
/*                                                                    */
/* pix_type_int constants (from dl_image_define.hpp):                 */
/*   9  = DL_IMAGE_PIX_TYPE_RGB565LE                                  */
/*   10 = DL_IMAGE_PIX_TYPE_RGB565BE                                  */
/*   11 = DL_IMAGE_PIX_TYPE_BGR565LE                                  */
/*   12 = DL_IMAGE_PIX_TYPE_BGR565BE                                  */
/* ------------------------------------------------------------------ */
static esp_err_t debug_detect_handler(httpd_req_t *req)
{
    static const char *DTAG = "face_debug";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

#if !CONFIG_ROBOT_ENABLE_FACE_DETECTION
    ESP_LOGW(DTAG, "/debug/detect requested but face detection is disabled");
    return httpd_resp_send(req,
        "{\"detector_ready\":false,\"disabled\":true,"
        "\"error\":\"face detection disabled by config\"}",
        HTTPD_RESP_USE_STRLEN);
#else
    /* --- check detector initialisation first ------------------------- */
    bool det_ready = face_detect_is_ready();
    unsigned psram_free = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (!det_ready) {
        char err[256];
        snprintf(err, sizeof(err),
            "{\"detector_ready\":false,"
            "\"detector_error\":\"HumanFaceDetect not initialised — check serial log for OOM\","
            "\"psram_free_bytes\":%u}", psram_free);
        ESP_LOGE(DTAG, "/debug/detect: detector NOT ready, psram_free=%u B", psram_free);
        httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* --- capture live RGB565 frame ----------------------------------- */
    camera_fb_t *fb = NULL;
    if (camera_capture_frame(&fb) != ESP_OK || !fb) {
        httpd_resp_send(req,
            "{\"detector_ready\":true,\"error\":\"camera capture failed\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int    width    = (int)fb->width;
    int    height   = (int)fb->height;
    int    len      = (int)fb->len;
    int    explen   = width * height * 2;
    int    pixfmt   = (int)fb->format;

    /* Copy to PSRAM and release camera mutex before slow inference */
    uint16_t *frame = (uint16_t *)heap_caps_malloc(
                          (size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame) memcpy(frame, fb->buf, (size_t)len);
    camera_release_frame(fb);

    if (!frame) {
        httpd_resp_send(req,
            "{\"detector_ready\":true,\"error\":\"PSRAM alloc failed\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* --- run all four pixel-type variants ---------------------------- */
    struct {
        const char *name;
        int         pix_type_int; /* maps to dl::image::pix_type_t */
        int         face_count;
        int         ms;
        int         bbox[4];
    } modes[4] = {
        { "rgb565_BE", 10, 0, 0, {0} },   /* DL_IMAGE_PIX_TYPE_RGB565BE */
        { "rgb565_LE", 9,  0, 0, {0} },   /* DL_IMAGE_PIX_TYPE_RGB565LE */
        { "bgr565_LE", 11, 0, 0, {0} },   /* DL_IMAGE_PIX_TYPE_BGR565LE */
        { "bgr565_BE", 12, 0, 0, {0} },   /* DL_IMAGE_PIX_TYPE_BGR565BE */
    };

    int best_idx   = 0;
    int best_faces = 0;

    for (int m = 0; m < 4; m++) {
        int64_t t0 = esp_timer_get_time();
        modes[m].face_count = face_detect_run_ex2(
            frame, width, height, modes[m].pix_type_int, modes[m].bbox);
        modes[m].ms = (int)((esp_timer_get_time() - t0) / 1000);

        ESP_LOGI(DTAG, "  %-12s -> faces=%d  ms=%d",
                 modes[m].name, modes[m].face_count, modes[m].ms);

        if (modes[m].face_count > best_faces) {
            best_faces = modes[m].face_count;
            best_idx   = m;
        }
    }
    free(frame);

    /* Persist winning mode in s_byte_swap (only if a face was found) */
    const char *active_mode;
    if (best_faces > 0) {
        bool use_swap = (modes[best_idx].pix_type_int == 9  ||   /* RGB565LE */
                         modes[best_idx].pix_type_int == 11 ||   /* BGR565LE */
                         modes[best_idx].pix_type_int == 12);    /* BGR565BE */
        face_detect_set_byte_swap(use_swap);
        active_mode = modes[best_idx].name;
        ESP_LOGI(DTAG, "Selected mode: %s (face_detect byte_swap=%d)",
                 active_mode, (int)use_swap);
    } else {
        active_mode = face_detect_get_byte_swap() ? "rgb565_LE" : "rgb565_BE";
        ESP_LOGI(DTAG, "No face detected in any mode — keeping current: %s", active_mode);
    }

    /* --- build JSON response ----------------------------------------- */
    char buf[1200];
    int  pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"detector_ready\":true,"
        "\"width\":%d,\"height\":%d,"
        "\"frame_len\":%d,\"expected_len\":%d,"
        "\"pixel_format\":%d,\"psram_free_bytes\":%u,"
        "\"face_count\":%d,"
        "\"active_pixel_mode\":\"%s\","
        "\"tests\":[",
        width, height, len, explen,
        pixfmt, psram_free,
        best_faces, active_mode);

    for (int m = 0; m < 4; m++) {
        if (pos >= (int)sizeof(buf) - 120) break;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"mode\":\"%s\",\"pix_type\":%d,"
            "\"face_count\":%d,\"inference_ms\":%d",
            m ? "," : "",
            modes[m].name, modes[m].pix_type_int,
            modes[m].face_count, modes[m].ms);

        if (modes[m].face_count > 0 && pos < (int)sizeof(buf) - 60) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                ",\"bbox\":{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
                modes[m].bbox[0], modes[m].bbox[1],
                modes[m].bbox[2], modes[m].bbox[3]);
        }
        if (pos < (int)sizeof(buf) - 2) { buf[pos++] = '}'; buf[pos] = '\0'; }
    }

    if (pos < (int)sizeof(buf) - 4) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    }

    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t camera_web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.stack_size        = 16384;
    cfg.max_uri_handlers  = 32;
    cfg.max_open_sockets  = 4;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    ESP_LOGI(TAG, "starting HTTP server...");
    ESP_LOGI(TAG, "httpd config: port=%u stack=%u max_uri_handlers=%u max_open_sockets=%u lru=%d recv_timeout=%d send_timeout=%d",
             (unsigned)cfg.server_port,
             (unsigned)cfg.stack_size,
             (unsigned)cfg.max_uri_handlers,
             (unsigned)cfg.max_open_sockets,
             cfg.lru_purge_enable,
             cfg.recv_wait_timeout,
             cfg.send_wait_timeout);
    ESP_LOGI(TAG, "httpd heap before start: free=%u internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "httpd_start failed with 16 KB stack, retrying with 12 KB stack");
        s_server = NULL;
        cfg.stack_size = 12288;
        ESP_LOGI(TAG, "httpd retry config: port=%u stack=%u max_uri_handlers=%u max_open_sockets=%u lru=%d",
                 (unsigned)cfg.server_port,
                 (unsigned)cfg.stack_size,
                 (unsigned)cfg.max_uri_handlers,
                 (unsigned)cfg.max_open_sockets,
                 cfg.lru_purge_enable);
        ESP_LOGI(TAG, "httpd heap before retry: free=%u internal=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        err = httpd_start(&s_server, &cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s (%d)", esp_err_to_name(err), err);
        return err;
    }
    ESP_LOGI(TAG, "httpd_start OK");

    /* Static helpers to avoid stack-allocating httpd_uri_t structs */
#define REG(uri_str, meth, fn) \
    do { \
        static const httpd_uri_t _u = { \
            .uri = uri_str, .method = meth, .handler = fn, \
        }; \
        esp_err_t _reg_err = httpd_register_uri_handler(s_server, &_u); \
        if (_reg_err == ESP_OK) { \
            ESP_LOGI(TAG, "registered %s %s", (meth) == HTTP_GET ? "GET" : "POST", uri_str); \
        } else { \
            ESP_LOGE(TAG, "failed to register %s %s: %s (%d)", \
                     (meth) == HTTP_GET ? "GET" : "POST", uri_str, esp_err_to_name(_reg_err), _reg_err); \
        } \
    } while (0)

    REG("/",                HTTP_GET,  index_handler);
    REG("/capture.jpg",     HTTP_GET,  capture_handler);
    REG("/status",          HTTP_GET,  status_handler);
    REG("/people",          HTTP_GET,  people_handler);
    REG("/enroll/status",   HTTP_GET,  enroll_status_handler);
    REG("/enroll/start",    HTTP_POST, enroll_start_handler);
    REG("/enroll/capture",  HTTP_POST, enroll_capture_handler);
    REG("/enroll/finish",   HTTP_POST, enroll_finish_handler);
    REG("/enroll/cancel",   HTTP_POST, enroll_cancel_handler);
    REG("/api/chat",        HTTP_POST, api_chat_handler);
    REG("/api/robot/chat",  HTTP_POST, api_chat_handler);
#if CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
    REG("/api/voice/record", HTTP_POST, api_voice_test_handler);
    REG("/api/voice/status", HTTP_GET, api_voice_status_handler);
    REG("/api/voice/last.wav", HTTP_GET, api_voice_last_wav_handler);
    REG("/api/voice/debug/slot0.wav", HTTP_GET, api_voice_debug_slot0_handler);
    REG("/api/voice/debug/slot1.wav", HTTP_GET, api_voice_debug_slot1_handler);
    REG("/api/voice/debug/slot2.wav", HTTP_GET, api_voice_debug_slot2_handler);
    REG("/api/voice/debug/slot3.wav", HTTP_GET, api_voice_debug_slot3_handler);
    REG("/api/voice/reset",  HTTP_POST, api_voice_reset_handler);
#else
    ESP_LOGI(TAG, "voice web route disabled by config");
#endif
    REG("/api/play",        HTTP_POST, api_play_handler);
    REG("/api/led/idle",    HTTP_POST, api_led_idle_handler);
    REG("/api/led/rainbow", HTTP_POST, api_led_rainbow_handler);
    REG("/api/led/speaking",    HTTP_POST, api_led_speaking_handler);
    REG("/api/led/recognized",  HTTP_POST, api_led_recognized_handler);
    REG("/api/led/error",       HTTP_POST, api_led_error_handler);
    REG("/debug/detect",        HTTP_GET,  debug_detect_handler);

#ifdef ROBOT_MOCK_MODE
    REG("/api/mock/state",  HTTP_POST, api_mock_handler);
    ESP_LOGI(TAG, "Mock mode: /api/mock/state registered");
#endif

#undef REG

    ESP_LOGI(TAG, "Routes registered:");
    ESP_LOGI(TAG, "  GET  /  /capture.jpg  /status  /people");
    ESP_LOGI(TAG, "  GET  /enroll/status");
    ESP_LOGI(TAG, "  POST /enroll/start  /capture  /finish  /cancel");
#if CONFIG_ROBOT_ENABLE_VOICE_WEB_ROUTE
    ESP_LOGI(TAG, "  POST /api/chat  /api/robot/chat  /api/voice/record  /api/voice/reset  /api/play  /api/led/{idle,rainbow,speaking,recognized,error}");
    ESP_LOGI(TAG, "  GET  /api/voice/status  /api/voice/last.wav  /api/voice/debug/slot[0-3].wav");
#else
    ESP_LOGI(TAG, "  POST /api/chat  /api/robot/chat  /api/play  /api/led/{idle,rainbow,speaking,recognized,error}");
#endif
    ESP_LOGI(TAG, "  GET  /debug/detect  (pixel-mode probe + face count)");
#ifdef ROBOT_MOCK_MODE
    ESP_LOGI(TAG, "  POST /api/mock/state  [MOCK MODE ACTIVE]");
#endif

    return ESP_OK;
}
