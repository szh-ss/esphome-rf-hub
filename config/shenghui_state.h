#pragma once
// ============================================================
// 昇辉开关「假状态」在设备侧的内存镜像
// ------------------------------------------------------------
// 昇辉 RF 单向、无状态回传，灯的开关状态由 HA 的 input_boolean 推算。
// HA 每次状态变化会把 8 个槽位的全量状态以 retain 消息发到
//   living-room-rf-hub/shenghui/state/all   payload: "1:on,2:off,..."
// 设备订阅后缓存到内存；重启后由 broker 的 retain 消息自动恢复。
// .35 首页已不再显示 / 轮询状态，这份缓存只用于 HA / 米家的状态同步，
// 不参与任何 RF 发射逻辑。
// ============================================================
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

namespace rf_state {

inline std::map<uint32_t, bool> &cache() {
  static std::map<uint32_t, bool> m;
  return m;
}

inline void set(uint32_t id, bool on) { cache()[id] = on; }

// 物理按压时先在本地乐观翻转一次，让首页圆点立刻反馈，不必等 HA 广播
// 回传 + 前端轮询。HA 随后发来的全量 state/all 是绝对值，会覆盖成最终值。
// 未知状态按「关」处理，第一次按压翻成「开」。
inline bool flip(uint32_t id) {
  auto it = cache().find(id);
  bool next = !(it != cache().end() && it->second);
  set(id, next);
  return next;
}

inline bool known(uint32_t id) { return cache().find(id) != cache().end(); }

inline bool is_on(uint32_t id) {
  auto it = cache().find(id);
  return it != cache().end() && it->second;
}

inline const char *text(uint32_t id) {
  if (!known(id)) return "unknown";
  return is_on(id) ? "on" : "off";
}

// 解析 "1:on,2:off,5:on"。格式刻意保持纯文本，避免引入 JSON 依赖；
// 只覆盖消息里出现的槽位，其余保持原值，防止半包把状态清成未知。
inline void parse(const std::string &payload) {
  size_t pos = 0;
  while (pos < payload.size()) {
    size_t comma = payload.find(',', pos);
    if (comma == std::string::npos) comma = payload.size();
    std::string part = payload.substr(pos, comma - pos);
    size_t colon = part.find(':');
    if (colon != std::string::npos) {
      uint32_t id = static_cast<uint32_t>(
          strtoul(part.substr(0, colon).c_str(), nullptr, 10));
      std::string v = part.substr(colon + 1);
      if (id != 0 && (v == "on" || v == "off")) set(id, v == "on");
    }
    pos = comma + 1;
  }
}

}  // namespace rf_state
