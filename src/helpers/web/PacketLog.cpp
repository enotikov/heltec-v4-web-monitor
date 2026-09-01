#include "PacketLog.h"

#include <stdarg.h>
#include <string.h>

#if defined(ESP_PLATFORM)
  #include <esp_heap_caps.h>
#endif

namespace {

const char* directionName(uint8_t direction) {
  switch (static_cast<WebPacketDirection>(direction)) {
    case WebPacketDirection::Rx:
      return "rx";
    case WebPacketDirection::Tx:
      return "tx";
    case WebPacketDirection::TxFailed:
      return "tx_fail";
    default:
      return "unknown";
  }
}

const char* routeName(uint8_t route) {
  switch (route) {
    case ROUTE_TYPE_TRANSPORT_FLOOD:
      return "transport_flood";
    case ROUTE_TYPE_FLOOD:
      return "flood";
    case ROUTE_TYPE_DIRECT:
      return "direct";
    case ROUTE_TYPE_TRANSPORT_DIRECT:
      return "transport_direct";
    default:
      return "unknown";
  }
}

const char* payloadTypeName(uint8_t type) {
  switch (type) {
    case PAYLOAD_TYPE_REQ:
      return "request";
    case PAYLOAD_TYPE_RESPONSE:
      return "response";
    case PAYLOAD_TYPE_TXT_MSG:
      return "text";
    case PAYLOAD_TYPE_ACK:
      return "ack";
    case PAYLOAD_TYPE_ADVERT:
      return "advert";
    case PAYLOAD_TYPE_GRP_TXT:
      return "group_text";
    case PAYLOAD_TYPE_GRP_DATA:
      return "group_data";
    case PAYLOAD_TYPE_ANON_REQ:
      return "anonymous_request";
    case PAYLOAD_TYPE_PATH:
      return "path";
    case PAYLOAD_TYPE_TRACE:
      return "trace";
    case PAYLOAD_TYPE_MULTIPART:
      return "multipart";
    case PAYLOAD_TYPE_CONTROL:
      return "control";
    case PAYLOAD_TYPE_RAW_CUSTOM:
      return "raw_custom";
    default:
      return "unknown";
  }
}

bool appendJson(char* reply, size_t reply_size, size_t& offset, const char* format, ...) {
  if (reply == nullptr || offset >= reply_size) {
    return false;
  }
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(&reply[offset], reply_size - offset, format, args);
  va_end(args);
  if (written < 0 || static_cast<size_t>(written) >= reply_size - offset) {
    reply[reply_size - 1] = 0;
    return false;
  }
  offset += static_cast<size_t>(written);
  return true;
}

bool appendEscapedJsonString(char* reply, size_t reply_size, size_t& offset, const char* value) {
  if (!appendJson(reply, reply_size, offset, "\"")) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value ? value : ""); *p; ++p) {
    if (*p == '"' || *p == '\\') {
      if (!appendJson(reply, reply_size, offset, "\\%c", *p)) return false;
    } else if (*p >= 0x20) {
      if (!appendJson(reply, reply_size, offset, "%c", *p)) return false;
    }
  }
  return appendJson(reply, reply_size, offset, "\"");
}

void formatNodeKey(const uint8_t* pub_key, char out[(WEB_PACKET_NODE_KEY_SIZE * 2) + 1]) {
  for (size_t i = 0; i < WEB_PACKET_NODE_KEY_SIZE; ++i) {
    snprintf(&out[i * 2], 3, "%02X", pub_key[i]);
  }
}

}  // namespace

WebPacketLog::WebPacketLog()
    : _entries(nullptr),
      _capacity(0),
      _count(0),
      _head(0),
      _next_sequence(1),
      _dropped(0),
      _nodes{}
#if defined(ESP_PLATFORM)
      , _mutex(nullptr)
#endif
{
}

WebPacketLog::~WebPacketLog() {
#if defined(ESP_PLATFORM)
  if (_entries != nullptr) {
    heap_caps_free(_entries);
  }
  if (_mutex != nullptr) {
    vSemaphoreDelete(_mutex);
  }
#endif
}

bool WebPacketLog::begin(size_t capacity) {
#if defined(ESP_PLATFORM)
  if (_entries != nullptr) {
    return true;
  }
  if (capacity == 0) {
    return false;
  }
  _mutex = xSemaphoreCreateMutex();
  if (_mutex == nullptr) {
    return false;
  }
  const size_t bytes = capacity * sizeof(WebPacketLogEntry);
  _entries = static_cast<WebPacketLogEntry*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (_entries == nullptr) {
    _entries = static_cast<WebPacketLogEntry*>(heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT));
  }
  if (_entries == nullptr) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
    return false;
  }
  _capacity = capacity;
  return true;
