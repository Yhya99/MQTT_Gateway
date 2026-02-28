#ifndef __GATEWAY_PRIVATE__H_
#define __GATEWAY_PRIVATE__H_

#include <Arduino.h>
#include "mongoose.h"
#include "../gateway_config.h"
#include "gateway_utils.h"
#include <WiFi.h>
#include "dashboard.h"

extern struct mg_mgr         gw_mgr;
extern struct mg_connection *gw_mqtt_conn;

// ── Device Status Enum ──────────────────────
enum DeviceStatus {
  DEV_PENDING,
  DEV_APPROVED,
  DEV_AUTHORIZED,
  DEV_DENIED,
  DEV_OFFLINE
};

struct Device {
  String id;
  String name;
  String type;
  DeviceStatus status;
  uint8_t hmacKey[32];
  uint8_t devicePubKey[32];
  bool    keyValid;
  uint32_t lastNonce;
  unsigned long firstSeen;
  unsigned long lastSeen;
  int messageCount; //log

  uint8_t pskHash[32];
  bool    pskSet;

  bool permPing;
Device() : status(DEV_PENDING), keyValid(false), lastNonce(0),
             firstSeen(0), lastSeen(0), messageCount(0),
             permPing(false), pskSet(false) {
    memset(hmacKey, 0, sizeof(hmacKey));
    memset(devicePubKey, 0, sizeof(devicePubKey));
    memset(pskHash, 0, sizeof(pskHash));
  }
};

struct DevicePerms {
  bool ping;         // matches permPing
};

static void gw_setup_wifi();
void gw_mqtt_timer_fn(void *arg);

//RPC method
void gw_rpc_ping(struct mg_rpc_req *r);
int gw_build_rpc_notification(char *buf, size_t len, const char *method,
                              const char *device_id, const char *message); 
void gw_mqtt_publish(const char *topic, const char *payload, size_t len);
void gw_mqtt_publish(const char *topic, const char *payload);

void gw_handle_gateway_rx(struct mg_str payload);

void gw_handle_gateway_connect(struct mg_str payload);

//
void gw_rpc_request_connect(struct mg_rpc_req *r);

#endif