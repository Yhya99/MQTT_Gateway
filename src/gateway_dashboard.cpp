#include "gateway_dashboard.h"

// Embedded HTML dashboard (same as before)
static const char* DASHBOARD_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Gateway Dashboard</title>
    <style>
        body { font-family: Arial; margin: 20px; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
        th { background-color: #f2f2f2; }
        .pending { background-color: #fff3cd; }
        .approved { background-color: #d4edda; }
        .denied { background-color: #f8d7da; }
        button { margin: 2px; }
    </style>
</head>
<body>
    <h1>ESP32 Gateway Dashboard</h1>
    <div>
        <button onclick="listDevices()">Refresh List</button>
    </div>
    <h2>Devices</h2>
    <table id="deviceTable">
        <thead>
            <tr><th>ID</th><th>Name</th><th>Type</th><th>Status</th><th>Last Seen</th><th>Actions</th></tr>
        </thead>
        <tbody></tbody>
    </table>

    <script>
        let ws = new WebSocket('ws://' + location.host + '/ws');
        ws.onopen = function() { console.log('WebSocket connected'); listDevices(); };
        ws.onmessage = function(event) {
            let msg = JSON.parse(event.data);
            if (msg.type === 'device_list') {
                updateTable(msg.devices);
            } else if (msg.type === 'device_update') {
                listDevices(); // simple refresh
            } else if (msg.type === 'response') {
                alert(msg.cmd + ': ' + msg.status);
            }
        };

        function listDevices() {
            ws.send(JSON.stringify({ cmd: 'list_devices' }));
        }

        function approveDevice(id) {
            ws.send(JSON.stringify({ cmd: 'approve', device_id: id }));
        }

        function denyDevice(id) {
            ws.send(JSON.stringify({ cmd: 'deny', device_id: id }));
        }

        function pingDevice(id) {
            ws.send(JSON.stringify({ cmd: 'send_ping', device_id: id }));
        }

        function updateTable(devices) {
          let tbody = document.querySelector('#deviceTable tbody');
          tbody.innerHTML = '';
          devices.forEach(dev => {
              let row = tbody.insertRow();
              row.className = dev.status.toLowerCase();
              row.insertCell().textContent = dev.id;
              row.insertCell().textContent = dev.name;
              row.insertCell().textContent = dev.type;
              row.insertCell().textContent = dev.status;
              row.insertCell().textContent = new Date(dev.lastSeen).toLocaleString();
              let actions = row.insertCell();
              if (dev.status === 'PENDING') {
                  if (dev.has_pending) {
                      actions.innerHTML = `
                          <input type="text" id="psk-${dev.id}" placeholder="PSK" size="10">
                          <button onclick="authorizeDevice('${dev.id}')">Authorize</button>
                          <button onclick="denyDevice('${dev.id}')">Deny</button>
                      `;
                  } else {
                      actions.innerHTML = '<em>No request</em>';
                  }
              } else {
                  actions.innerHTML = `<button onclick="pingDevice('${dev.id}')">Ping</button>`;
              }
          });
      }

      function authorizeDevice(id) {
          let psk = document.getElementById('psk-' + id).value;
          ws.send(JSON.stringify({ cmd: 'authorize', device_id: id, psk: psk }));
      }
    </script>
</body>
</html>
)rawliteral";

DashboardServer::DashboardServer(GatewayCore& core) : m_core(core), m_httpConn(nullptr) {}

void DashboardServer::begin(int port) {
  char url[32];
  snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);
  m_httpConn = mg_http_listen(m_core.getMgr(), url, handler, this);
  if (!m_httpConn) {
    MG_ERROR(("Failed to start dashboard HTTP server"));
  } else {
    MG_INFO(("Dashboard started on port %d", port));
  }

  // Subscribe to core events
  m_core.onEvent([this](const String& id, int event) {
    const char* status = nullptr;
    auto dev = m_core.getDevice(id);
    if (dev) {
      switch (dev->status) {
        case DEV_PENDING: status = "PENDING"; break;
        case DEV_APPROVED: status = "APPROVED"; break;
        case DEV_DENIED: status = "DENIED"; break;
        default: status = "OFFLINE";
      }
    }
    broadcastDeviceUpdate(id, status ? status : "UNKNOWN");
  });
}

void DashboardServer::handler(struct mg_connection *c, int ev, void *ev_data) {
  // FIX BUG 1: The user pointer passed to mg_http_listen lives in c->fn_data.
  // ev_data is event-specific (mg_http_message*, mg_ws_message*, etc.) and
  // must NOT be cast to DashboardServer* — doing so caused the ESP32 crash.
  DashboardServer* self = (DashboardServer*)c->fn_data;

  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message*)ev_data;
    if (mg_match(hm->uri, mg_str("/"), NULL)) {
      mg_http_reply(c, 200, "Content-Type: text/html\r\n", "%s", DASHBOARD_HTML);
    } else if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
      mg_ws_upgrade(c, hm, NULL);
    } else {
      mg_http_reply(c, 404, "", "Not Found\n");
    }
  } else if (ev == MG_EV_WS_OPEN) {
    if (self) self->onWsOpen(c);
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message*)ev_data;
    if (self) self->onWsMsg(c, wm->data);
  } else if (ev == MG_EV_CLOSE) {
    if (!self) return;
    for (auto it = self->m_wsClients.begin(); it != self->m_wsClients.end(); ++it) {
      if (*it == c) { self->m_wsClients.erase(it); break; }
    }
  }
}

