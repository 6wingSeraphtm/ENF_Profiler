#include "krprof/html.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "krprof/analysis.hpp"
#include "krprof/attribution.hpp"

namespace krprof {
namespace {

void esc(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '&': out.append("&amp;"); break;
            case '<': out.append("&lt;"); break;
            case '>': out.append("&gt;"); break;
            case '"': out.append("&quot;"); break;
            default: out.push_back(c);
        }
    }
}

std::string fixed(double v, int digits = 1) {
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%.*f", digits, v);
    return buf.data();
}

// Веса профайлера — это снимки. Читателю нужны миллисекунды: «3237 снимков» ни о чём не
// говорит, а «3.2 с из 40» говорит всё.
std::string as_ms(std::uint64_t samples, std::uint32_t hz) {
    if (hz == 0) { return std::to_string(samples); }
    const double ms = 1000.0 * static_cast<double>(samples) / hz;
    return ms >= 1000.0 ? fixed(ms / 1000.0, 2) + " с" : fixed(ms) + " мс";
}

double share(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

// Полоска доли. Инлайновый div вместо графика: читается так же, а зависимостей ноль —
// отчёт должен открываться с диска на сервере, где интернета может не быть вовсе.
void bar(std::string& out, double pct, const char* cls = "") {
    out.append("<div class=\"bar ").append(cls).append("\"><i style=\"width:")
       .append(fixed(std::min(pct, 100.0), 2))
       .append("%\"></i></div>");
}


// Строка внутрь JSON: имена приходят из чужой памяти, и кавычка или обратный слэш в
// них превратили бы данные флеймграфа в синтаксическую ошибку — страница осталась бы
// без графика молча. `</` разрывается, чтобы содержимое не закрыло сам <script>.
void json_str(std::string& out, std::string_view text) {
    for (const char c : text) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '<': out.append("\\u003c"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::array<char, 8> buf{};
                    std::snprintf(buf.data(), buf.size(), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(buf.data());
                } else {
                    out.push_back(c);
                }
        }
    }
}

