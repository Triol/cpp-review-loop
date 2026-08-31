#!/usr/bin/env python3
"""
make_report.py — render the interactive HTML review report from findings JSON.

Usage:
    python make_report.py <findings.json> [--out <report.html>]

Reads a findings JSON (schema: references/report-format.md) and writes a
self-contained HTML report (no network, no external dependencies, works from
file://). The page lets the user set a verdict per finding (采纳修复 / 驳回 /
待讨论) plus an optional comment, autosaves to localStorage, and exports the
collected feedback as <report-stem>-feedback.json for the follow-up fix round.

OFFLINE CONSTRAINT: the template must stay fully self-contained — no CDN,
no webfonts (system font stack only), no <script src>/<link>/<img> external
references, no fetch/XHR. Reports must render and stay interactive on a
machine with no network at all. Python side is stdlib-only for the same
reason (no pip install required).

Prints the paths to tell the user afterwards.
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

SEVERITIES = ("Blocker", "Major", "Minor", "Nit")

TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>C++ 代码审查报告</title>
<style>
:root{
  --bg:#f6f7f9; --card:#fff; --ink:#1a1d21; --muted:#6b7280; --line:#e5e7eb;
  --blocker:#dc2626; --major:#ea580c; --minor:#ca8a04; --nit:#6b7280;
  --accept:#16a34a; --discuss:#2563eb; --reject:#9ca3af; --accent:#2563eb;
}
*{box-sizing:border-box}
html{overflow-x:clip}
body{margin:0;font-family:"Segoe UI","Microsoft YaHei",system-ui,sans-serif;background:var(--bg);color:var(--ink);font-size:14px;line-height:1.65}
.wrap{max-width:960px;margin:0 auto;padding:24px 16px 120px}
header h1{font-size:22px;margin:0 0 4px}
.meta{color:var(--muted);font-size:13px;margin-bottom:12px}
.summary{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin-bottom:12px;white-space:pre-wrap}
.hint{background:#eef4ff;border:1px solid #c7d8ff;border-radius:10px;padding:10px 14px;font-size:13px;margin:12px 0}
.toolbar{position:sticky;top:0;z-index:30;background:var(--bg);margin:0 calc(50% - 50vw);padding:10px calc(50vw - 50%);border-bottom:1px solid var(--line);display:flex;flex-wrap:wrap;gap:8px;align-items:center;transform:translateZ(0)}
.chip{border:1px solid var(--line);background:var(--card);border-radius:999px;padding:4px 12px;cursor:pointer;font-size:13px}
.chip.on{background:var(--ink);color:#fff;border-color:var(--ink)}
.prog{margin-left:auto;font-size:13px;color:var(--muted)}
button.act{border:1px solid var(--accent);background:var(--accent);color:#fff;border-radius:8px;padding:6px 14px;cursor:pointer;font-size:13px}
button.act.ghost{background:var(--card);color:var(--accent)}
h2{font-size:16px;margin:20px 0 8px}
.pattern{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 16px;margin-bottom:10px}
.pattern b{display:block;margin-bottom:4px}
.pattern .ex{color:var(--muted);font-size:12.5px;margin-top:4px}
.card{background:var(--card);border:1px solid var(--line);border-left-width:4px;border-radius:10px;padding:14px 16px;margin-bottom:12px}
.card.s-blocker{--sev:var(--blocker);border-left-color:var(--sev)}
.card.s-major{--sev:var(--major);border-left-color:var(--sev)}
.card.s-minor{--sev:var(--minor);border-left-color:var(--sev)}
.card.s-nit{--sev:var(--nit);border-left-color:var(--sev)}
.card.v-accept,.card.v-discuss,.card.v-custom{outline:2px solid var(--sev)}
.card.v-reject{opacity:.55}
.fid{position:relative;display:inline-block;font-family:Consolas,monospace;font-weight:700;font-size:15px;color:#fff;background:var(--sev,#334155);border-radius:6px;padding:1px 8px;margin-right:8px;cursor:pointer;user-select:none;vertical-align:1px}
.fid:hover{filter:brightness(1.12)}
.fid::after{content:"点击复制";position:absolute;left:50%;top:-24px;transform:translateX(-50%);background:#111827;color:#fff;font-family:"Segoe UI","Microsoft YaHei",system-ui,sans-serif;font-size:11px;font-weight:400;padding:2px 8px;border-radius:4px;white-space:nowrap;opacity:0;pointer-events:none;transition:opacity .15s;z-index:6}
.fid:hover::after{opacity:1}
.fid.copied::after{content:"已复制 ✓";opacity:1}
.sev{display:inline-block;font-size:12px;font-weight:600;border-radius:6px;padding:1px 8px;color:#fff}
.sev.blocker{background:var(--blocker)}
.sev.major{background:var(--major)}
.sev.minor{background:var(--minor)}
.sev.nit{background:var(--nit)}
.loc{font-family:Consolas,monospace;font-size:12.5px;color:var(--muted);margin-left:8px}
.card h3{font-size:15px;margin:8px 0 6px}
.card .why{margin:4px 0}
.card .fix{background:#f0fdf4;border:1px solid #bbf7d0;border-radius:8px;padding:8px 10px;margin:6px 0}
.card .fix b{color:var(--accept)}
.verdicts{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
.vbtn{border:1px solid var(--line);background:var(--card);border-radius:8px;padding:5px 14px;cursor:pointer;font-size:13px}
.vbtn.accept.on{background:var(--accept);border-color:var(--accept);color:#fff}
.vbtn.reject.on{background:var(--reject);border-color:var(--reject);color:#fff}
.vbtn.discuss.on{background:var(--discuss);border-color:var(--discuss);color:#fff}
.vbtn.custom.on{background:#7c3aed;border-color:#7c3aed;color:#fff}
.comment{width:100%;margin-top:8px;border:1px solid var(--line);border-radius:8px;padding:8px;font:inherit;min-height:44px;resize:vertical;background:#fafafa}
.hidden{display:none!important}
footer.note{color:var(--muted);font-size:12.5px;margin-top:24px}
#modal{position:fixed;inset:0;background:rgba(0,0,0,.4);display:flex;align-items:center;justify-content:center;z-index:100}
#modal .box{background:#fff;border-radius:12px;padding:16px;max-width:640px;width:92%}
#modal textarea{width:100%;height:300px;font-family:Consolas,monospace;font-size:12px;margin-top:8px}
</style>
</head>
<body>
<div class="wrap">
<header>
  <h1>C++ 代码审查报告</h1>
  <div class="meta" id="meta"></div>
  <div class="summary" id="summary"></div>
  <div class="hint">逐项选择处理方式（可加意见；只写意见未点按钮的项会按「待讨论」随反馈一起导出），完成后点「<b>导出反馈 JSON</b>」，把下载的文件放回本报告同目录；然后在你的编码 agent（ZCode / Claude Code / DeepSeek 等能加载本 skill 的工具）里说：<b>处理审查反馈，按反馈修复</b>（反馈文件即刚导出的 JSON，文件名见上方元信息）。刷新页面不丢已填内容（本地自动保存）。</div>
</header>
<div class="toolbar">
  <span style="font-size:13px;color:var(--muted)">严重度</span>
  <span id="sevchips"></span>
  <select id="stfilter" class="chip" style="padding:4px 8px">
    <option value="">全部状态</option>
    <option value="none">未处理</option>
    <option value="accept">已采纳</option>
    <option value="reject">已驳回</option>
    <option value="discuss">待讨论</option>
    <option value="custom">已自定义</option>
  </select>
  <span class="prog" id="prog"></span>
  <button class="act" id="btn-export">导出反馈 JSON</button>
  <button class="act ghost" id="btn-copy">复制反馈 JSON</button>
</div>
<main>
  <div id="systemic-sec" class="hidden">
    <h2>系统性模式</h2>
    <div id="systemic"></div>
  </div>
  <h2>发现明细（<span id="fcount"></span>）</h2>
  <div id="findings"></div>
</main>
<footer class="note" id="genat"></footer>
</div>
<div id="modal" class="hidden">
  <div class="box">
    <b>反馈 JSON（剪贴板不可用，请手动全选复制）</b>
    <textarea id="modal-ta" readonly></textarea>
    <div style="text-align:right;margin-top:8px"><button class="act ghost" id="modal-close">关闭</button></div>
  </div>
</div>
<script id="review-data" type="application/json">__REVIEW_DATA__</script>
<script>
(function(){
"use strict";
var DATA=JSON.parse(document.getElementById("review-data").textContent);
var REV=DATA.review||{};
var FINDINGS=DATA.findings||[];
var SYSTEMIC=DATA.systemic||[];
var ORDER={Blocker:0,Major:1,Minor:2,Nit:3};
var VLABEL={accept:"采纳修复",reject:"驳回",discuss:"待讨论",custom:"自定义"};
var COMMENT_PH="意见（可选）：补充上下文、为什么驳回、修复边界…";
var CUSTOM_PH="自定义处理方式（必填）：写明你的决定或方案，agent 会按此执行…";
var KEY="cppcr:"+(REV.repo||"")+":"+(REV.date||"")+":"+FINDINGS.length;
var state={verdicts:{},comments:{}};
try{
  var saved=JSON.parse(localStorage.getItem(KEY)||"null");
  if(saved&&saved.verdicts){state.verdicts=saved.verdicts;state.comments=saved.comments||{};}
}catch(e){}

function esc(s){return String(s==null?"":s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;");}
function save(){try{localStorage.setItem(KEY,JSON.stringify(state));}catch(e){}}

FINDINGS.sort(function(a,b){
  var da=(ORDER[a.severity]!==undefined?ORDER[a.severity]:9);
  var db=(ORDER[b.severity]!==undefined?ORDER[b.severity]:9);
  if(da!==db)return da-db;
  var d=String(a.file).localeCompare(String(b.file));
  if(d)return d;
  return (a.line||0)-(b.line||0);
});

document.title="C++ 代码审查报告 — "+(REV.repo||"");
document.getElementById("meta").textContent=[REV.repo,REV.scope,REV.date].filter(Boolean).join(" · ");
document.getElementById("summary").textContent=DATA.summary||"(无总体评估)";
document.getElementById("genat").textContent="生成时间 "+(REV.generated_at||"")+" · 反馈导出文件名："+(REV.feedback_file||"code-review-report-feedback.json");

var sevF="",stF="";
var chips=document.getElementById("sevchips");
function chip(label,val){
  var b=document.createElement("button");
  b.className="chip"+(val===""?" on":"");
  b.textContent=label;
  b.onclick=function(){
    sevF=val;
    chips.querySelectorAll(".chip").forEach(function(x){x.classList.remove("on");});
    b.classList.add("on");
    applyFilters();
  };
  chips.appendChild(b);
}
chip("全部","");
var present=[];
FINDINGS.forEach(function(f){if(present.indexOf(f.severity)<0)present.push(f.severity);});
present.sort(function(a,b){return (ORDER[a]!==undefined?ORDER[a]:9)-(ORDER[b]!==undefined?ORDER[b]:9);});
present.forEach(function(s){chip(s,s);});

if(SYSTEMIC.length){
  document.getElementById("systemic-sec").classList.remove("hidden");
  document.getElementById("systemic").innerHTML=SYSTEMIC.map(function(p){
    return '<div class="pattern"><b><span class="fid" data-copy="'+esc(p.id||"")+'" title="点击复制编号">'+esc(p.id||"")+"</span>"+esc(p.title)+(p.count?"（"+esc(p.count)+" 处）":"")+"</b>"
      +(p.recommendation?"<div>"+esc(p.recommendation)+"</div>":"")
      +((p.examples&&p.examples.length)?"<div class='ex'>示例："+esc(p.examples.join("、"))+"</div>":"")
      +"</div>";
  }).join("");
}

function cardHtml(f){
  var v=state.verdicts[f.id]||"";
  var btns=["accept","reject","discuss","custom"].map(function(k){
    return '<button class="vbtn '+k+(v===k?" on":"")+'" data-v="'+k+'">'+VLABEL[k]+"</button>";
  }).join("");
  return '<div class="card s-'+f.severity.toLowerCase()+' v-'+(v||"none")+'" data-id="'+esc(f.id)+'" data-sev="'+esc(f.severity)+'">'
    +'<div><span class="fid" data-copy="'+esc(f.id)+'" title="点击复制编号">'+esc(f.id)+"</span>"
    +'<span class="sev '+f.severity.toLowerCase()+'">'+esc(f.severity)+"</span>"
    +'<span class="loc">'+esc(f.file)+(f.line?":"+f.line:"")+(f.category?" · "+esc(f.category):"")+"</span></div>"
    +"<h3>"+esc(f.title)+"</h3>"
    +(f.detail?"<div class='why'>"+esc(f.detail)+"</div>":"")
    +(f.fix?"<div class='fix'><b>建议修复：</b>"+esc(f.fix)+"</div>":"")
    +'<div class="verdicts">'+btns+"</div>"
    +'<textarea class="comment" placeholder="'+COMMENT_PH+'">'+esc(state.comments[f.id]||"")+"</textarea>"
    +"</div>";
}
var fc=document.getElementById("findings");
fc.innerHTML=FINDINGS.map(cardHtml).join("");
document.getElementById("fcount").textContent=FINDINGS.length+" 项";

fc.addEventListener("click",function(e){
  var btn=e.target.closest?e.target.closest(".vbtn"):null;
  if(!btn)return;
  var card=btn.closest(".card");
  var id=card.dataset.id,v=btn.dataset.v;
  state.verdicts[id]=(state.verdicts[id]===v)?"":v;
  card.querySelectorAll(".vbtn").forEach(function(b){b.classList.toggle("on",state.verdicts[id]===b.dataset.v);});
  card.classList.remove("v-accept","v-reject","v-discuss","v-custom","v-none");
  card.classList.add("v-"+(state.verdicts[id]||"none"));
  var ta=card.querySelector(".comment");
  if(ta)ta.placeholder=(state.verdicts[id]==="custom")?CUSTOM_PH:COMMENT_PH;
  if(state.verdicts[id]==="custom"&&ta)ta.focus();
  save();updateProg();applyFilters();
});
fc.addEventListener("input",function(e){
  if(!e.target.classList||!e.target.classList.contains("comment"))return;
  state.comments[e.target.closest(".card").dataset.id]=e.target.value;
  save();
});

document.getElementById("stfilter").addEventListener("change",function(e){stF=e.target.value;applyFilters();});

function effStatus(id){
  var v=state.verdicts[id];
  if(v)return v;
  return (state.comments[id]||"").trim()?"discuss":"";
}
function updateProg(){
  var decided=0;
  FINDINGS.forEach(function(f){if(effStatus(f.id))decided++;});
  document.getElementById("prog").textContent="已处理 "+decided+" / "+FINDINGS.length;
}
function applyFilters(){
  updateProg();
  document.querySelectorAll("#findings .card").forEach(function(card){
    var id=card.dataset.id;
    var okSev=(!sevF)||card.dataset.sev===sevF;
    var eff=effStatus(id);
    var okSt=(!stF)||(stF==="none"?!eff:eff===stF);
    card.classList.toggle("hidden",!(okSev&&okSt));
  });
}

function buildFeedback(){
  var verdicts={};
  FINDINGS.forEach(function(f){
    var v=state.verdicts[f.id];
    var c=state.comments[f.id]||"";
    if(!v&&!c.trim())return;
    verdicts[f.id]={verdict:v||"discuss",comment:c,severity:f.severity,
                    file:f.file,line:f.line||null,title:f.title,fix:f.fix||""};
  });
  return {report:REV.report_name||"",findings_file:REV.findings_file||"",repo:REV.repo||"",
          scope:REV.scope||"",exported_at:new Date().toISOString(),
          total_findings:FINDINGS.length,decided:Object.keys(verdicts).length,verdicts:verdicts};
}
function feedbackText(){return JSON.stringify(buildFeedback(),null,2);}
function showModal(t){
  document.getElementById("modal-ta").value=t;
  document.getElementById("modal").classList.remove("hidden");
  document.getElementById("modal-ta").select();
}
function flash(msg){
  var b=document.getElementById("btn-export"),old=b.textContent;
  b.textContent=msg;
  setTimeout(function(){b.textContent=old;},1400);
}
document.getElementById("btn-export").onclick=function(){
  var name=REV.feedback_file||"code-review-report-feedback.json";
  var blob=new Blob([feedbackText()],{type:"application/json"});
  var a=document.createElement("a");
  a.href=URL.createObjectURL(blob);a.download=name;
  document.body.appendChild(a);a.click();
  setTimeout(function(){URL.revokeObjectURL(a.href);a.remove();},500);
  flash("已导出 ✓");
};
document.getElementById("btn-copy").onclick=function(){
  var t=feedbackText();
  if(navigator.clipboard&&navigator.clipboard.writeText){
    navigator.clipboard.writeText(t).then(function(){flash("已复制 ✓");},function(){showModal(t);});
  }else{showModal(t);}
};
document.getElementById("modal-close").onclick=function(){
  document.getElementById("modal").classList.add("hidden");
};

function fallbackCopy(t){
  var ta=document.createElement("textarea");
  ta.value=t;ta.style.position="fixed";ta.style.opacity="0";
  document.body.appendChild(ta);ta.select();
  try{document.execCommand("copy");}catch(e){}
  ta.remove();
}
function copyText(t,ok){
  if(navigator.clipboard&&navigator.clipboard.writeText){
    navigator.clipboard.writeText(t).then(ok,function(){fallbackCopy(t);ok();});
  }else{fallbackCopy(t);ok();}
}
document.addEventListener("click",function(e){
  var b=e.target.closest?e.target.closest(".fid"):null;
  if(!b||!b.dataset.copy)return;
  copyText(b.dataset.copy,function(){
    b.classList.add("copied");
    setTimeout(function(){b.classList.remove("copied");},1200);
  });
});

applyFilters();
})();
</script>
</body>
</html>
"""