void DashboardServer::onWsOpen(struct mg_connection *c) {
  m_wsClients.push_back(c);
  MG_INFO(("Dashboard client connected, total %d", m_wsClients.size()));
}

void DashboardServer::onWsMsg(struct mg_connection *c, struct mg_str data) {
  char* cmd = mg_json_get_str(data, "$.cmd");
  if (!cmd) return;
  if (strcmp(cmd, "list_devices") == 0) {
    sendDeviceList(c);
  } else if (strcmp(cmd, "authorize") == 0) {
    char* devId = mg_json_get_str(data, "$.device_id");
    char* psk = mg_json_get_str(data, "$.psk");
    if (devId && psk) {
      bool ok = m_core.authorizeDevice(devId, psk);
      mg_ws_printf(c, WEBSOCKET_OP_TEXT,
        "{\"type\":\"response\",\"cmd\":\"authorize\",\"status\":\"%s\",\"device_id\":\"%s\"}",
        ok ? "ok" : "fail", devId);
      free(devId); free(psk);
    }
  } else if (strcmp(cmd, "deny") == 0) {
    char* devId = mg_json_get_str(data, "$.device_id");
    if (devId) {
      m_core.denyDevice(devId);
      mg_ws_printf(c, WEBSOCKET_OP_TEXT,
        "{\"type\":\"response\",\"cmd\":\"deny\",\"status\":\"ok\",\"device_id\":\"%s\"}",
        devId);
      free(devId);
    }
  } else if (strcmp(cmd, "send_ping") == 0) {
    char* devId = mg_json_get_str(data, "$.device_id");
    if (devId) {
      m_core.publishToDevice(devId,
        "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":{},\"id\":1}");
      mg_ws_printf(c, WEBSOCKET_OP_TEXT,
        "{\"type\":\"response\",\"cmd\":\"ping\",\"status\":\"sent\",\"device_id\":\"%s\"}",
        devId);
      free(devId);
    }
  }
  free(cmd);
}

void DashboardServer::sendDeviceList(struct mg_connection *c) {
  // FIX BUG 3: Build the entire JSON in one String then send as a single
  // WebSocket frame. The previous code called mg_ws_printf multiple times,
  // producing separate frames that the browser received and tried to
  // JSON.parse() individually — all but the last fragment would fail to parse.
  String json = "{\"type\":\"device_list\",\"devices\":[";
  bool first = true;
  for (auto& pair : m_core.getAllDevices()) {
    if (!first) json += ",";
    first = false;
    const Device& dev = pair.second;
    const char* statusStr = dev.status == DEV_PENDING  ? "PENDING"  :
                            dev.status == DEV_APPROVED ? "APPROVED" :
                            dev.status == DEV_DENIED   ? "DENIED"   : "OFFLINE";
    char entry[320];
    mg_snprintf(entry, sizeof(entry),
      "{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"status\":\"%s\","
      "\"lastSeen\":%lu,\"has_pending\":%s}",
      dev.id.c_str(), dev.name.c_str(), dev.type.c_str(), statusStr,
      dev.lastSeen, dev.has_pending ? "true" : "false");
    json += entry;
  }
  json += "]}";
  mg_ws_send(c, json.c_str(), json.length(), WEBSOCKET_OP_TEXT);
}

void DashboardServer::broadcastDeviceUpdate(const String& deviceId, const char* status) {
  for (auto client : m_wsClients) {
    mg_ws_printf(client, WEBSOCKET_OP_TEXT,
      "{\"type\":\"device_update\",\"device\":{\"id\":\"%s\",\"status\":\"%s\"}}",
      deviceId.c_str(), status);
  }
}