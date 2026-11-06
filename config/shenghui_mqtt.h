#pragma once
// ============================================================
// 昇辉开关 → Home Assistant MQTT Discovery
// ------------------------------------------------------------
// ESPHome 实体是编译期静态的，网页里动态录入的开关不会自动变成 HA 实体。
// 这里用官方 esphome::mqtt::CustomMQTTDevice 绕开实体系统：
//   - 录入 / 改名 / 删除时，把该槽位的 button + event 配置以 retain
//     消息发到 homeassistant/{button,event}/<object_id>/config；
//   - 设备连上 broker 时遍历 1..next_id-1 全量重发，对已删除 / 未录入
//     的 ID 发空 payload，保证重启后删掉的实体不会自己长回来；
//   - 订阅 <prefix>/shenghui/+/command，HA 按下按钮时重放对应槽位的帧。
//
// slot 参数一律是 1-based 槽位 ID（slot_store.h），与 MQTT topic 编号、
// HA 实体编号一致；ID 删除后不复用，因此实体绑定永远稳定。
//
// esp32.yaml 里 mqtt: 的 discovery: false 很关键：它让 ESPHome 不再给
// 现有风扇 / 沙发 / 文本传感器发 discovery，避免 HA 出现重复实体。
//
// 本文件由 includes: 注入 main.cpp 顶部，早于所有 id(...) 对象声明，
// 因此不直接引用 id(...)：射频对象由 yaml 的 lambda 通过 set_trigger 传入。
// ============================================================
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include "esphome.h"
#include "esphome/components/mqtt/custom_mqtt_device.h"
#include "slot_store.h"
#include "shenghui_state.h"

