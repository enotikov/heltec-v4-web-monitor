#pragma once

#include <Arduino.h>
#include <Identity.h>
#include <Packet.h>

#if defined(ESP_PLATFORM)
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
#endif

#ifndef WEB_PACKET_LOG_CAPACITY
  #define WEB_PACKET_LOG_CAPACITY 128
#endif

#ifndef WEB_PACKET_NODE_CAPACITY
  #define WEB_PACKET_NODE_CAPACITY 32
#endif

#define WEB_PACKET_NODE_KEY_SIZE 8

enum class WebPacketDirection : uint8_t {
  Rx = 0,
  Tx = 1,
  TxFailed = 2,
};

struct WebPacketLogEntry {
  uint32_t sequence;
  uint32_t epoch_secs;
  uint32_t uptime_ms;
  uint16_t packet_len;
  uint16_t payload_len;
  uint16_t airtime_ms;
  uint16_t transport_codes[2];
  int16_t rssi_dbm;
  int16_t score_milli;
  int16_t snr_quarters;
  uint8_t hash[4];
  uint8_t direction;
  uint8_t payload_type;
  uint8_t route_type;
  uint8_t path_hops;
  uint8_t path_hash_size;
  uint8_t has_signal;
  uint8_t path_byte_len;
  uint8_t path[MAX_PATH_SIZE];
  uint8_t node_key[WEB_PACKET_NODE_KEY_SIZE];
  uint8_t has_verified_node;
};

struct WebPacketNode {
  mesh::Identity id;
  uint32_t last_seen;
  int32_t lat_e6;
  int32_t lon_e6;
  char name[33];
  uint8_t type;
  uint8_t has_location;
  uint8_t in_use;
};

class WebPacketLog {
public:
  WebPacketLog();
  ~WebPacketLog();

  bool begin(size_t capacity = WEB_PACKET_LOG_CAPACITY);
  void record(const mesh::Packet& packet, WebPacketDirection direction, uint32_t epoch_secs,
              uint32_t uptime_ms, int packet_len, int rssi_dbm, float snr_db, int score_milli,
              int airtime_ms, bool has_signal);
  void rememberVerifiedAdvert(const mesh::Packet& packet, const mesh::Identity& id, uint32_t timestamp,
                              uint8_t type, const char* name, bool has_location,
                              int32_t lat_e6, int32_t lon_e6);
  bool formatJson(char* reply, size_t reply_size, uint32_t since_sequence, size_t limit,
                  bool include_nodes, const mesh::Identity& self_id, const char* self_name,
                  int32_t self_lat_e6, int32_t self_lon_e6);
  bool clear();
  size_t getCapacity() const { return _capacity; }
  size_t getCount() const { return _count; }
  uint32_t getDroppedCount() const { return _dropped; }
  bool isAvailable() const { return _entries != nullptr; }

private:
  WebPacketLogEntry* _entries;
  size_t _capacity;
  size_t _count;
  size_t _head;
  uint32_t _next_sequence;
  volatile uint32_t _dropped;
  WebPacketNode _nodes[WEB_PACKET_NODE_CAPACITY];
#if defined(ESP_PLATFORM)
  SemaphoreHandle_t _mutex;
#endif

  WebPacketLog(const WebPacketLog&) = delete;
  WebPacketLog& operator=(const WebPacketLog&) = delete;
};
