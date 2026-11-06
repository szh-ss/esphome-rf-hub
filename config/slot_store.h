#pragma once
// ============================================================
// 昇辉开关槽位存储：变长 NVS blob，槽位数量不再有固定上限
// ------------------------------------------------------------
// 旧版用 8 个定长 globals（sw_frames / sw_names）存 NVS，槽位数被
// std::array<..., 8> 编译期定死。这里改成一块可变长度的 NVS blob：
//
//   namespace "rfslots" / key "slots"
//   blob = Header(16B) + N * Record(28B)
//   Header: magic "RFS1"(4) | version(1) | reserved(3)
//           | next_id(4, LE) | count(4, LE)
//   Record: id(4, LE) | frame[8] | name[16]（UTF-8，末位补 0）
//
// 之所以不用 ESPHome 的 globals restore_value：它按 sizeof(T) 整块读写，
// ESP32PreferenceBackend::load() 有 `actual_len != len → false` 的校验，
// 改数组长度会让旧数据全部失效。原生 nvs_get_blob 可先探测长度再读，
// 想加多少槽位就写多长的 blob，且 nvs_commit() 立即落盘，不依赖
// global_preferences->sync() 的延迟提交。
//
// 槽位 ID 从 1 开始、单调递增且删除后永不复用：HA 的 button / event
// 实体、MQTT topic、巴法云同步和自动化都按 ID 绑定，复用 ID 会串到
// 旧实体上。删除会留下 ID 空洞，这是预期行为。
//
// 无损迁移：blob 不存在即视为首次启动，把旧 8 槽（含空槽）原样搬成
// id 1..8、next_id=9；blob 存在（哪怕一个槽都没有）就绝不再迁移，
// 避免「删光后重启又被复活」。旧 globals 保留原样、只读，作为迁移源。
// ============================================================
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <nvs.h>
#include <nvs_flash.h>
#include "esphome.h"

namespace slot_store {

constexpr size_t FRAME_LEN = 8;
constexpr size_t NAME_CAP = 16;    // 含结尾 0，最多 15 字节 UTF-8
constexpr size_t HEADER_LEN = 16;
constexpr size_t RECORD_LEN = 28;  // id(4) + frame(8) + name(16)
constexpr int LEGACY_SLOTS = 8;
constexpr const char *NVS_NAMESPACE = "rfslots";
constexpr const char *NVS_KEY = "slots";
constexpr const char *TAG = "slot_store";

struct Entry {
  uint32_t id{0};
  std::array<uint8_t, FRAME_LEN> frame{};
  std::string name;

  // 帧非全零才算「已录入」；刚点添加、还没按物理开关的槽位不算。
  bool recorded() const {
    for (auto b : this->frame) {
      if (b != 0) return true;
    }
    return false;
  }
};

struct State {
  std::vector<Entry> entries;
  uint32_t next_id{1};
  bool ready{false};
  nvs_handle_t handle{0};
};

inline State &state() {
  static State s;
  return s;
}

inline size_t count();  // load() 里要用到，先声明

inline void put_u32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t get_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool open_() {
  State &st = state();
  if (st.handle != 0) return true;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &st.handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open(%s) 失败：%d", NVS_NAMESPACE, static_cast<int>(err));
    return false;
  }
  return true;
}

