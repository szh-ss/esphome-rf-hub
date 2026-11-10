# Build a patched copy of ESPHome web_server v3 local index page.
# Source: /esphome/esphome/components/web_server/server_index_v3.h (gzip variant)
# Output: /config/custom_index.h  (gzip bytes as a C array)
#
# The injected script appends a "昇辉开关管理" section into the <esp-app>
# shadow root's main.flex-grid-half, reusing the stock .tab-header /
# .tab-container / .col classes. It also injects a style into <esp-log>'s own
# shadow root so the log column matches the entity column's height, scrolls
# internally and auto-follows new log lines. No stock markup is replaced.
#
# PATCHES modify the stock minified front-end JS in place:
#   - translate the entity-group / log headers to Chinese
#   - replace render_fan() with a compact Chinese-labelled control row
#     (labelled switch, speed, explicit fan-off button, forward/reverse buttons)
#   - replace renderTitle() with a Chinese inline uptime badge beside the title
# (The OTA Update section is disabled the official way: `ota: false` in the
#  web_server config makes the stock renderOta() skip it.)
# Every patch requires exactly one match, so an ESPHome image upgrade that
# changes the stock page fails loudly instead of silently producing a bad page.

import gzip
import io
import re

SRC = "/esphome/esphome/components/web_server/server_index_v3.h"
DST = "/config/custom_index.h"