// Отрисовка флеймграфа. Своя, без библиотек: отчёт открывают с диска сервера, где сети
// может не быть, а нужны две вещи, которых нет у готовых виджетов, — цвет по МОДУ и
// зум по поддереву с сохранением предков.
constexpr const char* k_flame_js = R"JS(
(function(){
  const N=FG.n, W=document.getElementById('fg'), TIP=document.getElementById('fgtip');
  const CRUMB=document.getElementById('fgcrumb'), Q=document.getElementById('fgq');
  const ROW=19, PAD=1;
  // Родителей в данных нет — восстанавливаем один раз, они нужны для крошек и зума.
  const P=new Int32Array(N.length).fill(-1);
  N.forEach((n,i)=>n[4].forEach(c=>P[c]=i));
  const maxd=N.reduce((m,n)=>Math.max(m,n[3]),0);
  W.style.height=((maxd+1)*ROW+PAD*2)+'px';

  // Палитра по модам. Оттенки фиксированы списком, а не случайны: одна и та же сборка
  // модов даёт одну и ту же картинку в разных прогонах.
  const HUES=[214,266,158,26,190,334,232,120,44,300,10,176];
  const mods=[...new Set(FG.mods)].sort();
  const dark=()=>matchMedia('(prefers-color-scheme:dark)').matches
             && document.documentElement.dataset.theme!=='light'
             || document.documentElement.dataset.theme==='dark';
  function fill(m){const h=HUES[mods.indexOf(m)%HUES.length];
    return dark()?`hsl(${h} 38% 40%)`:`hsl(${h} 52% 70%)`;}
  const ink=()=>dark()?'#eef1f5':'#101319';

  const ms=v=>FG.hz?(1000*v/FG.hz):v;
  const fmt=v=>{const x=ms(v);return x>=1000?(x/1000).toFixed(2)+' с':x.toFixed(1)+' мс';};

  let focus=0, q='';
  function label(i){return N[i][0]<0?'весь профиль':FG.names[N[i][0]];}
  function mod(i){return N[i][0]<0?'':FG.mods[N[i][0]];}

  function place(i,x,w,out){
    out.push([i,x,w]);
    if(w<0.25) return;                       // тоньше четверти процента не видно
    let cx=x; const tot=N[i][1]||1;
    for(const c of N[i][4]){const cw=w*N[c][1]/tot; place(c,cx,cw,out); cx+=cw;}
  }

  function draw(){
    const chain=[]; for(let i=focus;i>=0;i=P[i]) chain.unshift(i);
    const bars=[]; place(focus,0,100,bars);
    const html=[];
    // Предки приближенного узла остаются во всю ширину — так видно, куда провалились.
    for(const a of chain.slice(0,-1)) html.push(bar(a,0,100,true));
    for(const [i,x,w] of bars) html.push(bar(i,x,w,false));
    W.innerHTML=html.join('');
    CRUMB.textContent=chain.length>1?chain.map(label).join('  ›  '):'';
  }

  function bar(i,x,w,anc){
    const nm=label(i), m=mod(i), root=N[i][0]<0;
    const hit=q&&((nm+' '+m).toLowerCase().includes(q));
    const cls='b'+(root?' base':'')+(q?(hit?' hit':' miss'):'');
    const st=`left:${x}%;width:${w}%;bottom:${N[i][3]*ROW+1}px;`+
             (root?'':`background:${fill(m)};color:${ink()}`);
    const txt=w>2.2?esc(nm):'';
    return `<div class="${cls}" style="${st}" data-i="${i}">${txt}</div>`;
  }
  function esc(t){return t.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}

  W.addEventListener('click',e=>{const b=e.target.closest('.b');if(!b)return;
    focus=+b.dataset.i; draw();});
  W.addEventListener('dblclick',()=>{focus=0;draw();});
  document.getElementById('fgreset').addEventListener('click',()=>{focus=0;draw();});
  Q.addEventListener('input',()=>{q=Q.value.trim().toLowerCase();draw();});

  W.addEventListener('mousemove',e=>{const b=e.target.closest('.b');
    if(!b){TIP.hidden=true;return;}
    const i=+b.dataset.i, n=N[i], sym=n[0];
    const pct=(100*n[1]/(N[0][1]||1)).toFixed(2);
    TIP.innerHTML=`<b>${esc(label(i))}</b>`+
      (sym>=0?`<span class="k">мод</span> ${esc(FG.mods[sym])}<br>`:'')+
      `<span class="k">всего</span> ${fmt(n[1])} · ${pct}% &nbsp;`+
      `<span class="k">своё</span> ${fmt(n[2])}`+
      (sym>=0&&FG.srcs[sym]?`<br><span class="k">${esc(FG.srcs[sym])}</span>`:'');
    TIP.hidden=false;
    const r=TIP.getBoundingClientRect();
    TIP.style.left=Math.min(e.clientX+14,innerWidth-r.width-8)+'px';
    TIP.style.top=Math.max(8,e.clientY-r.height-12)+'px';});
  W.addEventListener('mouseleave',()=>TIP.hidden=true);

  document.getElementById('fglg').innerHTML=mods.map(m=>
    `<span><i style="background:${fill(m)}"></i>${esc(m)}</span>`).join('');
  matchMedia('(prefers-color-scheme:dark)').addEventListener('change',draw);
  draw();
})();
)JS";

void name_cell(std::string& out, const symbol& s) {
    out.append("<span class=\"mod\">");
    esc(out, s.mod.empty() ? unknown_mod : std::string_view{s.mod});
    out.append("</span> <span class=\"fn\">");
    if (!s.klass.empty()) {
        esc(out, s.klass);
        out.append("<span class=\"dim\">::</span>");
    }
    esc(out, s.func.empty() ? std::string_view{"?"} : std::string_view{s.func});
    out.append("</span>");
}

}  // namespace