namespace shenghui_mqtt {

constexpr const char *DISCOVERY_PREFIX = "homeassistant";
constexpr const char *NODE_ID = "living_room_rf_hub";
constexpr const char *TOPIC_PREFIX = "living-room-rf-hub";
constexpr const char *DEVICE_NAME = "客厅 RF 中枢";
// HA → 设备：8 个假状态的全量快照（retain）
constexpr const char *STATE_ALL_TOPIC = "living-room-rf-hub/shenghui/state/all";
// 设备 → HA：首页状态胶囊的对表请求，payload 形如 "5:on"（非 retain）
constexpr const char *STATE_SET_TOPIC = "living-room-rf-hub/shenghui/state/set";

// slot 是 1-based ID；旧版这里写的是 slot+1（旧参数为 0-based 下标），
// 迁移后 ID 1..8 与旧 topic 编号逐一对应，HA 实体 ID 保持不变。
inline std::string object_id(int slot) {
  return std::string(NODE_ID) + "_shenghui_" + std::to_string(slot);
}

inline std::string config_topic(int slot) {
  return std::string(DISCOVERY_PREFIX) + "/button/" + object_id(slot) + "/config";
}

inline std::string command_topic(int slot) {
  return std::string(TOPIC_PREFIX) + "/shenghui/" + std::to_string(slot) + "/command";
}

// 物理按键（RF 接收）事件：每次按下发一条非 retain 消息，HA 的 event 实体
// 会更新 state 并产生 state_changed，自动化每按一次都能触发。
inline std::string event_object_id(int slot) {
  return object_id(slot) + "_press";
}

inline std::string event_config_topic(int slot) {
  return std::string(DISCOVERY_PREFIX) + "/event/" + event_object_id(slot) + "/config";
}

inline std::string event_topic(int slot) {
  return std::string(TOPIC_PREFIX) + "/shenghui/" + std::to_string(slot) + "/event";
}

// JSON 字符串转义（UTF-8 中文原样保留）
inline std::string json_escape(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[7];
          snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// HA MQTT button 的 discovery 配置。object_id / unique_id 固定用英文，
// 显示名取网页里填的名字；这样改名只更新 name，entity_id 保持稳定。
inline std::string config_payload(const std::string &name, int slot) {
  std::string id = object_id(slot);
  std::string s = "{\"name\":\"" + json_escape(name) + "\"";
  s += ",\"object_id\":\"" + id + "\"";
  s += ",\"unique_id\":\"" + id + "\"";
  s += ",\"command_topic\":\"" + command_topic(slot) + "\"";
  s += ",\"payload_press\":\"PRESS\"";
  s += ",\"availability_topic\":\"" + std::string(TOPIC_PREFIX) + "/status\"";
  s += ",\"payload_available\":\"online\"";
  s += ",\"payload_not_available\":\"offline\"";
  s += ",\"device\":{\"identifiers\":[\"" + std::string(NODE_ID) + "\"],";
  s += "\"name\":\"" + std::string(DEVICE_NAME) + "\",\"manufacturer\":\"ESPHome\"}";
  s += "}";
  return s;
}

// HA MQTT event 实体：state_topic 收到 {"event_type":"press"} 就触发一次，
// state 变成新时间戳，因此连续按同一个开关也会逐次产生 state_changed。
// HA 端要求 payload 是带 event_type 字段的 JSON，且会忽略 retain 消息。
inline std::string event_config_payload(const std::string &name, int slot) {
  std::string id = event_object_id(slot);
  std::string s = "{\"name\":\"" + json_escape(name + " 按下") + "\"";
  s += ",\"object_id\":\"" + id + "\"";
  s += ",\"unique_id\":\"" + id + "\"";
  s += ",\"state_topic\":\"" + event_topic(slot) + "\"";
  s += ",\"event_types\":[\"press\"]";
  s += ",\"availability_topic\":\"" + std::string(TOPIC_PREFIX) + "/status\"";
  s += ",\"payload_available\":\"online\"";
  s += ",\"payload_not_available\":\"offline\"";
  s += ",\"device\":{\"identifiers\":[\"" + std::string(NODE_ID) + "\"],";
  s += "\"name\":\"" + std::string(DEVICE_NAME) + "\",\"manufacturer\":\"ESPHome\"}";
  s += "}";
  return s;
}

class DiscoveryDevice : public esphome::mqtt::CustomMQTTDevice {
 public:
  using TriggerFn = std::function<void(int)>;

  void set_trigger(TriggerFn fn) { trigger_ = std::move(fn); }

  // mqtt: on_connect 里调用。MQTTClientComponent 重连时自动重新订阅，
  // 所以这里只需注册一次。
  void begin() {
    if (subscribed_) return;
    subscribed_ = true;
    this->subscribe(std::string(TOPIC_PREFIX) + "/shenghui/+/command",
                    &DiscoveryDevice::on_command);
    // 每次订阅（含 MQTT 重连）broker 都会立刻补发最后一条 retain 消息，
    // 因此设备重启后假状态能自动恢复，不需要 HA 再主动推一次。
    this->subscribe(std::string(STATE_ALL_TOPIC), &DiscoveryDevice::on_state_all);
  }

  // mqtt: on_connect 是底层 MQTT 库的回调，触发时 MQTTClientComponent::state_
  // 还没置成 CONNECTED，is_connected() 为 false，publish()/subscribe() 会被
  // 直接丢弃。这里推迟 300ms 到主循环执行，确保连接状态已就绪。
  void schedule_full_sync() {
    esphome::App.scheduler.set_timeout(&this->sync_tag_, 300, [this]() {
      this->begin();
      this->publish_all();
    });
  }

  // 遍历所有曾分配过的 ID：已录入的发 config，已删除 / 未录入的发空
  // payload，清掉 broker 里可能残留的旧实体。
  void publish_all() {
    if (!this->is_connected()) return;
    uint32_t until = slot_store::next_id();
    for (uint32_t id = 1; id < until; id++) {
      this->publish_slot(static_cast<int>(id));
    }
    ESP_LOGI("shenghui_mqtt", "HA 实体全量同步完成：已扫描 %u 个 ID",
             static_cast<unsigned>(until - 1));
  }

  // 已录入 → 发 button + event 两份 config；否则发两份空 payload 让 HA 移除。
  // 未连上 broker 时直接返回，下一次 on_connect 会全量补发。
  void publish_slot(int slot) {
    if (!this->is_connected() || slot < 1) return;
    if (!slot_store::used(static_cast<uint32_t>(slot))) {
      this->publish(config_topic(slot), "", 0, true);
      this->publish(event_config_topic(slot), "", 0, true);
      return;
    }
    std::string name = slot_store::name_of(static_cast<uint32_t>(slot));
    this->publish(config_topic(slot), config_payload(name, slot), 0, true);
    this->publish(event_config_topic(slot), event_config_payload(name, slot), 0, true);
    ESP_LOGI("shenghui_mqtt", "HA 实体已发布：槽位 %d = %s", slot, name.c_str());
  }

  // 物理开关按下时调用：非 retain 消息，HA event 实体每次都会更新 state
  // 并产生 state_changed，自动化不会因为「状态没变」而漏触发。
  void publish_press(int slot) {
    if (!this->is_connected() || slot < 1) return;
    // 先本地乐观翻转：物理按压不必等「HA 翻转 → retain 广播 → 设备订阅 →
    // 前端轮询」这一整圈，首页圆点下一帧就能反映。HA 的全量 state/all
    // 是绝对值，稍后会覆盖成最终一致值，不会造成二次翻转。
    bool next = rf_state::flip(static_cast<uint32_t>(slot));
    this->publish(event_topic(slot), "{\"event_type\":\"press\"}", 0, false);
    ESP_LOGI("shenghui_mqtt", "按下事件已发布：槽位 %d（本地状态先翻为 %s）",
             slot, next ? "开" : "关");
  }

  // .35 首页状态胶囊：只请求 HA 修改假状态，绝不发射 RF 信号。
  // 乐观更新本地缓存让页面立即反馈；HA 收到后会再发一次全量 state/all
  // 作为最终一致值。按 slot 用独立 scheduler tag，连续点多个不会互相取消。
  void publish_state_set(int slot, bool on) {
    if (slot < 1) return;
    std::string payload = std::to_string(slot) + (on ? ":on" : ":off");
    auto &tag = this->state_tags_[slot];
    esphome::App.scheduler.set_timeout(&tag, 0, [this, slot, on, payload]() {
      rf_state::set(static_cast<uint32_t>(slot), on);
      if (!this->is_connected()) return;
      this->publish(STATE_SET_TOPIC, payload, 0, false);
      ESP_LOGI("shenghui_mqtt", "状态对表已发送：槽位 %d → %s", slot, on ? "开" : "关");
    });
  }

 protected:
  void on_state_all(const std::string &topic, const std::string &payload) {
    rf_state::parse(payload);
    ESP_LOGI("shenghui_mqtt", "HA 假状态已同步：%s", payload.c_str());
  }

  void on_command(const std::string &topic, const std::string &payload) {
    if (payload != "PRESS" || !trigger_) return;
    // topic: living-room-rf-hub/shenghui/<ID>/command
    const std::string marker = "/shenghui/";
    auto p = topic.rfind(marker);
    if (p == std::string::npos) return;
    int slot = atoi(topic.c_str() + p + marker.size());
    if (!slot_store::used(static_cast<uint32_t>(slot))) return;
    // RF 发射带 delay，交给 ESPHome 主循环执行（MQTT 回调栈很小）
    esphome::App.scheduler.set_timeout(&tags_[slot], 0,
                                       [this, slot]() { trigger_(slot); });
  }

  TriggerFn trigger_;
  bool subscribed_{false};
  std::map<uint32_t, int> tags_;  // 每个 ID 独立的 scheduler 去重 tag
  std::map<int, int> state_tags_;  // 状态对表按 slot 去重
  int sync_tag_{0};               // on_connect 全量同步的 scheduler 去重 tag
};

inline DiscoveryDevice &device() {
  static DiscoveryDevice d;
  return d;
}

}  // namespace shenghui_mqtt