INJECT = r"""<script>
(function(){
  var CSS = [
    // 运行时长改为中文，并作为行内小字紧跟在标题右侧，不再另起一行。
    'header h1 .sh-uptime{display:inline-block;margin-left:12px;font-size:14px;font-weight:400;opacity:.62;vertical-align:middle;white-space:nowrap}',
    // 调试日志宿主铺满列宽；具体高度由 styleLog() 按左侧实体表实测同步。
    '#col_logs esp-log{display:block}',
    '#col_shenghui{flex-basis:100%;grid-column:1/-1}',
    '#col_shenghui .sh-panel{padding:10px 12px}',
    // 名称输入与「添加」按钮同一行：输入框占满剩余宽度，按钮固定在右侧。
    '#col_shenghui .sh-row{display:flex;align-items:stretch;gap:8px;margin:0 0 10px}',
    '#col_shenghui .sh-primary{flex:none;white-space:nowrap;padding:8px 14px;background:#6b7280;color:#fff;border:1px solid #6b7280;border-radius:7px;font:inherit;font-size:13px;font-weight:600;cursor:pointer}',
    '#col_shenghui .sh-primary:hover{background:#5b6472}',
    '#col_shenghui .sh-primary:disabled{opacity:.45;cursor:not-allowed}',
    '#col_shenghui .sh-name{flex:1;min-width:0;box-sizing:border-box;margin:0;padding:8px 10px;background:rgba(127,127,127,.10);color:inherit;border:1px solid rgba(127,127,127,.35);border-radius:7px;font:inherit;font-size:13px}',
    '#col_shenghui .sh-name::placeholder{opacity:.6}',
    '#col_shenghui .sh-msg{min-height:20px;margin:0 0 8px;color:#0b6b3a;font-size:13px}',
    '#col_shenghui .sh-msg.err{color:#b42318}',
    '#col_shenghui .sh-card{display:flex;align-items:center;gap:8px;flex-wrap:wrap;background:rgba(127,127,127,.08);border:1px solid rgba(127,127,127,.35);border-radius:8px;padding:8px 10px;margin-bottom:8px;color:inherit}',
    '#col_shenghui .sh-card .idx{opacity:.65;font-size:12px;width:16px;flex:none}',
    '#col_shenghui .sh-card input{flex:1;min-width:80px;background:rgba(127,127,127,.10);color:inherit;border:1px solid rgba(127,127,127,.35);border-radius:6px;padding:5px 8px;font:inherit;font-size:13px}',
    '#col_shenghui .sh-card button{background:rgba(127,127,127,.10);color:inherit;border:1px solid rgba(127,127,127,.35);border-radius:6px;padding:5px 9px;font:inherit;font-size:12px;cursor:pointer;white-space:nowrap}',
    '#col_shenghui .sh-card button+button{margin-left:14px}',
    '#col_shenghui .sh-card button.danger{color:#b42318;border-color:#f0c7c2}',
    '#col_shenghui .sh-badge{font-size:11px;padding:2px 6px;border-radius:10px;background:rgba(127,127,127,.18);opacity:.85;white-space:nowrap;flex:none;margin-right:4px}',
    // 状态圆点已移除：副码徽标后直接是「触发」按钮，不再显示 / 轮询状态。
    '#col_shenghui .sh-empty{background:rgba(127,127,127,.08);border:1px dashed rgba(127,127,127,.35);border-radius:8px;padding:16px;text-align:center;opacity:.75;font-size:13px;margin-bottom:8px}',
    // 操作说明置于「昇辉开关管理」最上方，作为进入该区块的第一段引导。
    '#col_shenghui .sh-tip{opacity:.7;font-size:12px;line-height:1.65;margin:0 0 12px;padding:0 0 10px;border-bottom:1px solid rgba(127,127,127,.22)}',
    '#col_shenghui .sh-rx{margin:10px 0 0;padding:8px 10px;background:rgba(127,127,127,.08);border:1px solid rgba(127,127,127,.25);border-radius:7px;font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:11px;line-height:1.7;opacity:.85;word-break:break-all}'
  ].join('');

  // entity-row lives inside <esp-entity-table>'s own shadow root, so the
  // fan control styles have to be injected there, not into the app root.
  function styleFan(){
    var app=document.querySelector('esp-app');
    var root=app&&app.shadowRoot; if(!root) return false;
    var tbl=root.querySelector('esp-entity-table');
    var sr=tbl&&tbl.shadowRoot; if(!sr) return false;
    if(sr.querySelector('#sh-fan-style')) return true;
    var st=document.createElement('style'); st.id='sh-fan-style';
    st.textContent=[
      '.fan-ctl{display:flex;align-items:center;justify-content:flex-end;flex-wrap:wrap;gap:8px;width:100%}',
      '.fan-ctl esp-range-slider{flex:1 1 130px;min-width:100px;max-width:220px;display:block}',
      '.fan-ctl .range-wrap{width:100%}',
      '.fan-ctl .fan-lvl{font-size:12px;opacity:.75;white-space:nowrap}',
      '.fan-ctl .fan-label{font-size:12px;opacity:.65;white-space:nowrap}',
      '.fan-ctl .fan-btn{background:rgba(127,127,127,.10);color:inherit;border:1px solid rgba(127,127,127,.35);border-radius:6px;padding:5px 10px;font:inherit;font-size:12px;cursor:pointer;white-space:nowrap}',
      '.fan-ctl .fan-btn:hover{border-color:var(--primary-color,currentColor)}'
    ].join('');
    sr.appendChild(st);
    return true;
  }

  // 调试日志：与左侧实体列等高、内容区可滚动、新日志自动跟随到底部。
  // 关键背景：esp-log 只保留最近 50 行且 .logs 只有 overflow-x，日志一多
  // 就把右列撑高；官方没有任何高度约束和自动滚动逻辑。
  var logHooked = false, logScrollBound = false;

  function syncLogHeight(root){
    var ent = root.querySelector('#col_entities esp-entity-table');
    var log = root.querySelector('#col_logs esp-log');
    if(!ent || !log || !ent.shadowRoot || !log.shadowRoot) return;
    var main = root.querySelector('main.flex-grid-half');
    // 双击表头展开单列时，左列被隐藏，改成占满可视高度。
    if(main && (main.classList.contains('expanded_logs') || main.classList.contains('expanded_entity'))){
      log.style.setProperty('--sh-log-h','70vh');
      return;
    }
    // 用左侧内容的自然高度（不受 grid 拉伸影响），避免「日志变高 -> 行变高
    // -> 测量值变大」的循环放大。
    var nodes = ent.shadowRoot.querySelectorAll('.tab-header,.tab-container');
    if(!nodes.length) return;
    var top = nodes[0].getBoundingClientRect().top;
    var bottom = nodes[nodes.length-1].getBoundingClientRect().bottom;
    var header = log.shadowRoot.querySelector('.tab-header');
    var hh = header ? header.getBoundingClientRect().height : 0;
    log.style.setProperty('--sh-log-h', Math.max(180, Math.round(bottom - top - hh))+'px');
  }

  function bindLogScroll(sr){
    var bound = null, atBottom = true;
    function cur(){
      var l = sr.querySelector('.logs');
      if(l && l !== bound){
        bound = l;
        l.addEventListener('scroll', function(){
          // 用户主动上翻查看历史时暂时不跟随，回到底部附近自动恢复。
          atBottom = l.scrollHeight - l.scrollTop - l.clientHeight < 40;
        });
      }
      return l;
    }
    if(!cur()) return false;
    // 观察整个 shadow root：lit 重渲染会替换 .tbody 里的行，节点不会失效。
    new MutationObserver(function(){
      var l = cur();
      if(!l || !atBottom) return;
      requestAnimationFrame(function(){ l.scrollTop = l.scrollHeight; });
    }).observe(sr, {childList:true, subtree:true});
    requestAnimationFrame(function(){ var l=cur(); if(l) l.scrollTop=l.scrollHeight; });
    return true;
  }

  function hookLogSync(root){
    if(logHooked) return;
    logHooked = true;
    var ent = root.querySelector('#col_entities');
    if(window.ResizeObserver && ent){
      new ResizeObserver(function(){ syncLogHeight(root); }).observe(ent);
    }
    window.addEventListener('resize', function(){ syncLogHeight(root); });
    setInterval(function(){ syncLogHeight(root); },2000);  // 实体列表重渲染兜底
  }

  function styleLog(){
    var app = document.querySelector('esp-app');
    var root = app && app.shadowRoot; if(!root) return false;
    var log = root.querySelector('#col_logs esp-log');
    var sr = log && log.shadowRoot; if(!sr || !sr.querySelector('.logs')) return false;
    if(!sr.querySelector('#sh-log-style')){
      var st = document.createElement('style'); st.id = 'sh-log-style';
      st.textContent = [
        '.tab-container{height:var(--sh-log-h,360px);box-sizing:border-box;display:flex;flex-direction:column;overflow:hidden}',
        '.logs{flex:1;min-height:0;box-sizing:border-box;overflow-y:auto;overflow-x:auto;overscroll-behavior:contain;scrollbar-width:thin}',
        '.thead{position:sticky;top:0;z-index:1;background:rgba(127,127,127,.22);backdrop-filter:blur(4px)}',
        '@media (width<=1024px){.tab-container{height:auto!important;max-height:60vh}.logs{max-height:60vh}}'
      ].join('');
      sr.appendChild(st);
    }
    if(!logScrollBound){
      if(!bindLogScroll(sr)) return false;
      logScrollBound = true;
    }
    syncLogHeight(root);
    hookLogSync(root);
    return true;
  }

  function mount(){
    var app = document.querySelector('esp-app');
    if(!app) return false;
    var root = app.shadowRoot || app;
    var main = root.querySelector('main.flex-grid-half') || root.querySelector('main');
    if(!main) return false;
    if(root.querySelector('#col_shenghui')) return true;

    var st = document.createElement('style');
    st.textContent = CSS;
    (app.shadowRoot ? root : document.head).appendChild(st);

    var sec = document.createElement('section');
    sec.id = 'col_shenghui';
    sec.className = 'col';
    sec.innerHTML =
      '<div class="tab-header">昇辉开关管理</div>' +
      '<div class="tab-container sh-panel">' +
        '<p class="sh-tip">先在左侧填名称，再点右侧的「添加昇辉控股开关」（可留空，之后随时改名）；点击后 30 秒内连按 2~3 下要录入的物理开关，录入完成后会自动出现对应按钮。同一盏灯有两个开关时，在该开关上点「添加副码」把第二个开关绑进来，两边按压都会同步状态。副码提示右侧的圆点是状态：绿色 = 开、灰色 = 关、虚线 = 未知，它是 HA 推算的假状态；点一下即可对表，只改状态、不会发射 RF 信号。点「触发」会发射 RF 并同步翻转这个状态。HA 侧边栏「昇辉灯状态」页面显示的是同一份数据。</p>' +
        '<div class="sh-row">' +
          '<input id="sh-name" class="sh-name" type="text" maxlength="15" placeholder="输入开关名称（可留空，之后可随时改名）">' +
          '<button id="sh-add" class="sh-primary">添加昇辉控股开关</button>' +
        '</div>' +
        '<p id="sh-msg" class="sh-msg"></p>' +
        '<div id="sh-list"></div>' +
        '<div id="sh-rx" class="sh-rx"></div>' +
      '</div>';
    main.appendChild(sec);
    wire(sec);
    return true;
  }

  function wire(sec){
    var $ = function(s){ return sec.querySelector(s); };
    var pollTimer = null;

    function esc(s){return String(s).replace(/[&<>"']/g,function(c){
      return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
    function say(t,err){var m=$('#sh-msg');m.textContent=t||'';m.className='sh-msg'+(err?' err':'');}
    function api(q){
      return fetch('/sw/api?'+q,{cache:'no-store'}).then(function(r){
        if(!r.ok) throw new Error('request failed');
        return r.json();
      });
    }
    function render(d){
      var box=$('#sh-list');
      if(!d.slots.length){
        box.innerHTML='<div class="sh-empty">尚未录入开关</div>';
      }else{
        box.innerHTML=d.slots.map(function(x){
          var n=x.aliases||0;
          return '<div class="sh-card"><span class="idx">'+x.slot+'</span>'
            +'<input type="text" title="最多 5 个汉字或 15 个字母" value="'+esc(x.name)+'" data-input="'+x.slot+'">'
            +(n?'<span class="sh-badge">'+n+' 个副码</span>':'')
            +'<button data-act="trigger" data-slot="'+x.slot+'">触发</button>'
            +'<button data-act="rename" data-slot="'+x.slot+'">改名</button>'
            +'<button data-act="add_alias" data-slot="'+x.slot+'">添加副码</button>'
            +(n?'<button data-act="remove_alias" data-slot="'+x.slot+'">清除副码</button>':'')
            +'<button class="danger" data-act="remove" data-slot="'+x.slot+'">删除</button>'
            +'</div>';
        }).join('');
      }
      $('#sh-add').disabled = false;  // 槽位数量无固定上限
      var rx=d.rx||[], rxBox=$('#sh-rx');
      if(rxBox){
        rxBox.innerHTML = rx.length
          ? ('最近收到的信号（'+rx.length+' 条）<br>'+rx.slice(-4).reverse().map(function(r){
              return esc(r.hex)+' &nbsp;RSSI '+r.rssi+(r.ok?'':' （无效帧）')+(r.rssi<-75?' <span style="opacity:.55">· 弱信号/噪声</span>':'');
            }).join('<br>'))
          : '最近收到的信号：无（按下开关后这里应出现一行十六进制码）';
      }
      return d;
    }
    function load(){ return api('action=list').then(render); }
    // 状态显示与高频轮询已移除：一次操作只发一个请求、不做任何后续等待。
    function waitForNew(before){
      var known={}, tries=0;
      before.forEach(function(x){known[x.slot]=1;});
      clearInterval(pollTimer);
      pollTimer=setInterval(function(){
        tries++;
        api('action=list').then(function(d){
          render(d);
          var added=null;
          d.slots.forEach(function(x){if(!known[x.slot]) added=x;});
          if(added){
            clearInterval(pollTimer);
            $('#sh-name').value='';
            say('录入成功：'+added.name);
          }else if(tries>20){
            clearInterval(pollTimer);
            say('未检测到新开关，可重试',true);
          }
        }).catch(function(){});
      },150);
    }
    // 副码录入不新增槽位，靠 aliases 计数或新增射频帧判断学习结果。
    function waitForAlias(slot,before,rxBase){
      var tries=0, seen=0;
      clearInterval(pollTimer);
      pollTimer=setInterval(function(){
        tries++;
        api('action=list').then(function(d){
          var now=0;
          d.slots.forEach(function(x){if(String(x.slot)===String(slot)) now=x.aliases||0;});
          render(d);
          if(now>before){
            clearInterval(pollTimer);
            say('副码添加成功，两个开关现在共用一个状态');
            return;
          }
          // 底噪包 RSSI 通常 <= -90，只把明显更强的帧当作「开关信号」。
          var fresh=(d.rx||[]).filter(function(r){return r.ms>rxBase && r.ok && r.rssi>-80;});
          if(fresh.length){
            var last=fresh[fresh.length-1];
            if(last.ms!==seen){
              seen=last.ms;
              say('已收到开关信号 '+last.hex+'（RSSI '+last.rssi+'），请再按一下确认');
            }
          }else if(d.learn && d.learn.mode==='' && tries>3){
            clearInterval(pollTimer);
            say('学习已结束：未收到任何信号，请靠近设备重试',true);
            return;
          }
          if(tries>20){
            clearInterval(pollTimer);
            say('未检测到副码，可重试',true);
          }
        }).catch(function(){});
      },150);
    }

    $('#sh-add').addEventListener('click',function(){
      var v=$('#sh-name').value.trim();
      if(v && new TextEncoder().encode(v).length>15){
        say('名称过长：最多 5 个汉字或 15 个字母',true);return;
      }
      var q='action=add'+(v?'&name='+encodeURIComponent(v):'');
      api('action=list').then(function(d){
        return api(q).then(function(){
          say(v?('学习模式：请在 30 秒内连按 2~3 下「'+v+'」的开关')
                :'学习模式：请在 30 秒内连按 2~3 下要录入的物理开关');
          waitForNew(d.slots);
        });
      }).catch(function(){say('操作失败，请刷新重试',true);});
    });

    $('#sh-list').addEventListener('click',function(e){
      var b=e.target.closest('button'); if(!b) return;
      var slot=b.getAttribute('data-slot'), act=b.getAttribute('data-act');
      if(act==='remove' && !window.confirm('确定删除这个开关？')) return;
      if(act==='remove_alias' && !window.confirm('确定清除这个开关的所有副码？')) return;
      // 「触发」只发一个请求；设备端发射 RF 的同时自行翻假状态并同步 HA，
      // 不再有第二次 HTTP 请求、状态圆点和收敛等待。
      if(act==='trigger'){
        api('action=trigger&slot='+slot)
          .then(function(d){ render(d); say('已触发'); })
          .catch(function(){ say('触发失败，请刷新重试',true); });
        return;
      }
      if(act==='add_alias'){
        api('action=list').then(function(d){
          var before=0, rxBase=0, rx=d.rx||[];
          d.slots.forEach(function(x){if(String(x.slot)===String(slot)) before=x.aliases||0;});
          if(rx.length) rxBase=rx[rx.length-1].ms;
          return api('action=add_alias&slot='+slot).then(function(){
            say('副码学习：请在 30 秒内连续按 2~3 下另一个开关');
            waitForAlias(slot,before,rxBase);
          });
        }).catch(function(){say('操作失败，请刷新重试',true);});
        return;
      }
      var q='action='+act+'&slot='+slot;
      if(act==='rename'){
        var v=this.querySelector('[data-input="'+slot+'"]').value.trim();
        if(!v){say('名称不能为空',true);return;}
        var bytes=new TextEncoder().encode(v).length;
        if(bytes>15){say('名称过长：最多 5 个汉字或 15 个字母',true);return;}
        q+='&name='+encodeURIComponent(v);
      }
      api(q).then(function(d){
        render(d);
        say(act==='trigger'?'已触发':act==='remove'?'已删除':act==='remove_alias'?'副码已清除':'已改名');
      }).catch(function(){say('操作失败，请刷新重试',true);});
    });

    load().catch(function(){say('读取失败，请刷新页面',true);});
    // 状态轮询已移除：页面只在用户操作时刷新一次列表。
  }

  var tries=0;
  var timer=setInterval(function(){
    tries++;
    var a=mount(), b=styleFan(), c=styleLog();
    if((a && b && c) || tries>100) clearInterval(timer);
  },50);
})();
</script>"""