std::string to_html(const call_tree& tree,
                    const sample_log& log,
                    const symbol_table& symbols,
                    const run_meta& meta) {
    const std::uint64_t total = tree.total();
    std::string o;
    o.reserve(64 * 1024);

    o.append(R"(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Профиль скриптов DayZ</title><style>
/* Приборный отчёт: страницу ведёт моноширинный шрифт, потому что её содержание —
   идентификаторы и числа. Пропорциональный оставлен прозе. Шрифты системные: отчёт
   открывают с диска сервера, где сети может не быть. */
:root{
  --bg:#f6f7f9; --surface:#ffffff; --ink:#0d1117; --dim:#5b6470;
  --rule:#e1e4e9; --track:#e9ecf1; --accent:#364fc7; --signal:#b42318;
}
:root:not([data-theme="light"]){}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){
  --bg:#0d1014; --surface:#14181e; --ink:#e3e7ec; --dim:#8b95a1;
  --rule:#232a33; --track:#1e242c; --accent:#8ea2ff; --signal:#ff8a75;
}}
:root[data-theme="dark"]{
  --bg:#0d1014; --surface:#14181e; --ink:#e3e7ec; --dim:#8b95a1;
  --rule:#232a33; --track:#1e242c; --accent:#8ea2ff; --signal:#ff8a75;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
  font:13px/1.6 ui-monospace,'Cascadia Mono','JetBrains Mono',Consolas,monospace;
  font-variant-numeric:tabular-nums}
.wrap{max-width:1000px;margin:0 auto;padding:40px 20px 72px}
.prose{font-family:ui-sans-serif,system-ui,'Segoe UI',sans-serif}
h1{font:600 19px/1.3 ui-sans-serif,system-ui,sans-serif;margin:0 0 6px;
   letter-spacing:-.01em;text-wrap:balance}
.lede{font-family:ui-sans-serif,system-ui,sans-serif;color:var(--dim);
      font-size:13px;max-width:62ch;margin:0 0 28px}
h2{font:600 11px/1 ui-sans-serif,system-ui,sans-serif;text-transform:uppercase;
   letter-spacing:.09em;color:var(--dim);margin:38px 0 0;
   padding-bottom:8px;border-bottom:1px solid var(--rule)}
h2 .hint{text-transform:none;letter-spacing:0;font-weight:400;margin-left:10px}
/* Полоса приборов: числа прогона идут первыми, до любых деталей. */
.strip{display:flex;flex-wrap:wrap;gap:2px;margin:0 0 4px}
.strip div{flex:1 1 108px;background:var(--surface);border:1px solid var(--rule);
           padding:10px 12px}
.strip b{display:block;font-size:17px;font-weight:600;letter-spacing:-.01em}
.strip b.sig{color:var(--signal)}
.strip span{font-family:ui-sans-serif,system-ui,sans-serif;font-size:11px;color:var(--dim)}
table{width:100%;border-collapse:collapse}
td{padding:6px 10px 6px 0;border-bottom:1px solid var(--rule);vertical-align:middle}
tr:last-child td{border-bottom:0}
th{font:600 11px/1 ui-sans-serif,system-ui,sans-serif;text-transform:uppercase;
   letter-spacing:.07em;color:var(--dim);text-align:left;padding:10px 10px 8px 0;
   border-bottom:1px solid var(--rule)}
td.n,th.n{text-align:right;white-space:nowrap;padding-right:0}
/* Шкала: прямые торцы и тонкая линия — прибор, а не пилюля. */
.bar{height:4px;background:var(--track);min-width:56px}
.bar i{display:block;height:100%;background:var(--accent)}
.mod{color:var(--accent);white-space:nowrap}
.dim{color:var(--dim)}
.src{color:var(--dim);font-size:11px}
.rank{color:var(--dim);font-size:11px}
/* Цепочка: засечка слева вместо карточки — она означает «одно и то же, много раз». */
.chain{border-left:2px solid var(--accent);padding:2px 0 2px 14px;margin:18px 0 0}
.chain h3{margin:0 0 2px;font:600 14px/1.4 inherit}
.chain .sum{font-family:ui-sans-serif,system-ui,sans-serif;font-size:11px;color:var(--dim);
            margin-bottom:6px}
.note{font-family:ui-sans-serif,system-ui,sans-serif;color:var(--dim);font-size:12px;
      margin:14px 0 0;max-width:62ch}
/* ── Флеймграф ─────────────────────────────────────────────────────────────
   Цвет кодирует МОД, а не «жар»: жар и так задан шириной, а вопрос обычно
   «чей это код». Так же поступает VTune, окрашивая по модулю. */
.fgbar{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin:14px 0 10px;
       font-family:ui-sans-serif,system-ui,sans-serif;font-size:12px}
.fgbar input{flex:1 1 200px;max-width:280px;background:var(--surface);color:var(--ink);
  border:1px solid var(--rule);padding:5px 9px;font:inherit}
