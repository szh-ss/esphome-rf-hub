#pragma once
// ============================================================
// 昇辉自发电开关：CC1101 433.3MHz / 250k 2-FSK
//
// 分工：
//   - 槽位数据（帧 + 名称）由 slot_store.h 存在变长 NVS blob 里，
//     槽位数量无固定上限，ID 单调递增且删除后不复用。
//   - 其余运行时状态放在本文件的 runtime() 里，上电复位、不持久化。
//   - 所有函数都不直接引用 id(...)：本文件由 includes: 注入到
//     main.cpp 顶部，早于 ESPHome 对象声明，对象一律通过参数传入。
//
// 槽位名称一律取自 slot_store；未命名时显示中性的「开关 N」（N=槽位 ID）。
// 双控：一个槽位可绑多个副码（slot_store 的 aliases blob），命中任一码
// 都按主槽位 ID 上报，HA 侧实体、topic、自动化完全不用改。
// 射频逻辑与原 esp32.yaml 内联 lambda 逐行等价。
// ============================================================
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "slot_store.h"

namespace shenghui {

using Frame = std::array<uint8_t, slot_store::FRAME_LEN>;

// 运行时状态，函数内 static 保证整个固件只有一份实例
// 学习期间的候选帧：CC1101 在无信号时会持续收到随机噪声包
// （实测 RSSI -77~-100、约每 0.5~2 秒一个）。原实现的「连续两帧相同」
// 会被夹在中间的噪声不断覆盖，导致永远学不进去。这里改成 3 秒窗口内
// 收集候选，只要有一帧重复出现（允许 1 字节误码）就确认——一次按压
// 会连发多帧相同码，而噪声帧彼此各不相同，不会误触发。
struct LearnCandidate {
  Frame frame{};
  float rssi{0};
  uint32_t ms{0};
};

struct Runtime {
  uint32_t learn_id = 0;        // 正在学习的新槽位 ID（1-based），0 = 空闲
  uint32_t learn_start = 0;
  uint32_t alias_learn_id = 0;  // 正在为哪个主槽位学习副码，0 = 空闲
  uint32_t alias_learn_start = 0;
  std::vector<LearnCandidate> learn_win;
  // 每个槽位独立去抖：一次按压会连发多帧（含松开帧），300ms 内的后续帧
  // 全部忽略；不同槽位互不影响，按完 A 立刻按 B 不会再被吃掉。
  std::map<uint32_t, uint32_t> last_detect;
  // 网页「触发」发射后只屏蔽该槽位短时间内的自收帧，别的开关照常响应。
  std::map<uint32_t, uint32_t> last_tx;
};

inline Runtime &runtime() {
  static Runtime r;
  return r;
}

// ---------------- 最近收到的射频帧（内存环形缓冲，用于诊断） ----------------
// 不持久化，重启即清空；页面 /sw/api?action=list 会把它一并返回。
struct RxRecord {
  uint32_t ms{0};
  uint8_t head0{0}, head1{0};   // x[0], x[1]：正常帧是 A4 23，便于分辨协议
  Frame frame{};
  float rssi{0};
  bool usable{false};
};
constexpr size_t RX_LOG_CAP = 12;

inline std::vector<RxRecord> &rx_log() {
  static std::vector<RxRecord> v;
  return v;
}

// 同一串重复帧（150ms 内）只刷新最后一条；返回 true 表示这是一条新记录，
// 调用方据此限流日志，避免一次按压刷出十几行。
inline bool rx_push(const std::vector<uint8_t> &x, float rssi, bool usable) {
  const uint8_t *f = &x[2];
  auto &v = rx_log();
  uint32_t now = millis();
  if (!v.empty() && now - v.back().ms < 150) {
    v.back().ms = now;
    v.back().rssi = rssi;
    v.back().usable = usable;
    v.back().head0 = x[0];
    v.back().head1 = x[1];
    for (int i = 0; i < slot_store::FRAME_LEN; i++) v.back().frame[i] = f[i];
    return false;
  }
  RxRecord r;
  r.ms = now;
  r.rssi = rssi;
  r.usable = usable;
  r.head0 = x[0];
  r.head1 = x[1];
  for (int i = 0; i < slot_store::FRAME_LEN; i++) r.frame[i] = f[i];
  v.push_back(r);
  if (v.size() > RX_LOG_CAP) v.erase(v.begin());
  return true;
}

inline std::string frame_hex(const uint8_t *f) {
  std::string out;
  char b[4];
  for (int i = 0; i < slot_store::FRAME_LEN; i++) {
    snprintf(b, sizeof(b), "%02X ", f[i]);
    out += b;
  }
  if (!out.empty()) out.pop_back();
  return out;
}

// 学习时的宽松过滤：只排除全 0 / 全 FF。双控的第二个开关可能是松开帧
// 或长帧，用监听时的严格过滤会把它们挡掉，导致永远学不进去。
inline bool frame_usable(const uint8_t *f) {
  bool all0 = true, allF = true;
  for (int i = 0; i < slot_store::FRAME_LEN; i++) {
    if (f[i] != 0x00) all0 = false;
    if (f[i] != 0xFF) allF = false;
  }
  return !(all0 || allF);
}

// 有效性过滤（与原 Arduino 固件 isPressFrame 一致）
inline bool valid_press_frame(const uint8_t *f) {
  bool all0 = true, allF = true;
  for (int i = 0; i < slot_store::FRAME_LEN; i++) {
    if (f[i] != 0x00) all0 = false;
    if (f[i] != 0xFF) allF = false;
  }
  if (all0 || allF) return false;               // 全 0 / 全 FF
  if (f[4] == 0xC0) return false;               // 松开帧
  if (f[0] == 0x01 && f[1] == 0x0C) return false;  // 长帧
  return true;
}

// ---------------- 触发按钮：按槽位 ID 重放已录入的帧 ----------------
// 与原 yaml 的 repeat(4) + send_packet + delay(10ms) + begin_rx 等价
template <typename RadioT>
void trigger(RadioT *radio, int slot) {
  slot_store::Entry *e = slot_store::find(static_cast<uint32_t>(slot));
  if (e == nullptr || !e->recorded()) {
    ESP_LOGW("shenghui", "槽位 %d 未录入，先点「添加昇辉控股开关」", slot);
    return;
  }

  // 先拷贝一份：发射过程带 delay(10)，期间 MQTT / HTTP 回调可能改动
  // slot_store 的 vector，不能一直持有其内部元素的引用。
  Frame fr = e->frame;
  std::vector<uint8_t> d;
  d.push_back(0xA4);
  d.push_back(0x23);
  for (auto b : fr) d.push_back(b);

  for (int i = 0; i < 4; i++) {
    radio->transmit_packet(d);
    delay(10);
  }
  radio->begin_rx();
  runtime().last_tx[static_cast<uint32_t>(slot)] = millis();
}

// ---------------- CC1101 收包：学习状态机 / 开关检测 ----------------
// notify(slot)：录入成功后同步该槽位的 HA MQTT discovery。
// press(slot)：每次检测到物理开关按下时发布 HA 事件，供自动化重复触发。
// slot 参数统一是 1-based 槽位 ID，直接对应 MQTT topic 与 HA 实体编号。
template <typename StatusT, typename NotifyT, typename PressT>
void on_packet(const std::vector<uint8_t> &x, float rssi, StatusT *status,
               NotifyT notify, PressT press) {
  Runtime &st = runtime();
  if (x.size() < 10) return;
  const uint8_t *f = &x[2];   // 同步 54 21 之后是 A4 23 + 8 字节码
  const bool usable = frame_usable(f);
  const bool strict = valid_press_frame(f);
  const bool fresh = rx_push(x, rssi, usable);

  // 学习流程（30 秒超时，3 秒窗口内出现重复帧即确认）
  // 两种互斥模式：learn_id = 录入新槽位；alias_learn_id = 给已有槽位加副码。
  const bool learning = st.learn_id != 0 || st.alias_learn_id != 0;
  if (learning) {
    if (!usable) return;
    if (fresh) {
      ESP_LOGI("shenghui", "学习收到帧 %02X %02X | %s RSSI=%.1f strict=%s",
               x[0], x[1], frame_hex(f).c_str(), rssi, strict ? "yes" : "no");
    }
    uint32_t start = st.learn_id != 0 ? st.learn_start : st.alias_learn_start;
    if (millis() - start > 30000) {
      if (st.learn_id != 0) slot_store::release_unlearned(st.learn_id);
      st.learn_id = 0;
      st.alias_learn_id = 0;
      st.learn_win.clear();
      ESP_LOGW("shenghui", "学习超时，已退出");
      status->publish_state("学习超时");
      return;
    }

    uint32_t now = millis();
    st.learn_win.erase(
        std::remove_if(st.learn_win.begin(), st.learn_win.end(),
                       [now](const LearnCandidate &c) { return now - c.ms > 3000; }),
        st.learn_win.end());

    Frame chosen{};
    bool confirmed = false;
    for (const auto &c : st.learn_win) {
      if (slot_store::frame_diff(c.frame.data(), f) <= 1) {
        // 取两帧里信号更强的一帧作为录入依据
        if (rssi >= c.rssi) {
          for (int i = 0; i < slot_store::FRAME_LEN; i++) chosen[i] = f[i];
        } else {
          chosen = c.frame;
        }
        confirmed = true;
        break;
      }
    }
    if (!confirmed) {
      LearnCandidate c;
      c.ms = now;
      c.rssi = rssi;
      for (int i = 0; i < slot_store::FRAME_LEN; i++) c.frame[i] = f[i];
      st.learn_win.push_back(c);
      if (st.learn_win.size() > 16) st.learn_win.erase(st.learn_win.begin());
      return;
    }
    st.learn_win.clear();
    const uint8_t *cf = chosen.data();
    ESP_LOGI("shenghui", "帧已确认：%s", frame_hex(cf).c_str());

    if (st.learn_id != 0) {
      // 新槽位：写主码并同步 HA discovery
      uint32_t id = st.learn_id;
      st.learn_id = 0;
      if (!slot_store::set_frame(id, cf)) {
        ESP_LOGE("shenghui", "槽位 %u 写入 NVS 失败，录入未保存", static_cast<unsigned>(id));
        status->publish_state("写入失败，请重试");
        return;
      }
      std::string nm = slot_store::name_of(id);
      ESP_LOGI("shenghui", "### 已录入%s（ID=%u，帧 %s）", nm.c_str(),
               static_cast<unsigned>(id), frame_hex(cf).c_str());
      status->publish_state(std::string("已录入 ") + nm);
      notify(static_cast<int>(id));
    } else {
      // 副码：只写 aliases blob，不分配 ID、不发 discovery，HA 无感
      uint32_t owner = st.alias_learn_id;
      st.alias_learn_id = 0;
      std::string nm = slot_store::name_of(owner);
      std::string conflict = slot_store::alias_conflict(cf);
      if (!conflict.empty()) {
        ESP_LOGW("shenghui", "副码未添加：%s（帧 %s）", conflict.c_str(), frame_hex(cf).c_str());
        status->publish_state(conflict);
      } else if (!slot_store::alias_add(owner, cf)) {
        ESP_LOGE("shenghui", "槽位 %u 的副码写入 NVS 失败", static_cast<unsigned>(owner));
        status->publish_state("副码写入失败，请重试");
      } else {
        ESP_LOGI("shenghui", "### 已为%s添加副码（主槽位 ID=%u，帧 %s）",
                 nm.c_str(), static_cast<unsigned>(owner), frame_hex(cf).c_str());
        status->publish_state(std::string("已为") + nm + "添加副码");
      }
    }
    return;
  }

  // 监听：忽略自己刚发的信号；只用基本过滤，副码可能是被严格过滤掉的帧型。
  // 去抖改成按槽位 300ms：吃掉一次按压的重复帧 / 松开帧，但不再让
  // 「1 秒内按第二个开关」「快速连按同一开关」被丢掉。
  if (!usable) return;

  int found = -1;
  // 先比主码、再比副码：主码优先，两码只差 1 字节时行为确定。
  for (const auto &e : slot_store::state().entries) {
    if (!e.recorded()) continue;
    int diff = 0;
    for (int i = 0; i < slot_store::FRAME_LEN; i++) {
      if (e.frame[i] != f[i]) diff++;
    }
    if (diff <= 1) { found = static_cast<int>(e.id); break; }  // 允许 1 字节误差
  }
  if (found < 0) {
    uint32_t owner = slot_store::alias_owner(f);
    // 副码一律折算成主槽位 ID 上报；主槽位已删除的孤儿记录不触发。
    if (owner != 0 && slot_store::used(owner)) found = static_cast<int>(owner);
  }
  if (found >= 0) {
    uint32_t id = static_cast<uint32_t>(found);
    uint32_t now = millis();
    auto tx = st.last_tx.find(id);
    if (tx != st.last_tx.end() && now - tx->second < 400) return;  // 自发射回灌
    auto last = st.last_detect.find(id);
    if (last == st.last_detect.end() || now - last->second > 300) {
      st.last_detect[id] = now;
      std::string nm = slot_store::name_of(id);
      ESP_LOGI("shenghui", ">>> 检测到%s 被按下（RSSI=%.1f dBm）", nm.c_str(), rssi);
      status->publish_state(nm + " 被按下");
      press(found);
    }
  }
}

}  // namespace shenghui
