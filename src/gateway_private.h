#ifndef __GATEWAY_PRIVATE__H_
#define __GATEWAY_PRIVATE__H_

extern struct mg_mgr         gw_mgr;
extern struct mg_connection *gw_mqtt_conn;

static void gw_setup_wifi();
void gw_mqtt_timer_fn(void *arg);
#endif