STOCK_FAN = """render_fan(){if(this.entity)return[this.entity.speed,` `,this.entity.speed_level,this._switch(this.entity),this.entity.speed_count?this._range(this.entity,`turn_${this.entity.state.toLowerCase()}`,`speed_level`,this.entity.speed_level?this.entity.speed_level:0,0,this.entity.speed_count,1):``]}"""

# stock page renders an undefined `speed`, a bare speed_level number, a switch
# and a 0..6 slider all crammed into one narrow flex cell, with no direction
# control at all (web_server REST does not parse `direction` either).
# Replace it with a fully Chinese-labelled row: labelled switch + "风速 N/6" +
# 1..6 slider + explicit "关闭风扇" + "转向 正转/反转". Direction buttons call
# /sw/api?action=fan_dir, which emits one RF frame without any state gating.
CUSTOM_FAN = """render_fan(){if(this.entity)return D`<div class="fan-ctl"><span class="fan-label">开关</span>${this._switch(this.entity)}${this.entity.speed_count?D`<span class="fan-lvl">风速 ${this.entity.speed_level||0}/${this.entity.speed_count}</span>${this._range(this.entity,`turn_on`,`speed_level`,this.entity.speed_level?this.entity.speed_level:1,1,this.entity.speed_count,1)}`:``}<button class="fan-btn" @click=${()=>this.actioner?.restAction(this.entity,`turn_off`)}>关闭风扇</button><span class="fan-label">转向</span><button class="fan-btn" @click=${()=>fetch(`/sw/api?action=fan_dir&dir=forward`,{cache:`no-store`})}>正转</button><button class="fan-btn" @click=${()=>fetch(`/sw/api?action=fan_dir&dir=reverse`,{cache:`no-store`})}>反转</button></div>`}"""