.fgbar input:focus{outline:2px solid var(--accent);outline-offset:-1px}
.fgbar button{background:var(--surface);color:var(--ink);border:1px solid var(--rule);
  padding:5px 11px;font:inherit;cursor:pointer}
.fgbar button:hover{border-color:var(--accent);color:var(--accent)}
.fgbar .crumb{color:var(--dim);flex:1 1 100%;font-size:11px;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fg{position:relative;width:100%;border:1px solid var(--rule);background:var(--surface);
    overflow:hidden}
.fg .b{position:absolute;height:18px;overflow:hidden;cursor:pointer;
  border-right:1px solid var(--surface);border-top:1px solid rgba(255,255,255,.14);
  font-size:11px;line-height:18px;padding:0 5px;white-space:nowrap;
  transition:filter .08s}
.fg .b:hover{filter:brightness(1.14)}
.fg .b.base{background:var(--track)!important;color:var(--dim)!important;
  border-top-color:transparent}
.fg .b.miss{opacity:.22}
.fg .b.hit{outline:1px solid var(--ink);outline-offset:-1px}
.fgtip{position:fixed;z-index:9;max-width:460px;background:var(--surface);
  border:1px solid var(--rule);box-shadow:0 6px 24px rgba(0,0,0,.22);
  padding:9px 11px;font-size:11px;line-height:1.5;pointer-events:none}
.fgtip b{display:block;font-size:12px;margin-bottom:3px;white-space:normal}
.fgtip .k{color:var(--dim)}
.legend{display:flex;flex-wrap:wrap;gap:4px 14px;margin-top:10px;
  font-family:ui-sans-serif,system-ui,sans-serif;font-size:11px;color:var(--dim)}
.legend i{display:inline-block;width:9px;height:9px;margin-right:5px;vertical-align:-1px}
@media (max-width:640px){.src{display:none}}
</style></head><body><div class="wrap">)");

    o.append("<h1>Профиль скриптов DayZ</h1><p class=\"lede\">Сэмплирующий профайлер "
             "KRPROF. Вес — собственное время кадра: то, что ушло в <code>super</code> "
             "или в чужой вызов, принадлежит тому, кто это исполнял, а не тому, кто "
             "позвал.</p>");

    // ── Метаданные прогона ───────────────────────────────────────────────────
    o.append("<div class=\"strip\">");
    o.append("<div><b>").append(as_ms(total, meta.sample_hz))
     .append("</b><span>времени в скрипте</span></div>");
    o.append("<div><b>").append(std::to_string(meta.samples))
     .append("</b><span>снимков</span></div>");
    o.append("<div><b>").append(std::to_string(meta.sample_hz))
     .append(" Гц</b><span>частота съёма</span></div>");
    o.append("<div><b>").append(fixed(meta.wall_ms / 1000.0))
     .append(" с</b><span>длительность</span></div>");
    o.append("<div><b>").append(std::to_string(meta.frames))
     .append("</b><span>эпизодов скрипта</span></div>");
    // Худший эпизод — единственное число, которое может означать беду, и единственное,
    // которому отдан сигнальный цвет. Остальное окрашено нейтрально намеренно.
    o.append("<div><b class=\"sig\">").append(fixed(meta.worst_frame_us / 1000.0))
     .append(" мс</b><span>худший эпизод</span></div>");
    o.append("</div>");

    // ── Моды ─────────────────────────────────────────────────────────────────
    std::vector<std::pair<std::string_view, std::uint64_t>> mods;
    {
        std::unordered_map<std::string_view, std::uint64_t> acc;
        for (const func_stat& f : by_function(tree, symbols)) {
            const symbol& s = symbols[f.id];
            acc[s.mod.empty() ? unknown_mod : std::string_view{s.mod}] += f.self;
        }
        mods.assign(acc.begin(), acc.end());
        std::ranges::sort(mods, [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
    }

    o.append("<h2>Во что обошёлся каждый мод<span class=\"hint\">по собственному "
             "времени</span></h2><table><tbody>");
    for (const auto& [mod, self] : mods) {
        o.append("<tr><td style=\"width:180px\"><span class=\"mod\">");
        esc(o, mod);
        o.append("</span></td><td>");
        bar(o, share(self, total));
        o.append("</td><td class=\"n\" style=\"width:70px\">")
         .append(fixed(share(self, total))).append(" %</td>");
        o.append("<td class=\"n\" style=\"width:90px\"><span class=\"dim\">")
         .append(as_ms(self, meta.sample_hz)).append("</span></td></tr>");
    }
    o.append("</tbody></table>");

    // ── Цепочки override'ов ──────────────────────────────────────────────────
    const auto chains = override_chains(tree, symbols);
    if (!chains.empty()) {
        o.append("<h2>Цепочки переопределений<span class=\"hint\">один метод — "
                 "несколько модов, у каждого своя цена</span></h2>");
        std::size_t shown = 0;
        for (const override_chain& c : chains) {
            if (++shown > 12) { break; }
            o.append("<div class=\"chain\"><h3>");
            esc(o, c.name);
            o.append("<div class=\"sum\">")
             .append(as_ms(c.total, meta.sample_hz))
             .append(" всего · ").append(std::to_string(c.links.size()))
             .append(" мод(ов) в цепочке</div>");
            o.append("<table><tbody>");
            for (const override_link& l : c.links) {
                o.append("<tr><td style=\"width:180px\"><span class=\"mod\">");
                esc(o, l.mod);
                o.append("</span></td><td>");
                bar(o, share(l.self, c.total), "g");
                o.append("</td><td class=\"n\" style=\"width:70px\">")
                 .append(fixed(share(l.self, c.total))).append(" %</td>");
                o.append("<td class=\"n\" style=\"width:90px\"><span class=\"dim\">")
                 .append(as_ms(l.self, meta.sample_hz)).append("</span></td><td>");
                if (!l.file.empty()) {
                    o.append("<span class=\"src\">");
                    esc(o, l.file);
                    o.append(":").append(std::to_string(l.line)).append("</span>");
                }
                o.append("</td></tr>");
            }
            o.append("</tbody></table></div>");
        }
        o.append("<p class=\"note\">Ванильный EnProfiler такие записи не показывает "
                 "вовсе: <code>FromSuper()</code> отбрасывает их намеренно.</p>");
    }

    // ── Самые долгие эпизоды ─────────────────────────────────────────────────
    // Объективный ответ на «куда ушло время в конкретном кадре». Сводка по модам
    // говорит про весь прогон, флеймграф — про форму стека; а «сервер дёрнулся, что
    // это было» видно только здесь.
    {
        const std::uint64_t period = meta.sample_hz != 0 ? 1'000'000ull / meta.sample_hz : 1000;
        const auto eps = episodes(log, symbols, period);
        if (!eps.empty()) {
            o.append("<h2>Самые долгие эпизоды<span class=\"hint\">непрерывная работа "
                     "скрипта — практически игровой кадр</span></h2>"
                     "<table><thead><tr><th>#</th><th class=\"n\">от старта</th>"
                     "<th class=\"n\">длилось</th><th class=\"n\">снимков</th>"
                     "<th>что было внутри дольше всего</th></tr></thead><tbody>");
            const std::uint64_t worst = eps[0].dur_us;
            std::size_t rank = 0;
            for (const episode& e : eps) {
                if (++rank > 12) { break; }
                o.append("<tr><td class=\"dim n\" style=\"width:28px\">")
                 .append(std::to_string(rank)).append("</td>");
                o.append("<td class=\"n\" style=\"width:84px\"><span class=\"dim\">")
                 .append(fixed(e.start_us / 1000.0)).append(" мс</span></td>");
                o.append("<td class=\"n\" style=\"width:84px\">")
                 .append(fixed(e.dur_us / 1000.0)).append(" мс</td>");
                o.append("<td class=\"n\" style=\"width:64px\"><span class=\"dim\">")
                 .append(std::to_string(e.samples)).append("</span></td><td>");
                bar(o, share(e.dur_us, worst));
                o.append("<div style=\"margin-top:3px\">");
                name_cell(o, symbols[e.top]);
                o.append(" <span class=\"dim\">")
                 .append(std::to_string(100 * e.top_samples / (e.samples ? e.samples : 1)))
                 .append("% эпизода</span></div></td></tr>");
            }
            o.append("</tbody></table>");
        }
    }

    // ── Флеймграф ────────────────────────────────────────────────────────────
    // Форма стека всё-таки нужна: таблица говорит «сколько», флеймграф — «под кем».
    // Рисуем сами, потому что нужен цвет по моду и зум по поддереву, а внешних
    // зависимостей у отчёта быть не должно.
    {
        const auto nodes = flame_tree(tree);
        o.append("<h2>Флеймграф<span class=\"hint\">цвет — мод, ширина — суммарное "
                 "время; клик приближает, двойной клик сбрасывает</span></h2>");
        o.append("<div class=\"fgbar\"><input id=\"fgq\" type=\"search\" "
                 "placeholder=\"поиск: функция или мод\" aria-label=\"Поиск по кадрам\">"
                 "<button id=\"fgreset\" type=\"button\">Сбросить масштаб</button>"
                 "<span class=\"crumb\" id=\"fgcrumb\"></span></div>");
        o.append("<div class=\"fg\" id=\"fg\"></div><div class=\"legend\" id=\"fglg\">"
                 "</div><div class=\"fgtip\" id=\"fgtip\" hidden></div>");

        o.append("<script>const FG=");
        o.append("{\"hz\":").append(std::to_string(meta.sample_hz));
        o.append(",\"names\":[");
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            if (i != 0) { o.push_back(','); }
            const symbol& s = symbols[static_cast<frame_id>(i)];
            o.push_back('"');
            if (!s.klass.empty()) { json_str(o, s.klass), o.append("::"); }
            json_str(o, s.func.empty() ? std::string_view{"?"} : std::string_view{s.func});
            o.push_back('"');
        }
        o.append("],\"mods\":[");
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            if (i != 0) { o.push_back(','); }
            const symbol& s = symbols[static_cast<frame_id>(i)];
            o.push_back('"');
            json_str(o, s.mod.empty() ? unknown_mod : std::string_view{s.mod});
            o.push_back('"');
        }
        o.append("],\"srcs\":[");
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            if (i != 0) { o.push_back(','); }
            const symbol& s = symbols[static_cast<frame_id>(i)];
            o.push_back('"');
            if (!s.file.empty()) {
                json_str(o, s.file);
                o.append(":").append(std::to_string(s.line));
            }
            o.push_back('"');
        }
        o.append("],\"n\":[");
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const flame_node& n = nodes[i];
            if (i != 0) { o.push_back(','); }
            o.append("[").append(n.is_root ? "-1" : std::to_string(n.id));
            o.append(",").append(std::to_string(n.total));
            o.append(",").append(std::to_string(n.self));
            o.append(",").append(std::to_string(n.depth)).append(",[");
            for (std::size_t k = 0; k < n.children.size(); ++k) {
                if (k != 0) { o.push_back(','); }
                o.append(std::to_string(n.children[k]));
            }
            o.append("]]");
        }
        o.append("]};");
        o.append(k_flame_js);
        o.append("</script>");
    }

    // ── Самые дорогие функции ────────────────────────────────────────────────
    o.append("<h2>Самые дорогие функции<span class=\"hint\">собственное время, "
             "без вложенных вызовов</span></h2><table><thead><tr>"
             "<th>#</th><th>функция</th><th></th><th class=\"n\">доля</th>"
             "<th class=\"n\">время</th><th>исходник</th></tr></thead><tbody>");
    std::size_t rank = 0;
    for (const func_stat& f : by_function(tree, symbols)) {
        if (++rank > 30) { break; }
        const symbol& s = symbols[f.id];
        o.append("<tr><td class=\"dim n\" style=\"width:28px\">").append(std::to_string(rank))
         .append("</td><td>");
        name_cell(o, s);
        o.append("</td><td style=\"width:160px\">");
        bar(o, share(f.self, total));
        o.append("</td><td class=\"n\" style=\"width:64px\">")
         .append(fixed(share(f.self, total))).append(" %</td>");
        o.append("<td class=\"n\" style=\"width:84px\"><span class=\"dim\">")
         .append(as_ms(f.self, meta.sample_hz)).append("</span></td><td>");
        if (!s.file.empty()) {
            o.append("<span class=\"src\">");
            esc(o, s.file);
            o.append(":").append(std::to_string(s.line)).append("</span>");
        }
        o.append("</td></tr>");
    }
    o.append("</tbody></table>");

    o.append("<p class=\"note\">Форма стека и покадровая ось — в файле "
             "<code>.speedscope.json</code> рядом.</p>");
    o.append("</div></body></html>\n");
    return o;
}

}  // namespace krprof