#else
  (void)capacity;
  return false;
#endif
}

void WebPacketLog::record(const mesh::Packet& packet, WebPacketDirection direction, uint32_t epoch_secs,
                          uint32_t uptime_ms, int packet_len, int rssi_dbm, float snr_db, int score_milli,
                          int airtime_ms, bool has_signal) {
#if defined(ESP_PLATFORM)
  if (_entries == nullptr || _mutex == nullptr || _capacity == 0 || xSemaphoreTake(_mutex, 0) != pdTRUE) {
    ++_dropped;
    return;
  }

  WebPacketLogEntry& entry = _entries[_head];
  memset(&entry, 0, sizeof(entry));
  entry.sequence = _next_sequence++;
  entry.epoch_secs = epoch_secs;
  entry.uptime_ms = uptime_ms;
  entry.packet_len = static_cast<uint16_t>(max(0, min(packet_len, 65535)));
  entry.payload_len = packet.payload_len;
  entry.airtime_ms = static_cast<uint16_t>(max(0, min(airtime_ms, 65535)));
  entry.transport_codes[0] = packet.hasTransportCodes() ? packet.transport_codes[0] : 0;
  entry.transport_codes[1] = packet.hasTransportCodes() ? packet.transport_codes[1] : 0;
  entry.rssi_dbm = static_cast<int16_t>(rssi_dbm);
  entry.score_milli = static_cast<int16_t>(max(-32768, min(score_milli, 32767)));
  entry.snr_quarters = static_cast<int16_t>(snr_db * 4.0f);
  packet.calculatePacketHash(entry.hash);
  entry.direction = static_cast<uint8_t>(direction);
  entry.payload_type = packet.getPayloadType();
  entry.route_type = packet.getRouteType();
  entry.path_hops = packet.getPathHashCount();
  entry.path_hash_size = packet.getPathHashSize();
  entry.has_signal = has_signal ? 1 : 0;
  entry.path_byte_len = min(static_cast<uint8_t>(MAX_PATH_SIZE), packet.getPathByteLen());
  if (entry.path_byte_len > 0) {
    memcpy(entry.path, packet.path, entry.path_byte_len);
  }

  if (packet.getPayloadType() == PAYLOAD_TYPE_ADVERT && packet.payload_len >= PUB_KEY_SIZE) {
    for (size_t i = 0; i < WEB_PACKET_NODE_CAPACITY; ++i) {
      if (_nodes[i].in_use && _nodes[i].id.matches(packet.payload)) {
        memcpy(entry.node_key, _nodes[i].id.pub_key, WEB_PACKET_NODE_KEY_SIZE);
        entry.has_verified_node = 1;
        break;
      }
    }
  }

  _head = (_head + 1) % _capacity;
  if (_count < _capacity) {
    ++_count;
  }
  xSemaphoreGive(_mutex);
#else
  (void)packet;
  (void)direction;
  (void)epoch_secs;
  (void)uptime_ms;
  (void)packet_len;
  (void)rssi_dbm;
  (void)snr_db;
  (void)score_milli;
  (void)airtime_ms;
  (void)has_signal;
#endif
}

