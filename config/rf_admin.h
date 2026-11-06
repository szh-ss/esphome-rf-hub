#pragma once
// ============================================================
// 昇辉开关槽位管理：命名 / 删除 / 清单 / 自动分配新槽
// ------------------------------------------------------------
// 数据全部由 slot_store.h 持有（变长 NVS blob），槽位数量无固定上限。
// 对外统一用 1-based 槽位 ID：它同时是 MQTT topic 编号、HA 实体编号、
// 网页按钮的 data-slot，删除后永久留空、绝不复用。
//
// 本文件由 includes: 注入到 main.cpp 顶部，早于所有 id(...) 对象声明，
// 因此不直接引用 id(...)，状态传感器由 yaml 的 lambda 以参数传入。
// 与 shenghui_switch.h 的分工：那个管射频收发，这个管槽位记录。
// ============================================================
#include <string>
#include "slot_store.h"
#include "shenghui_switch.h"

namespace rf_admin {

inline bool valid_slot(int slot) {
  return slot >= 1 && slot_store::find(static_cast<uint32_t>(slot)) != nullptr;
}

// 槽位是否已录入（帧非全零；刚点添加、还没按开关的不算）
inline bool slot_used(int slot) {
  return slot >= 1 && slot_store::used(static_cast<uint32_t>(slot));
}

inline void clear_slot(int slot) {
  if (slot < 1) return;
  if (!slot_store::remove(static_cast<uint32_t>(slot))) {
    ESP_LOGW("rf_admin", "槽位 %d 删除失败或不存在", slot);
    return;
  }
  ESP_LOGI("rf_admin", "槽位 %d 已删除（ID 保留不再复用）", slot);
}

inline void set_name(int slot, const std::string &name) {
  if (slot < 1) return;
  if (!slot_store::set_name(static_cast<uint32_t>(slot), name)) {
    ESP_LOGW("rf_admin", "槽位 %d 改名失败或不存在", slot);
  }
}

inline std::string name_of(int slot) {
  return slot_store::name_of(static_cast<uint32_t>(slot));
}

// 进入学习模式：复用 shenghui_switch.h 的运行时状态机，名称取自 NVS
template <typename StatusT>
inline void learn_start(StatusT *status, int slot) {
  if (slot < 1) return;
  auto &st = shenghui::runtime();
  // 两种学习模式互斥；取消副码学习不需要回收 ID（没分配过新槽位）。
  st.alias_learn_id = 0;
  st.learn_id = static_cast<uint32_t>(slot);
  st.learn_start = millis();
  st.learn_win.clear();
  std::string nm = name_of(slot);
  status->publish_state(nm + "：30 秒内连按 2~3 下开关");
  ESP_LOGI("rf_admin", "学习模式：请按%s的开关（ID=%d，30 秒内）", nm.c_str(), slot);
}

// 「添加昇辉控股开关」：分配一个新 ID 并进入学习。
// 名称在分配时写入 NVS（可留空）；录入成功后帧由 on_packet 写入。
template <typename StatusT>
inline void learn_auto(StatusT *status, const std::string &name) {
  uint32_t id = slot_store::allocate(name);
  if (id == 0) {
    status->publish_state("存储空间已满或写入失败，请先删除不用的开关");
    ESP_LOGE("rf_admin", "无法分配新槽位（NVS 写入失败）");
    return;
  }
  learn_start(status, static_cast<int>(id));
}

// 「添加副码」：给已录入的槽位再绑一个 RF 码，用于双控 / 多位开关。
// 不分配新槽位、不改 HA 实体；学习成功后由 on_packet 写入副码表。
template <typename StatusT>
inline void alias_learn_start(StatusT *status, int slot) {
  if (!slot_used(slot)) {
    status->publish_state("请先录入该开关，再添加副码");
    return;
  }
  auto &st = shenghui::runtime();
  // 若正在录入新槽位，先回收那个还没学完的 ID，避免留下隐形空号。
  if (st.learn_id != 0) {
    slot_store::release_unlearned(st.learn_id);
    st.learn_id = 0;
  }
  st.alias_learn_id = static_cast<uint32_t>(slot);
  st.alias_learn_start = millis();
  st.learn_win.clear();
  std::string nm = name_of(slot);
  status->publish_state(nm + "：30 秒内连按 2~3 下另一个开关");
  ESP_LOGI("rf_admin", "副码学习：请按%s的另一个开关（主槽位 ID=%d）", nm.c_str(), slot);
}

inline size_t alias_count(int slot) {
  return slot >= 1 ? slot_store::alias_count(static_cast<uint32_t>(slot)) : 0;
}

inline void alias_clear(int slot) {
  if (slot < 1) return;
  if (!slot_store::alias_clear(static_cast<uint32_t>(slot))) {
    ESP_LOGW("rf_admin", "槽位 %d 副码清除失败", slot);
  }
}

// 供状态传感器 / 调试显示的槽位清单
inline std::string summary() {
  std::string out;
  bool first = true;
  for (const auto &e : slot_store::state().entries) {
    if (!e.recorded()) continue;
    if (!first) out += " | ";
    first = false;
    out += std::to_string(e.id);
    out += ":";
    out += slot_store::name_of(e.id);
  }
  if (first) out = "空";
  return out;
}

}  // namespace rf_admin