// 把内存状态整块写入 NVS 并立即提交。容量耗尽 / 写入失败时返回 false，
// 由 mutate() 回滚内存，绝不留下「界面显示成功但重启后丢失」的状态。
inline bool save() {
  State &st = state();
  if (!open_()) return false;
  const size_t n = st.entries.size();
  std::vector<uint8_t> buf(HEADER_LEN + n * RECORD_LEN, 0);
  buf[0] = 'R';
  buf[1] = 'F';
  buf[2] = 'S';
  buf[3] = '1';
  buf[4] = 1;
  put_u32(&buf[8], st.next_id);
  put_u32(&buf[12], static_cast<uint32_t>(n));
  for (size_t i = 0; i < n; i++) {
    uint8_t *p = &buf[HEADER_LEN + i * RECORD_LEN];
    const Entry &e = st.entries[i];
    put_u32(p, e.id);
    std::memcpy(p + 4, e.frame.data(), FRAME_LEN);
    size_t m = e.name.size() < NAME_CAP - 1 ? e.name.size() : NAME_CAP - 1;
    if (m) std::memcpy(p + 12, e.name.data(), m);
  }
  esp_err_t err = nvs_set_blob(st.handle, NVS_KEY, buf.data(), buf.size());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_set_blob 失败（%u 条 / %u 字节）：%d",
             static_cast<unsigned>(n), static_cast<unsigned>(buf.size()), static_cast<int>(err));
    return false;
  }
  err = nvs_commit(st.handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_commit 失败：%d", static_cast<int>(err));
    return false;
  }
  return true;
}

inline bool read_() {
  State &st = state();
  size_t len = 0;
  esp_err_t err = nvs_get_blob(st.handle, NVS_KEY, nullptr, &len);
  if (err != ESP_OK) return false;  // 首次启动：没有 blob，需要从旧 globals 迁移
  if (len < HEADER_LEN || (len - HEADER_LEN) % RECORD_LEN != 0) {
    ESP_LOGW(TAG, "blob 长度异常（%u 字节），按未迁移处理", static_cast<unsigned>(len));
    return false;
  }
  std::vector<uint8_t> buf(len, 0);
  size_t actual = len;
  err = nvs_get_blob(st.handle, NVS_KEY, buf.data(), &actual);
  if (err != ESP_OK || actual != len) return false;
  if (std::memcmp(buf.data(), "RFS1", 4) != 0 || buf[4] != 1) {
    ESP_LOGW(TAG, "blob 格式不匹配，按未迁移处理");
    return false;
  }
  uint32_t nid = get_u32(&buf[8]);
  uint32_t cnt = get_u32(&buf[12]);
  if (nid < 1 || cnt != (len - HEADER_LEN) / RECORD_LEN) return false;

  st.entries.clear();
  st.entries.reserve(cnt);
  for (uint32_t i = 0; i < cnt; i++) {
    const uint8_t *p = &buf[HEADER_LEN + i * RECORD_LEN];
    Entry e;
    e.id = get_u32(p);
    std::memcpy(e.frame.data(), p + 4, FRAME_LEN);
    size_t m = strnlen(reinterpret_cast<const char *>(p + 12), NAME_CAP - 1);
    e.name.assign(reinterpret_cast<const char *>(p + 12), m);
    st.entries.push_back(std::move(e));
  }
  st.next_id = nid;
  return true;
}

// on_boot 中调用一次。frames / names 是旧 globals 的 value() 引用，
// 只在需要迁移时读取；迁移完成后旧 globals 再也不会被写入。
template <typename FramesT, typename NamesT>
inline void load(const FramesT &frames, const NamesT &names) {
  State &st = state();
  if (st.ready) return;
  st.ready = true;
  if (!open_()) return;

  if (read_()) {
    ESP_LOGI(TAG, "NVS 已载入 %u 条记录（已录入 %u 个），下一个 ID=%u",
             static_cast<unsigned>(st.entries.size()), static_cast<unsigned>(count()),
             static_cast<unsigned>(st.next_id));
    return;
  }

  st.entries.clear();
  st.next_id = 1;
  for (int i = 0; i < LEGACY_SLOTS; i++) {
    Entry e;
    e.id = static_cast<uint32_t>(i + 1);
    for (int j = 0; j < FRAME_LEN; j++) e.frame[j] = frames[i][j];
    const char *p = names[i].data();
    size_t m = strnlen(p, NAME_CAP - 1);
    if (m) e.name.assign(p, m);
    st.entries.push_back(std::move(e));
  }
  st.next_id = LEGACY_SLOTS + 1;  // 旧 1..8 全部占位，新槽从 9 开始
  if (save()) {
    ESP_LOGI(TAG, "旧 8 槽已无损迁移（已录入 %u 个），下一个 ID=%u",
             static_cast<unsigned>(count()), static_cast<unsigned>(st.next_id));
  } else {
    ESP_LOGE(TAG, "迁移写入失败：本次改动只存在于内存，重启后会重新迁移");
  }
}