# The stock header renders `started 3 minutes ago` (Intl.RelativeTimeFormat('en'))
# in a separate <div> under <h1>, so it always wraps to a second line. Replace it
# with a Chinese duration formatted from the same SSE uptime value, placed as an
# inline <span> right after the device title.
STOCK_TITLE = """renderTitle(){return D`<h1>${this.config.title||D`\xa0`}</h1><div>${[this.config.comment,`started ${this.uptime()}`].filter(e=>e).join(` · `)}</div>`}"""
CUSTOM_TITLE = """renderTitle(){return D`<h1>${this.config.title||D`\xa0`}<span class="sh-uptime">${this._shUptime()}</span></h1>${this.config.comment?D`<div class="sh-comment">${this.config.comment}</div>`:``}`}_shUptime(){let t=Math.max(0,this.ping||0)/1e3|0,r=[];let d=t/86400|0,h=t%86400/3600|0,m=t%3600/60|0,s=t%60;if(d)r.push(d+` 天`);if(h)r.push(h+` 小时`);if(m)r.push(m+` 分`);if(!d&&!h)r.push(s+` 秒`);return`已运行 `+r.join(` `)}"""

# Keep the internal English group keys (they must match the SSE sorting_group
# names); translate only at render time.
GROUP_LABELS = """${({'Sensor and Control':'传感器与控制','Configuration':'配置','Diagnostic':'诊断','States':'状态'})[e]||e}</div><div class="tab-container">"""

