#include "web_server.h"
#include "tuya_ble_client.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_system.h"

static const char *TAG = "web_server";

// Switch state and callback
static web_switch_cb_t g_switch_cb = NULL;
static bool g_switch_state = false;

// Ring buffer for logs
#define LOG_RING_SIZE 80
#define LOG_LINE_MAX 200
static char g_log_ring[LOG_RING_SIZE][LOG_LINE_MAX];
static int g_log_head = 0;
static int g_log_count = 0;

void web_server_add_log(const char* line) {
    // Strip ANSI color codes, escape JSON and HTML special chars
    char *dst = g_log_ring[g_log_head];
    int di = 0;
    for (int i = 0; line[i] && di < LOG_LINE_MAX - 2; i++) {
        if (line[i] == '\033') {
            // Skip ANSI escape until 'm'
            while (line[i] && line[i] != 'm') i++;
            continue;
        }
        if (line[i] == '\n' || line[i] == '\r') continue;
        // Escape JSON special chars (also covers HTML needs)
        if (line[i] == '"' || line[i] == '\\') {
            if (di + 2 < LOG_LINE_MAX - 1) { dst[di++] = '\\'; dst[di++] = line[i]; }
        } else if (line[i] == '<') {
            if (di + 4 < LOG_LINE_MAX - 1) { dst[di++]='&'; dst[di++]='l'; dst[di++]='t'; dst[di++]=';'; }
        } else if (line[i] == '>') {
            if (di + 4 < LOG_LINE_MAX - 1) { dst[di++]='&'; dst[di++]='g'; dst[di++]='t'; dst[di++]=';'; }
        } else {
            dst[di++] = line[i];
        }
    }
    dst[di] = '\0';

    g_log_head = (g_log_head + 1) % LOG_RING_SIZE;
    if (g_log_count < LOG_RING_SIZE) g_log_count++;
}

// GET /api/status - JSON status
static esp_err_t status_api_handler(httpd_req_t *req) {
    tuya_ble_status_t ble_status;
    tuya_ble_get_status(&ble_status);
    tuya_connection_state_t state = tuya_ble_get_state();

    wifi_ap_record_t ap_info;
    int wifi_rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi_rssi = ap_info.rssi;
    }

    // Get reset reason
    const char *reset_reason_str = "unknown";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: reset_reason_str = "power_on"; break;
        case ESP_RST_SW: reset_reason_str = "software"; break;
        case ESP_RST_PANIC: reset_reason_str = "panic"; break;
        case ESP_RST_INT_WDT: reset_reason_str = "int_wdt"; break;
        case ESP_RST_TASK_WDT: reset_reason_str = "task_wdt"; break;
        case ESP_RST_WDT: reset_reason_str = "wdt"; break;
        case ESP_RST_BROWNOUT: reset_reason_str = "brownout"; break;
        default: break;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"uptime\":%lld,\"heap\":%lu,"
        "\"wifi_rssi\":%d,"
        "\"ble_rssi\":%d,\"target_found\":%s,\"devices_seen\":%d,"
        "\"smp_paired\":%s,\"services\":%d,"
        "\"security_handle\":\"0x%04x\",\"security_verified\":%s,"
        "\"write_handle\":\"0x%04x\",\"notify_handle\":\"0x%04x\","
        "\"reset_reason\":\"%s\",\"switch_on\":%s}",
        tuya_ble_state_str(state),
        esp_timer_get_time() / 1000000,
        (unsigned long)esp_get_free_heap_size(),
        wifi_rssi,
        ble_status.last_rssi,
        ble_status.target_found ? "true" : "false",
        ble_status.devices_seen,
        ble_status.smp_paired ? "true" : "false",
        ble_status.discovered_services,
        ble_status.security_handle,
        ble_status.security_handle_verified ? "true" : "false",
        ble_status.write_handle,
        ble_status.notify_handle,
        reset_reason_str,
        g_switch_state ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, strlen(buf));
}

// GET /api/logs - last N log lines as JSON array
static esp_err_t logs_api_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "[");

    int start = (g_log_count < LOG_RING_SIZE) ? 0 : g_log_head;
    for (int i = 0; i < g_log_count; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        httpd_resp_sendstr_chunk(req, "\"");
        httpd_resp_sendstr_chunk(req, g_log_ring[idx]);
        httpd_resp_sendstr_chunk(req, "\"");
        if (i < g_log_count - 1) {
            httpd_resp_sendstr_chunk(req, ",");
        }
    }

    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// GET / - Dashboard HTML
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");

    const char *html =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Tuya BLE Bridge</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,system-ui,sans-serif;background:#0f172a;color:#e2e8f0;padding:16px}"
