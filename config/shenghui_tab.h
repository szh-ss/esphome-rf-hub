#pragma once
// ============================================================
// 昇辉开关管理：ESPhome 自带首页里的第四个区块 + /sw/api JSON 接口
// ------------------------------------------------------------
// 走 ESPHome 官方 web_server_base::add_handler() 扩展点，
// add_handler 会自动套上 web_server 的 digest 认证中间件。
//
// 首页处理顺序（关键）：on_boot 的 setup_priority 默认 600，高于
// WebServer::get_setup_priority()（setup_priority::WIFI - 1）。此时
// WebServerBase::init() 尚未被调用，add_handler 只是把 handler 放进
// handlers_ 向量；稍后 init() 按登记顺序 addHandler，因此这里注册的
// IndexHandler 会排在官方 WebServer handler 之前，"/" 由它接管。
// 其它路径（/events、REST 等）canHandle 返回 false，继续交给官方 handler。
// 官方 web_server 的 ota: false 让首页不再渲染 OTA Update；其余原生区块保留。
//
// 首页内容 = 官方 local v3 页面 + 一段注入脚本（见 custom_index.h），
// 注入脚本把「昇辉开关管理」区块追加进 <esp-app> 的 main.flex-grid-half。
//
// 本文件由 includes: 注入 main.cpp 顶部，早于所有 id(...) 对象声明，
// 因此不直接引用 id(...)，对象一律由 on_boot 注入的回调访问。
// 槽位数据由 slot_store.h 存变长 NVS blob，slot 参数一律 1-based ID，
// 数量无固定上限，删除后 ID 不复用。
// ============================================================
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include "esphome.h"
#include "rf_admin.h"
#include "shenghui_state.h"
#include "custom_index.h"

namespace shenghui_tab {

struct Api {
  std::function<std::string()> list;                        // 槽位状态 JSON
  std::function<void(const std::string &)> add;             // 按名称分配新槽并进入学习
  std::function<void(int)> trigger;                        // 触发指定槽位
  std::function<void(int)> remove;                         // 删除指定槽位
  std::function<void(int, const std::string &)> rename;    // 命名指定槽位
  std::function<void(int)> sync;                           // 同步该槽位到 HA MQTT discovery
  std::function<void(int)> add_alias;                      // 给该槽位学习一个副码（双控）
  std::function<void(int)> remove_alias;                  // 清除该槽位的全部副码
  std::function<void(int, bool)> set_state;               // 只对表 HA 假状态，不发射 RF
};

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

// 生成槽位清单：只包含已录入的开关，按槽位 ID 升序，供页面动态渲染按钮
inline std::string make_list() {
  std::string out = "{\"slots\":[";
  size_t count = 0;
  for (const auto &e : slot_store::state().entries) {
    if (!e.recorded()) continue;
    if (count) out += ",";
    out += "{\"slot\":" + std::to_string(e.id);
    out += ",\"name\":\"" + json_escape(slot_store::name_of(e.id)) + "\"";
    out += ",\"aliases\":" + std::to_string(slot_store::alias_count(e.id));
    out += ",\"state\":\"" + std::string(rf_state::text(e.id)) + "\"}";
    count++;
  }
  out += "],\"count\":" + std::to_string(count);
  // 诊断字段：当前学习模式 + 最近收到的射频帧，供页面判断「到底收没收到信号」。
  const auto &rt = shenghui::runtime();
  std::string mode = rt.learn_id != 0 ? "new" : (rt.alias_learn_id != 0 ? "alias" : "");
  uint32_t learn_slot = rt.learn_id != 0 ? rt.learn_id : rt.alias_learn_id;
  out += ",\"learn\":{\"mode\":\"" + mode + "\",\"slot\":" + std::to_string(learn_slot) + "}";
  out += ",\"rx\":[";
  bool first_rx = true;
  for (const auto &r : shenghui::rx_log()) {
    if (!first_rx) out += ",";
    first_rx = false;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f", r.rssi);
    out += "{\"ms\":" + std::to_string(r.ms);
    char hd[12];
    snprintf(hd, sizeof(hd), "%02X %02X | ", r.head0, r.head1);
    out += ",\"hex\":\"" + std::string(hd) + shenghui::frame_hex(r.frame.data()) + "\"";
    out += ",\"rssi\":" + std::string(buf);
    out += ",\"ok\":" + std::string(r.usable ? "true" : "false") + "}";
  }
  out += "]}";
  return out;
}

// 状态轮询专用：只回槽位号 + 状态，不带名称 / 副码 / 射频日志。
// 首页 250ms 高频刷新走这个接口，避免每次都拼完整 list 大 JSON。
inline std::string make_states() {
  std::string out = "{\"slots\":[";
  size_t count = 0;
  for (const auto &e : slot_store::state().entries) {
    if (!e.recorded()) continue;
    if (count) out += ",";
    out += "{\"slot\":" + std::to_string(e.id);
    out += ",\"state\":\"" + std::string(rf_state::text(e.id)) + "\"}";
    count++;
  }
  out += "]}";
  return out;
}

// 接管首页 "/"：返回打过补丁的官方页面（gzip）
class IndexHandler : public esphome::web_server_idf::AsyncWebHandler {
 public:
  bool canHandle(esphome::web_server_idf::AsyncWebServerRequest *request) const override {
    char buf[esphome::web_server_idf::AsyncWebServerRequest::URL_BUF_SIZE];
    esphome::StringRef url = request->url_to(buf);
    return url == "/";
  }