def normalize(data, findings_path, out_path):
    """Validate/normalize the findings payload and inject report metadata.

    Hard gates (防遗漏): a findings file missing scope/coverage/summary or a
    finding missing file/title/detail/fix is REJECTED with a Chinese error
    message — weaker executors then have to go back and fill the gap instead
    of silently rendering an incomplete report.
    """
    if not isinstance(data, dict):
        raise SystemExit("findings JSON root must be an object")
    findings = data.get("findings")
    if not isinstance(findings, list) or not findings:
        raise SystemExit("findings JSON must contain a non-empty 'findings' list")

    norm = []
    for i, f in enumerate(findings, 1):
        if not isinstance(f, dict):
            raise SystemExit(f"finding #{i} must be an object")
        f = dict(f)
        f.setdefault("id", f"F{i}")
        sev = str(f.get("severity", "Minor")).strip().capitalize()
        f["severity"] = sev if sev in SEVERITIES else "Minor"
        norm.append(f)
    data["findings"] = norm

    systemic = data.get("systemic")
    if isinstance(systemic, list):
        for i, p in enumerate(systemic, 1):
            if isinstance(p, dict):
                p.setdefault("id", f"S{i}")
        data["systemic"] = systemic

    review = dict(data.get("review") or {})
    summary = str(data.get("summary", "")).strip()
    scope = str(review.get("scope", "")).strip()
    coverage = str(review.get("coverage", "")).strip()

    problems = []
    if not scope:
        problems.append("review.scope 为空——写明审查范围（整仓 / 模块 / diff / 单文件）")
    if len(coverage) < 8:
        problems.append("review.coverage 缺失——覆盖声明必须同时写明「覆盖了什么」和「没覆盖什么」")
    if not summary:
        problems.append("summary 缺失——总体评估 3-5 句（整体健康度 + 最需要关注的事）")
    for i, f in enumerate(norm, 1):
        for key in ("file", "title", "detail", "fix"):
            if not str(f.get(key, "")).strip():
                problems.append(f"finding {f.get('id', f'#{i}')} 缺少必填字段 '{key}'"
                                "——每条发现必须有位置、问题描述与具体修复建议")
    if problems:
        raise SystemExit("findings JSON 未通过硬校验（防遗漏门槛），请补齐后重试：\n- "
                         + "\n- ".join(problems))

    now = datetime.now()
    review.setdefault("date", now.strftime("%Y-%m-%d"))
    review["generated_at"] = now.strftime("%Y-%m-%d %H:%M:%S")
    review["report_name"] = out_path.name
    review["feedback_file"] = out_path.stem + "-feedback.json"
    review["findings_file"] = findings_path.resolve().as_posix()
    data["review"] = review
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("findings", help="findings JSON path")
    ap.add_argument("--out", default=None,
                    help="output HTML path (default: code-review-report.html next to the findings file)")
    args = ap.parse_args()

    findings_path = Path(args.findings)
    if not findings_path.is_file():
        raise SystemExit(f"findings file not found: {findings_path}")
    data = json.loads(findings_path.read_text(encoding="utf-8-sig"))

    out_path = Path(args.out) if args.out else findings_path.parent / "code-review-report.html"
    data = normalize(data, findings_path, out_path)

    payload = json.dumps(data, ensure_ascii=False, indent=1).replace("</", "<\\/")
    out_path.write_text(TEMPLATE.replace("__REVIEW_DATA__", payload), encoding="utf-8")

    print(f"report   : {out_path.resolve()}")
    print(f"feedback : {(out_path.parent / (out_path.stem + '-feedback.json')).name}  (由浏览器内「导出反馈 JSON」生成，放回本目录)")
    print(f"findings : {findings_path.resolve()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