"h1{font-size:18px;color:#38bdf8;margin-bottom:12px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}"
".card{background:#1e293b;border-radius:8px;padding:12px}"
".card h2{font-size:12px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;margin-bottom:6px}"
".val{font-size:22px;font-weight:700}"
".sm{font-size:13px;color:#94a3b8;margin-top:4px}"
".ok{color:#4ade80}.warn{color:#fbbf24}.err{color:#f87171}.info{color:#38bdf8}"
".wide{grid-column:1/-1}"
"#log{background:#0c0c0c;border-radius:8px;padding:8px;font-family:'SF Mono',monospace;"
"font-size:11px;height:45vh;overflow-y:auto;line-height:1.5;white-space:pre-wrap;word-break:break-all}"
".log-e{color:#f87171}.log-w{color:#fbbf24}.log-i{color:#a3e635}.log-d{color:#6b7280}"
".controls{margin-bottom:12px;display:flex;gap:8px}"
".btn{background:#334155;color:#e2e8f0;border:none;padding:8px 16px;border-radius:6px;"
"font-size:13px;cursor:pointer;font-weight:600}"
".btn:hover{background:#475569}"
".btn-primary{background:#0284c7}.btn-primary:hover{background:#0369a1}"
".btn-on{background:#16a34a}.btn-on:hover{background:#15803d}"
".btn-off{background:#dc2626}.btn-off:hover{background:#b91c1c}"
".switch-state{font-size:28px;font-weight:700;margin:0 12px}"
".switch-row{display:flex;align-items:center;margin-bottom:12px;gap:8px}"
"</style></head><body>"
"<h1>Tuya BLE Bridge</h1>"
"<div class='switch-row'>"
"<button class='btn btn-on' onclick='switchCmd(\"on\")'>ON</button>"
"<span class='switch-state' id='sw_state'>-</span>"
"<button class='btn btn-off' onclick='switchCmd(\"off\")'>OFF</button>"
"</div>"
"<div class='controls'>"
"<button class='btn btn-primary' onclick='sendCmd(\"connect\")'>BLE Connect</button>"
"<button class='btn' onclick='sendCmd(\"disconnect\")'>Disconnect</button>"
"<button class='btn' onclick='el(\"log\").textContent=\"\"'>Clear Log</button>"
"</div>"
"<div class='grid'>"
"<div class='card'><h2>BLE State</h2><div class='val' id='state'>-</div>"
"<div class='sm' id='target'></div></div>"
"<div class='card'><h2>BLE RSSI</h2><div class='val' id='ble_rssi'>-</div>"
"<div class='sm' id='ble_signal'></div></div>"
"<div class='card'><h2>WiFi RSSI</h2><div class='val' id='wifi_rssi'>-</div>"
"<div class='sm' id='wifi_signal'></div></div>"
"<div class='card'><h2>System</h2><div class='val' id='uptime'>-</div>"
"<div class='sm' id='heap'></div></div>"
"<div class='card wide'><h2>GATT Details</h2>"
"<div class='sm' id='gatt'></div></div>"
"</div>"
"<div id='log'></div>"
"<script>"
"function el(id){return document.getElementById(id)}"
"function sig(r){return r>-60?'Excellent':r>-70?'Good':r>-80?'Fair':'Poor'}"
"function sigC(r){return r>-60?'ok':r>-70?'ok':r>-80?'warn':'err'}"
"function stC(s){return s=='ready'?'ok':s=='scanning'||s=='smp_pairing'||s=='discovering_services'?'info':"
"s=='disconnected'?'err':'warn'}"
"function update(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"el('state').className='val '+stC(d.state);el('state').textContent=d.state;"
"el('target').innerHTML='Target: '+(d.target_found?'<span class=ok>FOUND</span>':'<span class=err>NOT FOUND</span>')"
"+'  Scanned: '+d.devices_seen;"
"el('ble_rssi').textContent=d.ble_rssi?d.ble_rssi+' dBm':'-';"
"el('ble_rssi').className='val '+(d.ble_rssi?sigC(d.ble_rssi):'');"
"el('ble_signal').textContent=d.ble_rssi?sig(d.ble_rssi):'';"
"el('wifi_rssi').textContent=d.wifi_rssi+' dBm';"
"el('wifi_rssi').className='val '+sigC(d.wifi_rssi);"
"el('wifi_signal').textContent=sig(d.wifi_rssi);"
"var m=Math.floor(d.uptime/60),s=d.uptime%60;"
"el('uptime').textContent=m+'m '+s+'s';"
"el('heap').textContent='Free: '+(d.heap/1024).toFixed(0)+' KB';"
"el('gatt').innerHTML='SMP: '+(d.smp_paired?'<span class=ok>paired</span>':'<span class=warn>no</span>')"
"+'  Services: '+d.services"
"+'  Security: '+d.security_handle+(d.security_verified?' <span class=ok>verified</span>':' <span class=warn>hardcoded</span>')"
"+'  Write: '+d.write_handle+'  Notify: '+d.notify_handle;"
"var sw=el('sw_state');sw.textContent=d.switch_on?'ON':'OFF';"
"sw.style.color=d.switch_on?'#4ade80':'#f87171'"
"}).catch(()=>{})}"
"function loadLogs(){"
"fetch('/api/logs').then(r=>r.json()).then(lines=>{"
"var box=el('log'),html='';"
"lines.forEach(l=>{"
"var c='log-d';"
"if(l.indexOf(') E ')>0||l.indexOf('ERROR')>0)c='log-e';"
"else if(l.indexOf(') W ')>0||l.indexOf('WARN')>0)c='log-w';"
"else if(l.indexOf(') I ')>0)c='log-i';"
"html+='<div class='+c+'>'+l+'</div>'});"
"box.innerHTML=html;box.scrollTop=box.scrollHeight"
"}).catch(()=>{})}"
"function sendCmd(cmd){"
"fetch('/api/'+cmd,{method:'POST'}).then(()=>setTimeout(update,500))}"
"function switchCmd(cmd){"
"fetch('/api/switch/'+cmd,{method:'POST'}).then(()=>setTimeout(update,500))}"
"setInterval(update,2000);setInterval(loadLogs,1500);update();loadLogs()"
"</script></body></html>";

    return httpd_resp_send(req, html, strlen(html));
}

