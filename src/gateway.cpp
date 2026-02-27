#include "gateway.h"
#include "gateway_private.h"


struct mg_mgr         gw_mgr;
struct mg_connection *gw_mqtt_conn = nullptr;
struct mg_rpc       *gw_rpc_head  = nullptr;

static void gw_setup_wifi() {
  Serial.printf("Connecting to WiFi '%s'", GW_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(GW_WIFI_SSID, GW_WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nWiFi OK — IP: %s\n", WiFi.localIP().toString().c_str());
}

void gateway_init(){
  Serial.begin(115200);
  delay(500);
  gw_setup_wifi();
  // Initialize Mongoose event manager
  mg_mgr_init(&gw_mgr);
  
  //RPC Methods
  mg_rpc_add(&gw_rpc_head, mg_str("ping"),        gw_rpc_ping,        NULL);
  
  // MQTT reconnection timer — runs every 3s, starts immediately
  mg_timer_add(&gw_mgr, GW_MQTT_RECONNECT_MS, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, gw_mqtt_timer_fn, &gw_mgr);
}


//RPC method
void gw_rpc_ping(struct mg_rpc_req *r)
{
    mg_rpc_ok(r, "{%m:true,%m:%lu}",
    MG_ESC("pong"), MG_ESC("uptime_ms"), (unsigned long)millis());
}

// ─────────────────────────────────────────────
//  MQTT Publishing Helper
// ─────────────────────────────────────────────
void gw_mqtt_publish(const char *topic, const char *payload, size_t len) {
  if (gw_mqtt_conn == NULL) return;
  struct mg_mqtt_opts opts = {};
  opts.topic   = mg_str(topic);
  opts.message = mg_str_n(payload, len);
  opts.qos     = 1;
  opts.retain  = false;
  mg_mqtt_pub(gw_mqtt_conn, &opts);
}

void gw_mqtt_publish(const char *topic, const char *payload) {
  gw_mqtt_publish(topic, payload, strlen(payload));
}

// ─────────────────────────────────────────────
//  MQTT Event Handler
// ─────────────────────────────────────────────
void gw_mqtt_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_MQTT_OPEN) {
    MG_INFO(("MQTT connected to %s", GW_MQTT_BROKER));
    Serial.printf("MQTT connected to '%s'\n", GW_MQTT_BROKER);

    struct mg_mqtt_opts sub = {};

    sub.topic = mg_str(GW_T_GATEWAY_RX);
    sub.qos   = 1;
    mg_mqtt_sub(c, &sub);

  } else if (ev == MG_EV_MQTT_MSG) {
    struct mg_mqtt_message *mm = (struct mg_mqtt_message *)ev_data;
    #if MG_DEBUG == 1
    Serial.printf("\n[MQTT RX] %.*s <- %.100.*s%s\n",
                (int)mm->topic.len, mm->topic.buf,
                (int)(mm->data.len > 100 ? 100 : mm->data.len), mm->data.buf,
                mm->data.len > 100 ? "..." : "");
    #endif
    if (mg_match(mm->topic, mg_str(GW_T_GATEWAY_RX), NULL)) {
      gw_handle_gateway_rx(mm->data);
    }
    
  } else if (ev == MG_EV_CLOSE) {
    MG_INFO(("MQTT disconnected"));
    Serial.println("MQTT disconnected — will reconnect...");
    gw_mqtt_conn = NULL;
  }

}



void gw_handle_gateway_rx(struct mg_str payload) {
    char *deviceId = mg_json_get_str(payload, "$.device_id");
    if (!deviceId)
    {
        free(deviceId);
        return;
    }
    String devId(deviceId);
    String respTopic = "jrpc/devices/" + devId + "/rx";

    char *method = mg_json_get_str(payload, "$.method");

    if (!method) {
        free(method); free(deviceId);
        return;
      }
    
    // Dispatch via mg_rpc
    struct mg_iobuf io = {NULL, 0, 0, 256};
    struct mg_rpc_req r = {};
    r.head     = &gw_rpc_head;
    r.rpc      = NULL;
    r.pfn      = mg_pfn_iobuf;
    r.pfn_data = &io;
    r.req_data = NULL;
    r.frame    = payload;
    mg_rpc_process(&r);
    
    gw_mqtt_publish(respTopic.c_str(), (const char *)io.buf);

    free(io.buf);
    free(deviceId);
    return;
}
// ─────────────────────────────────────────────
//  MQTT Reconnection Timer
// ─────────────────────────────────────────────
void gw_mqtt_timer_fn(void *arg) {
  struct mg_mgr *m = (struct mg_mgr *)arg;

  if (gw_mqtt_conn != NULL) return;  // Already connected

  char url[128];
  mg_snprintf(url, sizeof(url), "mqtt://%s:%d", GW_MQTT_BROKER, GW_MQTT_PORT);

  char cid[64];
  mg_snprintf(cid, sizeof(cid), "%s_%lx", GW_GATEWAY_ID, (unsigned long)random(0xFFFF));

  struct mg_mqtt_opts opts = {};
  opts.client_id = mg_str(cid);
  opts.clean     = true;
  opts.keepalive = 60;
  opts.version   = 4;  // MQTT 3.1.1

  MG_INFO(("Connecting MQTT to %s...", url));
  Serial.printf("Connecting MQTT to '%s'...\n", url);
  gw_mqtt_conn = mg_mqtt_connect(m, url, &opts, gw_mqtt_ev_handler, NULL);
}

void gateway_poll() {
  mg_mgr_poll(&gw_mgr, 1);

}