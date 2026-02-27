#ifndef __GATEWAY_PRIVATE__H_
#define __GATEWAY_PRIVATE__H_

extern struct mg_mgr         gw_mgr;
extern struct mg_connection *gw_mqtt_conn;

static void gw_setup_wifi();
void gw_mqtt_timer_fn(void *arg);

//RPC method
void gw_rpc_ping(struct mg_rpc_req *r);
void gw_mqtt_publish(const char *topic, const char *payload, size_t len);
void gw_mqtt_publish(const char *topic, const char *payload);

#endif