void WebPacketLog::rememberVerifiedAdvert(const mesh::Packet& packet, const mesh::Identity& id,
                                          uint32_t timestamp, uint8_t type, const char* name,
                                          bool has_location, int32_t lat_e6, int32_t lon_e6) {
#if defined(ESP_PLATFORM)
  if (_entries == nullptr || _mutex == nullptr || xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  WebPacketNode* slot = nullptr;
  for (size_t i = 0; i < WEB_PACKET_NODE_CAPACITY; ++i) {
    if (_nodes[i].in_use && _nodes[i].id.matches(id)) {
      slot = &_nodes[i];
      break;
    }
    if (!_nodes[i].in_use) {
      if (slot == nullptr) slot = &_nodes[i];
    }
  }
  if (slot != nullptr) {
    const bool is_new_node = !slot->in_use;
    if (is_new_node) {
      memset(slot, 0, sizeof(*slot));
    }
    slot->id = id;
    slot->last_seen = timestamp;
    slot->type = type;
    slot->in_use = 1;
    if (has_location) {
      slot->lat_e6 = lat_e6;
      slot->lon_e6 = lon_e6;
      slot->has_location = 1;
    }
    if (name != nullptr && name[0] != 0) {
      strncpy(slot->name, name, sizeof(slot->name) - 1);
      slot->name[sizeof(slot->name) - 1] = 0;
    }

    uint8_t packet_hash[4];
    packet.calculatePacketHash(packet_hash);
    for (size_t i = 0; i < _count; ++i) {
      WebPacketLogEntry& entry = _entries[(_head + _capacity - 1 - i) % _capacity];
      if (memcmp(entry.hash, packet_hash, sizeof(packet_hash)) == 0 &&
          entry.payload_type == PAYLOAD_TYPE_ADVERT) {
        memcpy(entry.node_key, id.pub_key, WEB_PACKET_NODE_KEY_SIZE);
        entry.has_verified_node = 1;
        break;
      }
    }
  }
  xSemaphoreGive(_mutex);
#else
  (void)packet; (void)id; (void)timestamp; (void)type; (void)name;
  (void)has_location; (void)lat_e6; (void)lon_e6;
#endif
}

bool WebPacketLog::formatJson(char* reply, size_t reply_size, uint32_t since_sequence, size_t limit,
                              bool include_nodes, const mesh::Identity& self_id, const char* self_name,
                              int32_t self_lat_e6, int32_t self_lon_e6) {
#if defined(ESP_PLATFORM)
  if (reply == nullptr || reply_size == 0 || _entries == nullptr || _mutex == nullptr) {
    return false;
  }
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }

  const size_t emit_limit = min(limit == 0 ? static_cast<size_t>(64) : limit, _capacity);
  const uint32_t latest_sequence = _next_sequence > 1 ? _next_sequence - 1 : 0;
  size_t offset = 0;
  bool ok = appendJson(reply, reply_size, offset,
                       "{\"capacity\":%u,\"count\":%u,\"latest_seq\":%lu,\"dropped\":%lu",
                       static_cast<unsigned>(_capacity), static_cast<unsigned>(_count),
                       static_cast<unsigned long>(latest_sequence), static_cast<unsigned long>(_dropped));

  if (ok && include_nodes) {
    char self_key[(WEB_PACKET_NODE_KEY_SIZE * 2) + 1];
    formatNodeKey(self_id.pub_key, self_key);
    ok = appendJson(reply, reply_size, offset, ",\"self\":{\"key\":\"%s\",\"name\":", self_key);
    if (ok) ok = appendEscapedJsonString(reply, reply_size, offset, self_name);
    if (ok) ok = appendJson(reply, reply_size, offset, ",\"lat\":%.6f,\"lon\":%.6f},\"nodes\":[",
                            static_cast<double>(self_lat_e6) / 1000000.0,
                            static_cast<double>(self_lon_e6) / 1000000.0);
    bool first_node = true;
    for (size_t i = 0; ok && i < WEB_PACKET_NODE_CAPACITY; ++i) {
      const WebPacketNode& node = _nodes[i];
      if (!node.in_use) continue;
      char node_key[(WEB_PACKET_NODE_KEY_SIZE * 2) + 1];
      formatNodeKey(node.id.pub_key, node_key);
      ok = appendJson(reply, reply_size, offset, "%s{\"key\":\"%s\",\"name\":",
                      first_node ? "" : ",", node_key);
      if (ok) ok = appendEscapedJsonString(reply, reply_size, offset, node.name);
      if (ok) ok = appendJson(reply, reply_size, offset,
                              ",\"type\":%u,\"last_seen\":%lu,\"has_location\":%s",
                              static_cast<unsigned>(node.type), static_cast<unsigned long>(node.last_seen),
                              node.has_location ? "true" : "false");
      if (ok && node.has_location) {
        ok = appendJson(reply, reply_size, offset, ",\"lat\":%.6f,\"lon\":%.6f",
                        static_cast<double>(node.lat_e6) / 1000000.0,
                        static_cast<double>(node.lon_e6) / 1000000.0);
      }
      if (ok) ok = appendJson(reply, reply_size, offset, "}");
      first_node = false;
    }
    if (ok) ok = appendJson(reply, reply_size, offset, "]");
  }
  if (ok) ok = appendJson(reply, reply_size, offset, ",\"entries\":[");

  size_t emitted = 0;
  for (size_t i = 0; ok && i < _count && emitted < emit_limit; ++i) {
    const size_t index = (_head + _capacity - 1 - i) % _capacity;
    const WebPacketLogEntry& entry = _entries[index];
    if (entry.sequence <= since_sequence) {
      break;
    }
    char hash[9];
    snprintf(hash, sizeof(hash), "%02X%02X%02X%02X", entry.hash[0], entry.hash[1], entry.hash[2], entry.hash[3]);
    ok = appendJson(reply, reply_size, offset,
                    "%s{\"seq\":%lu,\"ts\":%lu,\"uptime_ms\":%lu,\"direction\":\"%s\","
                    "\"type\":%u,\"type_name\":\"%s\",\"route\":\"%s\",\"len\":%u,"
                    "\"payload_len\":%u,\"hops\":%u,\"path_hash_size\":%u,\"hash\":\"%s\","
                    "\"airtime_ms\":%u,\"score\":%d,\"transport\":[%u,%u]",
                    emitted == 0 ? "" : ",", static_cast<unsigned long>(entry.sequence),
                    static_cast<unsigned long>(entry.epoch_secs), static_cast<unsigned long>(entry.uptime_ms),
                    directionName(entry.direction), static_cast<unsigned>(entry.payload_type),
                    payloadTypeName(entry.payload_type), routeName(entry.route_type),
                    static_cast<unsigned>(entry.packet_len), static_cast<unsigned>(entry.payload_len),
                    static_cast<unsigned>(entry.path_hops), static_cast<unsigned>(entry.path_hash_size), hash,
                    static_cast<unsigned>(entry.airtime_ms), static_cast<int>(entry.score_milli),
                    static_cast<unsigned>(entry.transport_codes[0]), static_cast<unsigned>(entry.transport_codes[1]));
    if (ok && entry.has_signal) {
      ok = appendJson(reply, reply_size, offset, ",\"rssi\":%d,\"snr\":%.2f",
                      static_cast<int>(entry.rssi_dbm), static_cast<double>(entry.snr_quarters) / 4.0);
    }
    if (ok && entry.has_verified_node) {
      char node_key[(WEB_PACKET_NODE_KEY_SIZE * 2) + 1];
      formatNodeKey(entry.node_key, node_key);
      ok = appendJson(reply, reply_size, offset, ",\"node_key\":\"%s\",\"node_confidence\":\"verified\"", node_key);
    }
    if (ok && entry.path_hops > 0) {
      ok = appendJson(reply, reply_size, offset, ",\"path_nodes\":[");
      bool first_match = true;
      for (uint8_t hop = 0; ok && hop < entry.path_hops; ++hop) {
        const uint8_t* hash = &entry.path[hop * entry.path_hash_size];
        const WebPacketNode* match = nullptr;
        bool ambiguous = false;
        for (size_t n = 0; n < WEB_PACKET_NODE_CAPACITY; ++n) {
          if (!_nodes[n].in_use || !_nodes[n].has_location ||
              !_nodes[n].id.isHashMatch(hash, entry.path_hash_size)) continue;
          if (match != nullptr) { ambiguous = true; break; }
          match = &_nodes[n];
        }
        if (match != nullptr && !ambiguous) {
          char node_key[(WEB_PACKET_NODE_KEY_SIZE * 2) + 1];
          formatNodeKey(match->id.pub_key, node_key);
          ok = appendJson(reply, reply_size, offset, "%s\"%s\"", first_match ? "" : ",", node_key);
          first_match = false;
        }
      }
      if (ok) ok = appendJson(reply, reply_size, offset, "]");
    }
    if (ok) {
      ok = appendJson(reply, reply_size, offset, "}");
    }
    if (ok) {
      ++emitted;
    }
  }

  if (ok) {
    ok = appendJson(reply, reply_size, offset, "]}");
  }
  xSemaphoreGive(_mutex);
  return ok;
#else
  (void)reply;
  (void)reply_size;
  (void)since_sequence;
  (void)limit;
  (void)include_nodes; (void)self_id; (void)self_name; (void)self_lat_e6; (void)self_lon_e6;
  return false;
#endif
}

bool WebPacketLog::clear() {
#if defined(ESP_PLATFORM)
  if (_entries == nullptr || _mutex == nullptr || xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    return false;
  }
  memset(_entries, 0, _capacity * sizeof(WebPacketLogEntry));
  _count = 0;
  _head = 0;
  _dropped = 0;
  xSemaphoreGive(_mutex);
  return true;
#else
  return false;
#endif
}