inline Entry *find(uint32_t id) {
  for (auto &e : state().entries) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

inline bool valid_id(uint32_t id) { return id >= 1 && id < state().next_id; }
inline bool used(uint32_t id) {
  Entry *e = find(id);
  return e != nullptr && e->recorded();
}
inline uint32_t next_id() { return state().next_id; }
inline size_t count() {
  size_t n = 0;
  for (const auto &e : state().entries) {
    if (e.recorded()) n++;
  }
  return n;
}

inline std::string name_of(uint32_t id) {
  Entry *e = find(id);
  if (e != nullptr && !e->name.empty()) return e->name;
  return "开关 " + std::to_string(id);
}

// 所有写操作都走 mutate：先改内存、立刻落盘，失败则整体回滚。
template <typename F>
inline bool mutate(F fn) {
  State &st = state();
  std::vector<Entry> backup = st.entries;
  uint32_t backup_next = st.next_id;
  fn(st);
  if (save()) return true;
  st.entries = std::move(backup);
  st.next_id = backup_next;
  ESP_LOGE(TAG, "写入失败，内存状态已回滚");
  return false;
}

// 分配一个新 ID。学习尚未完成时该记录的 frame 全零，不会出现在页面列表，
// 也不会生成 HA 实体；学习超时由 release_unlearned() 回收这个 ID。
inline uint32_t allocate(const std::string &name) {
  uint32_t id = 0;
  bool ok = mutate([&](State &st) {
    id = st.next_id;
    Entry e;
    e.id = id;
    size_t m = name.size() < NAME_CAP - 1 ? name.size() : NAME_CAP - 1;
    if (m) e.name.assign(name.data(), m);
    st.entries.push_back(std::move(e));
    st.next_id++;
  });
  if (!ok) return 0;
  ESP_LOGI(TAG, "分配槽位 ID=%u", static_cast<unsigned>(id));
  return id;
}

inline bool set_frame(uint32_t id, const uint8_t *f) {
  if (find(id) == nullptr) return false;
  return mutate([&](State &st) {
    for (auto &e : st.entries) {
      if (e.id == id) {
        std::memcpy(e.frame.data(), f, FRAME_LEN);
        break;
      }
    }
  });
}

inline bool set_name(uint32_t id, const std::string &name) {
  if (find(id) == nullptr) return false;
  return mutate([&](State &st) {
    for (auto &e : st.entries) {
      if (e.id == id) {
        size_t m = name.size() < NAME_CAP - 1 ? name.size() : NAME_CAP - 1;
        e.name.assign(name.data(), m);
        break;
      }
    }
  });
}

inline bool alias_clear(uint32_t owner_id);  // 定义见本文件末尾的副码段

// 删除记录但保留 ID 空洞（next_id 不回退），避免复用旧实体绑定的编号。
inline bool remove(uint32_t id) {
  if (find(id) == nullptr) return false;
  bool ok = mutate([&](State &st) {
    for (auto it = st.entries.begin(); it != st.entries.end(); ++it) {
      if (it->id == id) {
        st.entries.erase(it);
        break;
      }
    }
  });
  // 主槽位没了，它名下的副码一并清掉，避免留下永远匹配不到的孤儿记录。
  if (ok) alias_clear(id);
  return ok;
}

// 学习超时 / 取消时回收末尾那个「还没录入」的 ID。它从未暴露给 HA，
// 复用安全，也能避免反复点添加却没按开关时留下大量隐形空号。
inline void release_unlearned(uint32_t id) {
  State &st = state();
  if (st.entries.empty()) return;
  const Entry &last = st.entries.back();
  if (last.id != id || last.recorded()) return;
  if (id + 1 != st.next_id) return;
  mutate([](State &s) {
    s.entries.pop_back();
    s.next_id--;
  });
  ESP_LOGI(TAG, "回收未录入的槽位 ID=%u", static_cast<unsigned>(id));
}

// ============================================================
// 副码：一个逻辑开关可以绑定多个 RF 帧
// ------------------------------------------------------------
// 场景：双控（两个不同位置的开关控制同一盏灯）会发出两个不同的码，
// 但 HA 侧只应该有一个 button / event / 假状态。副码存成独立 blob，
// 不动上面的 slots blob：老设备 OTA 后副码表为空，8 个旧槽位原样可用。
//
//   namespace "rfslots" / key "aliases"
//   blob = Header(16B) + N * Record(12B)
//   Header: magic "RFA1"(4) | version(1) | reserved(3) | count(4, LE at 8)
//   Record: owner_id(4, LE) | frame[8]
//
// 匹配顺序固定「先主码、后副码」：万一两码只差 1 字节，主码优先，
// 行为确定；副码命中后按 owner_id 上报，HA 感知不到副码的存在。
// ============================================================
constexpr const char *ALIAS_KEY = "aliases";
constexpr size_t ALIAS_HEADER_LEN = 16;
constexpr size_t ALIAS_RECORD_LEN = 12;

struct Alias {
  uint32_t owner_id{0};
  std::array<uint8_t, FRAME_LEN> frame{};
};

struct AliasState {
  std::vector<Alias> items;
  bool ready{false};
};

inline AliasState &alias_state() {
  static AliasState s;
  return s;
}

inline uint8_t frame_diff(const uint8_t *a, const uint8_t *b) {
  uint8_t d = 0;
  for (size_t i = 0; i < FRAME_LEN; i++) {
    if (a[i] != b[i]) d++;
  }
  return d;
}

inline bool alias_save() {
  AliasState &st = alias_state();
  if (!open_()) return false;
  const size_t n = st.items.size();
  std::vector<uint8_t> buf(ALIAS_HEADER_LEN + n * ALIAS_RECORD_LEN, 0);
  buf[0] = 'R'; buf[1] = 'F'; buf[2] = 'A'; buf[3] = '1'; buf[4] = 1;
  put_u32(&buf[8], static_cast<uint32_t>(n));
  for (size_t i = 0; i < n; i++) {
    uint8_t *p = &buf[ALIAS_HEADER_LEN + i * ALIAS_RECORD_LEN];
    put_u32(p, st.items[i].owner_id);
    std::memcpy(p + 4, st.items[i].frame.data(), FRAME_LEN);
  }
  esp_err_t err = nvs_set_blob(state().handle, ALIAS_KEY, buf.data(), buf.size());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "副码表写入失败（%u 条）：%d", static_cast<unsigned>(n), static_cast<int>(err));
    return false;
  }
  err = nvs_commit(state().handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "副码表 nvs_commit 失败：%d", static_cast<int>(err));
    return false;
  }
  return true;
}