PATCHES = [
    ("Chinese group headers", "${e}</div><div class=\"tab-container\">", GROUP_LABELS),
    ("Chinese log header", ">Debug Log</div>", ">调试日志</div>"),
    ("compact fan control", STOCK_FAN, CUSTOM_FAN),
    ("Chinese inline uptime", STOCK_TITLE, CUSTOM_TITLE),
]


def main():
    src = open(SRC, encoding="utf-8").read()
    m = re.search(r"INDEX_GZ\[\] PROGMEM = \{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit("cannot locate INDEX_GZ in " + SRC)
    raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))
    html = gzip.decompress(raw).decode("utf-8")
    if "</body>" not in html:
        raise SystemExit("unexpected index html")

    for label, old, new in PATCHES:
        n = html.count(old)
        if n != 1:
            raise SystemExit("patch %r matched %d times (expected 1)" % (label, n))
        html = html.replace(old, new, 1)
        print("patched:", label)

    patched = html.replace("</body>", INJECT + "</body>")

    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(patched.encode("utf-8"))
    data = buf.getvalue()

    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    body = "\n".join(lines)

    header = (
        "#pragma once\n"
        "// Patched ESPHome web_server v3 local index (ESPHome 2026.10.2).\n"
        "// Generated by gen_index.py from server_index_v3.h + injected tab.\n"
        "// Regenerate whenever the ESPHome image tag changes.\n"
        "// 补丁：中文分组 / 风扇控件（中文标识 + 关闭风扇 + 正反转） / 无限制槽位 / 昇辉管理（副码，无状态显示） / 日志等高滚动 / 中文运行时长同行 / 零轮询零收敛。\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n"
        '#include "esphome/core/hal.h"\n'
        "\n"
        "namespace shenghui_tab {\n"
        "const uint8_t CUSTOM_INDEX_GZ[] PROGMEM = {\n"
        + body + "\n"
        "};\n"
        "constexpr size_t CUSTOM_INDEX_GZ_LEN = sizeof(CUSTOM_INDEX_GZ);\n"
        "}  // namespace shenghui_tab\n"
    )
    open(DST, "w", encoding="utf-8").write(header)
    print("html:", len(html), "->", len(patched), "gzip:", len(data), "bytes")
    print("written:", DST)


main()
