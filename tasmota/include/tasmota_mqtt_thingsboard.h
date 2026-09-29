/*
  tasmota_mqtt_thingsboard.h - ThingsBoard MQTT JSON and RPC helpers

  SPDX-License-Identifier: GPL-3.0-or-later
*/
#ifndef TASMOTA_MQTT_THINGSBOARD_H
#define TASMOTA_MQTT_THINGSBOARD_H

#if defined(USE_MQTT_AZURE_IOT) || defined(USE_MQTT_AWS_IOT) || defined(USE_MQTT_AWS_IOT_LIGHT)
#error "USE_MQTT_THINGSBOARD cannot be combined with Azure IoT or AWS IoT"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char kMqttThingsBoardTelemetry[] = "v1/devices/me/telemetry";
const char kMqttThingsBoardAttributes[] = "v1/devices/me/attributes";
const char kMqttThingsBoardRpcRequest[] = "v1/devices/me/rpc/request/";
const char kMqttThingsBoardRpcSubscribe[] = "v1/devices/me/rpc/request/+";
const char kMqttThingsBoardRpcResponse[] = "v1/devices/me/rpc/response/";

// Preserve the ID spelling while limiting it to a nonnegative signed 32-bit ID.
inline const char *MqttThingsBoardRequestId(const char *topic, const char *prefix) {
  if (!topic || strncmp(topic, prefix, strlen(prefix))) { return nullptr; }
  const char *id = topic + strlen(prefix);
  size_t length = strlen(id);
  if (!length || length > 10) { return nullptr; }
  uint32_t value = 0;
  for (size_t i = 0; i < length; i++) {
    if (id[i] < '0' || id[i] > '9') { return nullptr; }
    uint32_t digit = id[i] - '0';
    if (value > (INT32_MAX - digit) / 10) { return nullptr; }
    value = value * 10 + digit;
  }
  return id;
}

// JsonParser modifies its input and its jsmn tokens have 11-bit offsets.
// Validate without copying, allocating tokens, or changing the shared parser
// state. Limit nesting to 16 containers to bound ESP8266 stack consumption.
class MqttThingsBoardJson {
 public:
  static bool IsObject(const uint8_t *payload, size_t length, uint32_t max_depth = 16) {
    if (!payload || !length) { return false; }
    MqttThingsBoardJson json(payload, length);
    json.max_depth_ = max_depth;
    json.Space();
    if (json.cursor_ == json.end_ || *json.cursor_ != '{' || !json.Value(0)) { return false; }
    json.Space();
    return json.cursor_ == json.end_;
  }

  // Input has already passed IsObject(). Output owns the complete command before
  // ExecuteCommand or a publish can overwrite PubSubClient's receive buffer.
  static bool Command(const uint8_t *payload, size_t length, char *out, size_t capacity, size_t method_capacity) {
    MqttThingsBoardJson json(payload, length);
    const uint8_t *method = nullptr, *params = nullptr;
    size_t method_length = 0, params_length = 0;
    json.Space();
    if (!json.Take('{')) { return false; }
    json.Space();
    if (json.Take('}')) { return false; }
    do {
      json.Space();
      const uint8_t *key = json.cursor_;
      if (!json.String()) { return false; }
      char name[7];
      bool known = DecodeString(key, json.cursor_ - key, name, sizeof(name));
      json.Space();
      if (!json.Take(':')) { return false; }
      json.Space();
      const uint8_t *value = json.cursor_;
      if (!json.Value(1)) { return false; }
      if (known && !strcmp(name, "method")) {
        if (method) { return false; }
        method = value;
        method_length = json.cursor_ - value;
      } else if (known && !strcmp(name, "params")) {
        if (params) { return false; }
        params = value;
        params_length = json.cursor_ - value;
      }
      json.Space();
      if (json.Take('}')) { break; }
      if (!json.Take(',')) { return false; }
    } while (true);
    json.Space();
    if (json.cursor_ != json.end_ || !method ||
        !DecodeString(method, method_length, out, capacity)) { return false; }
    size_t used = strlen(out);
    if (!used || used >= method_capacity) { return false; }
    bool has_letter = false;
    for (size_t i = 0; i < used; i++) {
      char c = out[i];
      bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (!letter && c != '_' && !(c >= '0' && c <= '9')) { return false; }
      has_letter |= letter;
    }
    // All-digit names would underflow CommandHandler's trailing-index scan.
    if (!has_letter) { return false; }
    if (!params || (params_length == 4 && !memcmp(params, "null", 4))) { return true; }
    if (used + 2 > capacity) { return false; }
    out[used++] = ' ';
    if (*params == '"') {
      return DecodeString(params, params_length, out + used, capacity - used);
    }
    if (*params == 't' || *params == 'f') {
      if (used + 1 >= capacity) { return false; }
      out[used++] = (*params == 't') ? '1' : '0';
    } else {
      if (params_length >= capacity - used) { return false; }
      memcpy(out + used, params, params_length);
      used += params_length;
    }
    out[used] = 0;
    return true;
  }