// on_boot 里在 slot_store::load() 之后调用一次。blob 不存在是正常情况
// （首次启用一槽多码），按空表处理，绝不触碰 slots blob。
inline void alias_load() {
  AliasState &st = alias_state();
  if (st.ready) return;
  st.ready = true;
  if (!open_()) return;
  size_t len = 0;
  if (nvs_get_blob(state().handle, ALIAS_KEY, nullptr, &len) != ESP_OK) {
    ESP_LOGI(TAG, "副码表为空（首次启用一槽多码）");
    return;
  }
  if (len < ALIAS_HEADER_LEN || (len - ALIAS_HEADER_LEN) % ALIAS_RECORD_LEN != 0) {
    ESP_LOGW(TAG, "副码表长度异常（%u 字节），按空表处理", static_cast<unsigned>(len));
    return;
  }
  std::vector<uint8_t> buf(len, 0);
  size_t actual = len;
  if (nvs_get_blob(state().handle, ALIAS_KEY, buf.data(), &actual) != ESP_OK || actual != len) return;
  if (std::memcmp(buf.data(), "RFA1", 4) != 0 || buf[4] != 1) {
    ESP_LOGW(TAG, "副码表格式不匹配，按空表处理");
    return;
  }
  uint32_t cnt = get_u32(&buf[8]);
  if (cnt != (len - ALIAS_HEADER_LEN) / ALIAS_RECORD_LEN) return;
  st.items.clear();
  st.items.reserve(cnt);
  for (uint32_t i = 0; i < cnt; i++) {
    const uint8_t *p = &buf[ALIAS_HEADER_LEN + i * ALIAS_RECORD_LEN];
    Alias a;
    a.owner_id = get_u32(p);
    std::memcpy(a.frame.data(), p + 4, FRAME_LEN);
    st.items.push_back(a);
  }
  ESP_LOGI(TAG, "副码表已载入 %u 条", static_cast<unsigned>(st.items.size()));
}