// POST /api/connect — delegates to main.c which manages reconnect state
static esp_err_t connect_handler(httpd_req_t *req) {
    extern void web_connect_callback(void);
    web_connect_callback();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/disconnect — delegates to main.c which stops auto-reconnect
static esp_err_t disconnect_handler(httpd_req_t *req) {
    extern void web_disconnect_callback(void);
    web_disconnect_callback();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/switch/on
static esp_err_t switch_on_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (g_switch_cb) {
        g_switch_cb(true);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no callback\"}");
}

// POST /api/switch/off
static esp_err_t switch_off_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (g_switch_cb) {
        g_switch_cb(false);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no callback\"}");
}

void web_server_set_switch_state(bool on) {
    g_switch_state = on;
}

esp_err_t web_server_start(web_switch_cb_t switch_cb) {
    g_switch_cb = switch_cb;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 16384;
    config.lru_purge_enable = true;  // Purge stale connections

    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting HTTP server on port %d...", config.server_port);
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "HTTP server handle: %p", server);

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t status_api = { .uri = "/api/status", .method = HTTP_GET, .handler = status_api_handler };
    httpd_uri_t logs_api = { .uri = "/api/logs", .method = HTTP_GET, .handler = logs_api_handler };
    httpd_uri_t connect_api = { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_handler };
    httpd_uri_t disconnect_api = { .uri = "/api/disconnect", .method = HTTP_POST, .handler = disconnect_handler };
    httpd_uri_t switch_on_api = { .uri = "/api/switch/on", .method = HTTP_POST, .handler = switch_on_handler };
    httpd_uri_t switch_off_api = { .uri = "/api/switch/off", .method = HTTP_POST, .handler = switch_off_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &status_api);
    httpd_register_uri_handler(server, &logs_api);
    httpd_register_uri_handler(server, &connect_api);
    httpd_register_uri_handler(server, &disconnect_api);
    httpd_register_uri_handler(server, &switch_on_api);
    httpd_register_uri_handler(server, &switch_off_api);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}