 private:
  const uint8_t *cursor_;
  const uint8_t *end_;
  uint32_t max_depth_ = 16;

  MqttThingsBoardJson(const uint8_t *payload, size_t length) : cursor_(payload), end_(payload + length) {}

  bool Hex4(uint32_t &value) {
    value = 0;
    for (uint32_t i = 0; i < 4; i++) {
      if (cursor_ == end_) { return false; }
      uint8_t c = *cursor_++;
      uint32_t digit;
      if (c >= '0' && c <= '9') { digit = c - '0'; }
      else if (c >= 'a' && c <= 'f') { digit = c - 'a' + 10; }
      else if (c >= 'A' && c <= 'F') { digit = c - 'A' + 10; }
      else { return false; }
      value = (value << 4) | digit;
    }
    return true;
  }

  bool Unicode(uint32_t &value) {
    if (!Hex4(value)) { return false; }
    if (value >= 0xD800 && value <= 0xDBFF) {
      uint32_t low;
      if (!Take('\\') || !Take('u') || !Hex4(low) || low < 0xDC00 || low > 0xDFFF) { return false; }
      value = 0x10000 + ((value - 0xD800) << 10) + low - 0xDC00;
    } else if (value >= 0xDC00 && value <= 0xDFFF) { return false; }
    return true;
  }

  static bool DecodeString(const uint8_t *text, size_t length, char *out, size_t capacity) {
    if (length < 2 || *text != '"' || text[length - 1] != '"' || !capacity) { return false; }
    MqttThingsBoardJson json(text + 1, length - 2);
    size_t used = 0;
    while (json.cursor_ < json.end_) {
      uint32_t c = *json.cursor_++;
      if (c == '\\') {
        if (json.cursor_ == json.end_) { return false; }
        c = *json.cursor_++;
        if (c == 'u') {
          if (!json.Unicode(c)) { return false; }
          if (c >= 0x80) {
            uint32_t extra = (c < 0x800) ? 1 : (c < 0x10000) ? 2 : 3;
            if (used + extra + 1 >= capacity) { return false; }
            out[used++] = (0xFF << (7 - extra)) | (c >> (6 * extra));
            while (extra) { out[used++] = 0x80 | ((c >> (6 * --extra)) & 0x3F); }
            continue;
          }
        } else {
          const char *escapes = "\"\\/bfnrt";
          const char *found = c ? strchr(escapes, c) : nullptr;
          if (!found) { return false; }
          c = "\"\\/\b\f\n\r\t"[found - escapes];
        }
      }
      if (!c || used + 1 >= capacity) { return false; }
      out[used++] = c;
    }
    out[used] = 0;
    return true;
  }

  void Space(void) {
    while (cursor_ < end_ && (*cursor_ == ' ' || *cursor_ == '\t' || *cursor_ == '\r' || *cursor_ == '\n')) { cursor_++; }
  }

  bool Take(uint8_t c) {
    if (cursor_ == end_ || *cursor_ != c) { return false; }
    cursor_++;
    return true;
  }

  bool Digit(void) const {
    return cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9';
  }

  bool Literal(const char *text) {
    size_t length = strlen(text);
    if (static_cast<size_t>(end_ - cursor_) < length || memcmp(cursor_, text, length)) { return false; }
    cursor_ += length;
    return true;
  }