inline size_t alias_count(uint32_t owner_id) {
  size_t n = 0;
  for (const auto &a : alias_state().items) {
    if (a.owner_id == owner_id) n++;
  }
  return n;
}

// 收到的帧是否属于某个副码；返回它的主槽位 ID，0 = 未命中。
inline uint32_t alias_owner(const uint8_t *f) {
  for (const auto &a : alias_state().items) {
    if (frame_diff(a.frame.data(), f) <= 1) return a.owner_id;
  }
  return 0;
}

// 学习副码前的冲突检查：返回中文提示，空串表示可以录入。
// 拒绝与任何已录入主码 / 已有副码相差不超过 1 字节的帧，避免同一个
// 物理开关被绑到两盏灯上、或在副码表里堆重复记录。
inline std::string alias_conflict(const uint8_t *f) {
  // 只拒绝「完全相同」的帧。双控的两个码可能只差 1 字节，用 <=1 判定会
  // 把合法的副开关误判成重复码而拒绝录入。
  for (const auto &e : state().entries) {
    if (e.recorded() && frame_diff(e.frame.data(), f) == 0)
      return "这个信号已录入为「" + name_of(e.id) + "」";
  }
  for (const auto &a : alias_state().items) {
    if (frame_diff(a.frame.data(), f) == 0) return "这个信号已经添加过副码";
  }
  return "";
}

inline bool alias_add(uint32_t owner_id, const uint8_t *f) {
  AliasState &st = alias_state();
  std::vector<Alias> backup = st.items;
  Alias a;
  a.owner_id = owner_id;
  std::memcpy(a.frame.data(), f, FRAME_LEN);
  st.items.push_back(a);
  if (alias_save()) {
    ESP_LOGI(TAG, "已为槽位 %u 添加副码（共 %u 个）",
             static_cast<unsigned>(owner_id), static_cast<unsigned>(alias_count(owner_id)));
    return true;
  }
  st.items = std::move(backup);
  return false;
}

inline bool alias_clear(uint32_t owner_id) {
  AliasState &st = alias_state();
  if (!st.ready) alias_load();
  std::vector<Alias> backup = st.items;
  std::vector<Alias> kept;
  for (const auto &a : st.items) {
    if (a.owner_id != owner_id) kept.push_back(a);
  }
  if (kept.size() == backup.size()) return true;  // 本来就没有副码
  st.items = std::move(kept);
  if (alias_save()) {
    ESP_LOGI(TAG, "槽位 %u 的副码已全部清除", static_cast<unsigned>(owner_id));
    return true;
  }
  st.items = std::move(backup);
  return false;
}

}  // namespace slot_store
