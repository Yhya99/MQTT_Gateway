#include "gateway.h"
#include "gateway_private.h"
#include "gateway_utils.h"
#include "../gateway_config.h"
#include <WiFi.h>
#include "mongoose.h"
#include "dashboard.h"

struct mg_mgr         gw_mgr;
struct mg_connection *gw_mqtt_conn = nullptr;

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
  // MQTT reconnection timer — runs every 3s, starts immediately
  mg_timer_add(&gw_mgr, GW_MQTT_RECONNECT_MS, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, gw_mqtt_timer_fn, &gw_mgr);
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