  bool String(void) {
    if (!Take('"')) { return false; }
    while (cursor_ < end_) {
      uint8_t c = *cursor_++;
      if (c == '"') { return true; }
      if (c < 0x20) { return false; }
      if (c == '\\') {
        if (cursor_ == end_) { return false; }
        c = *cursor_++;
        if (c == 'u') {
          uint32_t value;
          if (!Unicode(value)) { return false; }
        } else if (!strchr("\"\\/bfnrt", c) || !c) {
          return false;
        }
      } else if (c >= 0x80) {
        // Reject invalid UTF-8, overlong encodings, and surrogate code points.
        uint32_t extra = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 0;
        if (!extra || static_cast<size_t>(end_ - cursor_) < extra) { return false; }
        uint32_t codepoint = c & ((1 << (6 - extra)) - 1);
        for (uint32_t i = 0; i < extra; i++) {
          c = *cursor_++;
          if ((c & 0xC0) != 0x80) { return false; }
          codepoint = (codepoint << 6) | (c & 0x3F);
        }
        if ((extra == 2 && codepoint < 0x800) || (extra == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) { return false; }
      }
    }
    return false;
  }

  bool Number(void) {
    Take('-');
    if (!Take('0')) {
      if (!Digit()) { return false; }
      while (Digit()) { cursor_++; }
    }
    if (Take('.')) {
      if (!Digit()) { return false; }
      while (Digit()) { cursor_++; }
    }
    if (Take('e') || Take('E')) {
      if (!Take('+')) { Take('-'); }
      if (!Digit()) { return false; }
      while (Digit()) { cursor_++; }
    }
    return true;
  }

  bool Value(uint32_t depth) {
    Space();
    if (cursor_ == end_) { return false; }
    uint8_t c = *cursor_;
    if (c == '{' || c == '[') {
      if (depth >= max_depth_) { return false; }
      cursor_++;
      uint8_t close = (c == '{') ? '}' : ']';
      Space();
      if (Take(close)) { return true; }
      do {
        Space();
        if (c == '{') {
          if (!String()) { return false; }
          Space();
          if (!Take(':')) { return false; }
        }
        if (!Value(depth + 1)) { return false; }
        Space();
        if (Take(close)) { return true; }
      } while (Take(','));
      return false;
    }
    if (c == '"') { return String(); }
    if (c == 't') { return Literal("true"); }
    if (c == 'f') { return Literal("false"); }
    if (c == 'n') { return Literal("null"); }
    return Number();
  }
};

// Capture only during synchronous execution; the owner clears the active pointer
// before replying. The envelope is reserved from the start to avoid a second copy.
class MqttThingsBoardRpcResults {
 public:
  ~MqttThingsBoardRpcResults() { free(data_); }
  bool Empty(void) const { return count_ == 0; }

  void Append(const char *payload) {
    if (error_ || !payload) { return; }
    size_t length = strlen(payload);
    if (!MqttThingsBoardJson::IsObject(reinterpret_cast<const uint8_t*>(payload), length)) { return; }
    size_t needed = used_ + (count_ ? 1 : 0) + length + 2;
    if (needed > 8192) { error_ = "{\"error\":\"ResponseTooLarge\"}"; return; }
    char *next = static_cast<char*>(realloc(data_, needed + 1));
    if (!next) { error_ = "{\"error\":\"OutOfMemory\"}"; return; }
    data_ = next;
    if (!count_) { memcpy(data_, "{\"responses\":[", used_); }
    else { data_[used_++] = ','; }
    memcpy(data_ + used_, payload, length);
    used_ += length;
    memcpy(data_ + used_, "]}", 3);
    count_++;
  }

  const char *Payload(void) {
    if (error_) { return error_; }
    if (!count_) { return "{\"accepted\":true}"; }
    if (count_ == 1) {
      data_[used_] = 0;
      return data_ + sizeof("{\"responses\":[") - 1;
    }
    return data_;
  }

 private:
  char *data_ = nullptr;
  const char *error_ = nullptr;
  size_t used_ = sizeof("{\"responses\":[") - 1;
  size_t count_ = 0;
};

#endif  // TASMOTA_MQTT_THINGSBOARD_H