  void handleRequest(esphome::web_server_idf::AsyncWebServerRequest *request) override {
    auto *response =
        request->beginResponse(200, "text/html", CUSTOM_INDEX_GZ, CUSTOM_INDEX_GZ_LEN);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  }
};

// 管理区块的 JSON 接口（页面初始只有「添加昇辉控股开关」）
class ApiHandler : public esphome::web_server_idf::AsyncWebHandler {
 public:
  explicit ApiHandler(Api api) : api_(std::move(api)) {}

  bool canHandle(esphome::web_server_idf::AsyncWebServerRequest *request) const override {
    char buf[esphome::web_server_idf::AsyncWebServerRequest::URL_BUF_SIZE];
    esphome::StringRef url = request->url_to(buf);
    return url == "/sw/api";
  }

  void handleRequest(esphome::web_server_idf::AsyncWebServerRequest *request) override {
    std::string action = request->arg("action");
    std::string body;
    if (action == "list") {
      body = api_.list ? api_.list() : "{\"slots\":[],\"count\":0}";
    } else if (action == "states") {
      body = make_states();
    } else if (action == "add") {
      // 进入学习模式会发布 text_sensor 状态，进而触发 API / SSE 广播。
      // 在 AsyncWebServer 的 HTTP 任务里直接跑会让设备重启，所以
      // 用官方 scheduler 推迟到 ESPHome 主循环执行。
      if (api_.add) {
        std::string name = request->arg("name");
        auto fn = api_.add;
        esphome::App.scheduler.set_timeout(&this->add_tag_, 0,
                                           [fn, name]() { fn(name); });
      }
      body = api_.list ? api_.list() : "{\"slots\":[],\"count\":0}";
    } else if (action == "trigger" || action == "remove" || action == "rename") {
      int slot = atoi(request->arg("slot").c_str());  // 页面传 1-based 槽位 ID
      if (rf_admin::valid_slot(slot)) {
        if (action == "trigger" && api_.trigger) {
          // RF 发射带 delay，同样交给主循环
          auto fn = api_.trigger;
          esphome::App.scheduler.set_timeout(&this->trigger_tag_, 0,
                                             [fn, slot]() { fn(slot); });
        } else if (action == "remove" && api_.remove) {
          api_.remove(slot);
          this->schedule_discovery(slot);
        } else if (action == "rename" && api_.rename) {
          api_.rename(slot, request->arg("name"));
          this->schedule_discovery(slot);
        }
      }
      body = api_.list ? api_.list() : "{\"slots\":[],\"count\":0}";
    } else if (action == "set_state") {
      int slot = atoi(request->arg("slot").c_str());
      std::string want = request->arg("state");
      if (rf_admin::valid_slot(slot) && (want == "on" || want == "off") && api_.set_state) {
        // publish_state_set 内部会自行推迟到主循环，HTTP 任务里直接调用是安全的。
        api_.set_state(slot, want == "on");
      }
      body = api_.list ? api_.list() : "{\"slots\":[],\"count\":0}";
    } else if (action == "add_alias" || action == "remove_alias") {
      int slot = atoi(request->arg("slot").c_str());
      if (rf_admin::valid_slot(slot)) {
        if (action == "add_alias" && api_.add_alias) {
          // 进入学习模式会发布 text_sensor 状态，必须推迟到 ESPHome 主循环，
          // 在 AsyncWebServer 的 HTTP 任务里直接跑会让设备重启。
          auto fn = api_.add_alias;
          esphome::App.scheduler.set_timeout(&this->alias_tags_[slot], 0,
                                             [fn, slot]() { fn(slot); });
        } else if (action == "remove_alias" && api_.remove_alias) {
          api_.remove_alias(slot);  // 只写 NVS，不触碰状态传感器
        }
      }
      body = api_.list ? api_.list() : "{\"slots\":[],\"count\":0}";
    } else {
      body = "{\"slots\":[],\"count\":0}";
    }
    request->send(200, "application/json; charset=utf-8", body.c_str());
  }

 protected:
  // 槽位数据已由 slot_store::save() 在每次改动时 nvs_commit() 立即落盘，
  // 不再依赖 restore_value 的延迟提交，也不需要额外的 sync()。
  // discovery 发布走 MQTT，推迟到主循环（HTTP 任务栈很小）。
  // 每个 ID 用独立 tag，连续改多个名字不会互相取消。
  void schedule_discovery(int slot) {
    if (!api_.sync) return;
    auto fn = api_.sync;
    esphome::App.scheduler.set_timeout(&this->mqtt_tags_[slot], 0,
                                       [fn, slot]() { fn(slot); });
  }

  Api api_;
  int add_tag_{0};
  int trigger_tag_{0};
  std::map<int, int> mqtt_tags_;
  std::map<int, int> alias_tags_;
};

// 在 on_boot 中调用；global_web_server_base 此时已赋值，认证凭据也已设置。
// IndexHandler 必须先登记，才能在 init() 里排在官方 handler 之前。
inline void install(Api api) {
  auto *base = esphome::web_server_base::global_web_server_base;
  if (base == nullptr) return;
  base->add_handler(new IndexHandler());               // NOLINT(cppcoreguidelines-owning-memory)
  base->add_handler(new ApiHandler(std::move(api)));  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace shenghui_tab
