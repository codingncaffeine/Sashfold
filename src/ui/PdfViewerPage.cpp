// The PDF viewer's page: the toolbar, the sidebar of thumbnails and the
// outline, and the pages — each a box the size of the page at the zoom,
// its picture asked for as it comes near the view and let go when it goes
// far, its text laid over it in transparent runs (so selecting, copying and
// find work as on any page) and its links as anchors. Everything it needs
// of the document it fetches from the viewer's own scheme (PdfViewer.cpp).

#include <string>

namespace sashfold::ui {

std::string pdf_viewer_html()
{
    return R"VIEWER(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width">
<title>@@TITLE@@</title>
<style>
:root {
  --backdrop: #525659;
  --bar: #323639;
  --side: #3a3d40;
  --ink: #f1f3f4;
  --muted: #bdc1c6;
  --hover: rgba(255, 255, 255, 0.12);
  --field: #202124;
  --accent: #8ab4f8;
  --selection: rgba(0, 92, 230, 0.32);
}
html, body { margin: 0; padding: 0; }
body { background: var(--backdrop); color: var(--ink); font: 13px system-ui, "Noto Sans", "DejaVu Sans", sans-serif; }
#bar { position: fixed; top: 0; left: 0; right: 0; height: 48px; background: var(--bar); display: flex; align-items: center;
  padding: 0 8px; box-shadow: 0 1px 4px rgba(0, 0, 0, 0.45); z-index: 10; }
#bar .title { flex: 1; min-width: 0; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; font-size: 14px; padding: 0 8px; }
/* A narrow viewer (a frame, an embed) gives the title's room to the controls, as Chrome's does. */
@media (max-width: 560px) { #bar .title { visibility: hidden; } }
#bar .group { display: flex; align-items: center; padding: 0 6px; }
#bar .sep { width: 1px; height: 20px; background: rgba(255, 255, 255, 0.2); margin: 0 6px; }
button { background: transparent; border: 0; color: var(--ink); width: 36px; height: 36px; border-radius: 18px; padding: 0;
  display: flex; align-items: center; justify-content: center; cursor: pointer; }
button:hover { background: var(--hover); }
button svg { width: 20px; height: 20px; fill: currentColor; }
input#page { width: 36px; background: var(--field); color: var(--ink); border: 0; border-radius: 3px; text-align: center;
  font: inherit; padding: 4px 2px; }
#count { color: var(--muted); padding: 0 6px; white-space: nowrap; }
select#zoom { background: var(--field); color: var(--ink); border: 0; border-radius: 3px; font: inherit; padding: 4px; }
#side { position: fixed; top: 48px; bottom: 0; left: 0; width: 216px; background: var(--side); overflow-y: auto; display: none;
  z-index: 5; box-shadow: 1px 0 4px rgba(0, 0, 0, 0.35); }
body.side-open #side { display: block; }
body.side-open #pages { padding-left: 216px; }
#tabs { display: flex; padding: 8px 8px 0; }
#tabs button { width: auto; height: 28px; border-radius: 14px; padding: 0 12px; color: var(--muted); }
#tabs button.on { color: var(--ink); background: var(--hover); }
#thumbs { display: flex; flex-direction: column; align-items: center; padding: 12px 0; }
.thumb { margin: 6px 0 2px; background: #fff; border: 2px solid transparent; cursor: pointer; }
.thumb.current { border-color: var(--accent); }
.thumb img { display: block; width: 100%; height: 100%; }
.thumb-label { color: var(--muted); font-size: 12px; margin-bottom: 6px; }
#outline { display: none; padding: 8px 4px 16px 4px; }
#outline ul { list-style: none; margin: 0; padding-left: 14px; }
#outline > ul { padding-left: 4px; }
#outline li { margin: 0; }
#outline a { display: block; color: var(--ink); text-decoration: none; padding: 5px 6px; border-radius: 4px; }
#outline a:hover { background: var(--hover); }
#pages { padding: 64px 0 24px; }
.page { position: relative; background: #fff; margin: 0 auto 8px; box-shadow: 0 2px 6px rgba(0, 0, 0, 0.45); overflow: hidden; }
.page > img { position: absolute; left: 0; top: 0; width: 100%; height: 100%; }
.layers { position: absolute; left: 0; top: 0; }
.text { position: absolute; left: 0; top: 0; color: transparent; line-height: 1; }
.text span { position: absolute; white-space: pre; cursor: text; }
.text span::selection { background: var(--selection); }
.text br::selection { background: transparent; }
.links a { position: absolute; display: block; }
#notice { position: fixed; top: 48px; left: 0; right: 0; bottom: 0; display: none; align-items: center; justify-content: center; }
#notice.on { display: flex; }
#toast { position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%); background: var(--bar); color: var(--ink);
  padding: 10px 18px; border-radius: 6px; box-shadow: 0 4px 16px rgba(0, 0, 0, 0.5); font-size: 14px; display: none; }
#toast.on { display: block; }
#notice form, #notice div.box { background: var(--bar); padding: 20px 24px; border-radius: 8px; box-shadow: 0 4px 16px rgba(0, 0, 0, 0.5);
  display: flex; flex-direction: column; gap: 12px; min-width: 280px; }
#notice input { background: var(--field); color: var(--ink); border: 1px solid #5f6368; border-radius: 4px; padding: 8px; font: inherit; }
#notice .wrong { color: #f28b82; display: none; }
#notice button.wide { width: auto; height: 32px; border-radius: 4px; background: var(--accent); color: #202124; padding: 0 16px;
  align-self: flex-end; font: inherit; font-weight: 600; }
</style>
</head>
<body>
<div id="bar">
  <button id="toggle-side" title="Thumbnails and outline" aria-label="Thumbnails and outline">
    <svg viewBox="0 0 24 24"><path d="M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z"/></svg>
  </button>
  <div class="title" inert>@@TITLE@@</div>
  <div class="group">
    <input id="page" name="page" value="1" inputmode="numeric" autocomplete="off" aria-label="Page number"><span id="count">/ 0</span>
  </div>
  <div class="sep"></div>
  <div class="group">
    <button id="zoom-out" title="Zoom out (Ctrl+-)" aria-label="Zoom out"><svg viewBox="0 0 24 24"><path d="M19 13H5v-2h14v2z"/></svg></button>
    <select id="zoom" name="zoom" aria-label="Zoom">
      <option value="auto">Automatic</option>
      <option value="page-fit">Fit page</option>
      <option value="page-width">Fit width</option>
      <option value="0.5">50%</option>
      <option value="0.75">75%</option>
      <option value="1">100%</option>
      <option value="1.25">125%</option>
      <option value="1.5">150%</option>
      <option value="2">200%</option>
      <option value="3">300%</option>
      <option value="4">400%</option>
      <option value="custom" hidden></option>
    </select>
    <button id="zoom-in" title="Zoom in (Ctrl++)" aria-label="Zoom in"><svg viewBox="0 0 24 24"><path d="M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z"/></svg></button>
  </div>
  <div class="sep"></div>
  <div class="group">
    <button id="rotate" title="Rotate clockwise (Ctrl+])" aria-label="Rotate clockwise"><svg viewBox="0 0 24 24"><path d="M15.55 5.55L11 1v3.07C7.06 4.56 4 7.92 4 12s3.05 7.44 7 7.93v-2.02c-2.84-.48-5-2.94-5-5.91s2.16-5.43 5-5.91V10l4.55-4.45zM19.93 11c-.17-1.39-.72-2.73-1.62-3.89l-1.42 1.42c.54.75.88 1.6 1.02 2.47h2.02zM13 17.9v2.02c1.39-.17 2.74-.71 3.9-1.61l-1.44-1.44c-.75.54-1.59.89-2.46 1.03zm3.89-2.42l1.42 1.41c.9-1.16 1.45-2.5 1.62-3.89h-2.02c-.14.87-.48 1.72-1.02 2.48z"/></svg></button>
    <button id="download" title="Download" aria-label="Download"><svg viewBox="0 0 24 24"><path d="M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z"/></svg></button>
    <button id="print" title="Print (Ctrl+P)" aria-label="Print"><svg viewBox="0 0 24 24"><path d="M19 8H5c-1.66 0-3 1.34-3 3v6h4v4h12v-4h4v-6c0-1.66-1.34-3-3-3zm-3 11H8v-5h8v5zm3-7c-.55 0-1-.45-1-1s.45-1 1-1 1 .45 1 1-.45 1-1 1zm-1-9H6v4h12V3z"/></svg></button>
  </div>
</div>
<div id="side">
  <div id="tabs"><button id="tab-thumbs" class="on">Pages</button><button id="tab-outline">Outline</button></div>
  <div id="thumbs"></div>
  <div id="outline"></div>
</div>
<div id="pages"></div>
<div id="notice"></div>
<div id="toast" role="status"></div>
<script>
"use strict";
const BASE = "sashfold-pdf://@@ID@@/";
const BAR = 48;
const GAP = 8;
const ZOOMS = [0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2, 2.5, 3, 4, 5];
let info = null;
let zoom = 1;
let mode = "auto";
let rotation = 0;
const pages = [];
let observer = null;
let thumbObserver = null;

const $ = (id) => document.getElementById(id);

async function json(path, options) {
  const response = await fetch(BASE + path, options);
  if (!response.ok)
    throw new Error("" + response.status);
  return response.json();
}

function turned() { return rotation % 180 !== 0; }
function pageWidth(p) { return turned() ? p.h : p.w; }
function pageHeight(p) { return turned() ? p.w : p.h; }
function wantedScale() { return Math.round(zoom * (window.devicePixelRatio || 1) * 100) / 100; }
// A page's picture is at most this many pixels (pdf.js's limit); past it the
// picture is drawn smaller and stretched to the page's size.
const MAX_PIXELS = 16777216;
function pictureScale(p) {
  const wanted = wantedScale();
  const most = Math.sqrt(MAX_PIXELS / Math.max(1, p.w * p.h));
  return wanted > most ? Math.floor(most * 100) / 100 : wanted;
}

function current() {
  const middle = window.scrollY + BAR + (window.innerHeight - BAR) / 3;
  let best = 0;
  for (let i = 0; i < pages.length; i++) {
    if (pages[i].el.offsetTop <= middle)
      best = i;
    else
      break;
  }
  return best;
}

function fit(kind) {
  if (!pages.length)
    return 1;
  const p = pages[current()];
  const side = document.body.classList.contains("side-open") ? 216 : 0;
  const width = (document.documentElement.clientWidth || window.innerWidth) - side - 40;
  const height = window.innerHeight - BAR - 24;
  const byWidth = width / pageWidth(p);
  const byPage = Math.min(byWidth, height / pageHeight(p));
  if (kind === "page-width")
    return byWidth;
  if (kind === "page-fit")
    return byPage;
  // Automatic: the width, but never larger than 125%, and a landscape page whole.
  return pageWidth(p) > pageHeight(p) ? Math.min(byPage, 1.25) : Math.min(byWidth, 1.25);
}

function showZoom() {
  const select = $("zoom");
  if (mode !== "custom") {
    select.value = mode;
  } else {
    const preset = Array.from(select.options).find((o) => Math.abs(parseFloat(o.value) - zoom) < 0.001);
    if (preset) {
      select.value = preset.value;
    } else {
      const custom = select.querySelector("option[value=custom]");
      custom.textContent = Math.round(zoom * 100) + "%";
      custom.hidden = false;
      select.value = "custom";
    }
  }
}

function setZoom(value, newMode) {
  const anchor = current();
  const offsetInPage = window.scrollY + BAR - pages[anchor].el.offsetTop;
  const ratio = pages[anchor].el.offsetHeight ? offsetInPage / pages[anchor].el.offsetHeight : 0;
  mode = newMode || "custom";
  zoom = Math.min(8, Math.max(0.1, value));
  for (const p of pages)
    size(p);
  showZoom();
  // The same place stays in view.
  window.scrollTo(window.scrollX, pages[anchor].el.offsetTop - BAR + ratio * pages[anchor].el.offsetHeight);
  for (const p of pages) {
    if (p.visible)
      show(p);
  }
}

function zoomStep(direction) {
  const next = direction > 0 ? ZOOMS.find((z) => z > zoom + 0.001) : ZOOMS.slice().reverse().find((z) => z < zoom - 0.001);
  if (next)
    setZoom(next, "custom");
}

function size(p) {
  const w = Math.round(pageWidth(p) * zoom);
  const h = Math.round(pageHeight(p) * zoom);
  p.el.style.width = w + "px";
  p.el.style.height = h + "px";
  // The layers are in the page's own space (its /Rotate applied), turned with the view.
  const lw = p.w * zoom;
  const lh = p.h * zoom;
  p.layers.style.width = lw + "px";
  p.layers.style.height = lh + "px";
  const turns = {
    0: "",
    90: "translate(" + lh + "px, 0px) rotate(90deg)",
    180: "translate(" + lw + "px, " + lh + "px) rotate(180deg)",
    270: "translate(0px, " + lw + "px) rotate(270deg)",
  };
  p.layers.style.transform = turns[rotation];
  p.layers.style.transformOrigin = "0 0";
  if (p.items)
    placeText(p);
  if (p.linkItems)
    placeLinks(p);
}

function show(p) {
  const scale = pictureScale(p);
  if (p.scale !== scale || p.rotation !== rotation) {
    p.scale = scale;
    p.rotation = rotation;
    const img = new Image();
    img.alt = "";
    img.decoding = "async";
    img.addEventListener("load", () => {
      if (p.scale !== scale || p.rotation !== rotation || !p.visible)
        return;
      if (p.img && p.img !== img)
        p.img.remove();
      p.img = img;
    });
    p.el.insertBefore(img, p.layers);
    img.src = BASE + "page/" + p.index + ".bmp?scale=" + scale + "&rotate=" + rotation;
  }
  if (!p.textAsked) {
    p.textAsked = true;
    json("page/" + p.index + "/text.json").then((items) => { p.items = items; buildText(p); }).catch(() => {});
    json("page/" + p.index + "/links.json").then((items) => { p.linkItems = items; buildLinks(p); }).catch(() => {});
  }
}

function hide(p) {
  // Far from the view: its pictures are let go (the text stays, it is small).
  for (const img of p.el.querySelectorAll(":scope > img"))
    img.remove();
  p.img = null;
  p.scale = 0;
}

function buildText(p) {
  const layer = p.text;
  layer.textContent = "";
  p.spans = [];
  for (const item of p.items) {
    const span = document.createElement("span");
    span.textContent = item[0];
    layer.appendChild(span);
    p.spans.push(span);
    if (item[6])
      layer.appendChild(document.createElement("br"));
  }
  placeText(p);
}

function placeText(p) {
  if (!p.spans)
    return;
  // Each run where it is drawn, its letters spaced so it is as long as drawn.
  for (let i = 0; i < p.spans.length; i++) {
    const item = p.items[i];
    const span = p.spans[i];
    const height = item[4] * zoom;
    span.style.left = item[1] * zoom + "px";
    span.style.top = item[2] * zoom - height + "px";
    span.style.fontSize = height + "px";
    span.style.letterSpacing = "0px";
    if (item[5])
      span.style.transform = "rotate(" + item[5] + "rad)";
  }
  for (let i = 0; i < p.spans.length; i++) {
    const item = p.items[i];
    const span = p.spans[i];
    const chars = Math.max(1, Array.from(item[0]).length);
    const measured = span.offsetWidth;
    if (measured > 0)
      span.style.letterSpacing = (item[3] * zoom - measured) / chars + "px";
  }
}

function buildLinks(p) {
  const layer = p.links;
  layer.textContent = "";
  p.anchors = [];
  for (const link of p.linkItems) {
    const a = document.createElement("a");
    if (link.url !== undefined) {
      a.href = link.url;
      a.title = link.url;
      a.rel = "noopener noreferrer";
    } else {
      a.href = "#page=" + (link.page + 1);
      a.addEventListener("click", (event) => { event.preventDefault(); goTo(link.page, link.y); });
    }
    layer.appendChild(a);
    p.anchors.push(a);
  }
  placeLinks(p);
}

function placeLinks(p) {
  if (!p.anchors)
    return;
  for (let i = 0; i < p.anchors.length; i++) {
    const r = p.linkItems[i].r;
    const a = p.anchors[i];
    a.style.left = r[0] * zoom + "px";
    a.style.top = r[1] * zoom + "px";
    a.style.width = (r[2] - r[0]) * zoom + "px";
    a.style.height = (r[3] - r[1]) * zoom + "px";
  }
}

function goTo(index, y) {
  const p = pages[Math.max(0, Math.min(pages.length - 1, index))];
  if (!p)
    return;
  let top = p.el.offsetTop - BAR - GAP;
  if (typeof y === "number" && rotation === 0)
    top += y * zoom;
  window.scrollTo(window.scrollX, Math.max(0, top));
}

function rotate(by) {
  rotation = (rotation + by + 360) % 360;
  if (mode !== "custom")
    zoom = fit(mode);
  for (const p of pages) {
    size(p);
    if (p.visible)
      show(p);
  }
  showZoom();
  for (const t of document.querySelectorAll(".thumb")) {
    const p = pages[t.dataset.index];
    const scale = 150 / Math.max(pageWidth(p), pageHeight(p));
    t.style.width = pageWidth(p) * scale + "px";
    t.style.height = pageHeight(p) * scale + "px";
    const img = t.querySelector("img");
    if (img && img.src)
      img.src = BASE + "page/" + p.index + ".bmp?scale=" + Math.round(scale * (window.devicePixelRatio || 1) * 100) / 100 + "&rotate=" + rotation;
  }
}

function updateCurrent() {
  const index = current();
  const field = $("page");
  if (document.activeElement !== field)
    field.value = index + 1;
  for (const t of document.querySelectorAll(".thumb.current"))
    t.classList.remove("current");
  const thumb = document.querySelector('.thumb[data-index="' + index + '"]');
  if (thumb)
    thumb.classList.add("current");
}

function buildOutline(items, parent) {
  const list = document.createElement("ul");
  for (const item of items) {
    const li = document.createElement("li");
    const a = document.createElement("a");
    a.textContent = item.title || "(untitled)";
    if (item.url) {
      a.href = item.url;
    } else {
      a.href = "#page=" + ((item.page || 0) + 1);
      a.addEventListener("click", (event) => { event.preventDefault(); goTo(item.page || 0, item.y); });
    }
    li.appendChild(a);
    if (item.children)
      buildOutline(item.children, li);
    list.appendChild(li);
  }
  parent.appendChild(list);
}

function buildThumbs() {
  const holder = $("thumbs");
  thumbObserver = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (!entry.isIntersecting)
        continue;
      const t = entry.target;
      if (t.querySelector("img"))
        continue;
      const p = pages[t.dataset.index];
      const scale = 150 / Math.max(pageWidth(p), pageHeight(p));
      const img = new Image();
      img.alt = "";
      img.src = BASE + "page/" + p.index + ".bmp?scale=" + Math.round(scale * (window.devicePixelRatio || 1) * 100) / 100 + "&rotate=" + rotation;
      t.appendChild(img);
    }
  }, { root: $("side"), rootMargin: "300px 0px" });
  for (const p of pages) {
    const t = document.createElement("div");
    t.className = "thumb";
    t.dataset.index = p.index;
    const scale = 150 / Math.max(pageWidth(p), pageHeight(p));
    t.style.width = pageWidth(p) * scale + "px";
    t.style.height = pageHeight(p) * scale + "px";
    t.addEventListener("click", () => goTo(p.index));
    holder.appendChild(t);
    const label = document.createElement("div");
    label.className = "thumb-label";
    label.textContent = p.index + 1;
    holder.appendChild(label);
    thumbObserver.observe(t);
  }
}

function readHash() {
  const out = {};
  const hash = (location.hash || "").replace(/^#/, "");
  for (const part of hash.split("&")) {
    const [key, value] = part.split("=");
    if (key === "page" && value)
      out.page = parseInt(value, 10) - 1;
    else if (key === "zoom" && value)
      out.zoom = parseFloat(value.split(",")[0]) / 100;
  }
  return out;
}

function build() {
  document.title = info.title || info.fileName || document.title;
  document.querySelector("#bar .title").textContent = document.title;
  $("count").textContent = "/ " + info.pages.length;
  const holder = $("pages");
  observer = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      const p = pages[entry.target.dataset.index];
      p.visible = entry.isIntersecting;
      if (p.visible)
        show(p);
      else
        hide(p);
    }
  }, { rootMargin: "150% 0px" });
  info.pages.forEach((dims, index) => {
    const el = document.createElement("div");
    el.className = "page";
    el.dataset.index = index;
    const layers = document.createElement("div");
    layers.className = "layers";
    const text = document.createElement("div");
    text.className = "text";
    const links = document.createElement("div");
    links.className = "links";
    layers.appendChild(text);
    layers.appendChild(links);
    el.appendChild(layers);
    holder.appendChild(el);
    pages.push({ index, el, layers, text, links, w: dims[0], h: dims[1], img: null, scale: 0, visible: false });
  });
  const wanted = readHash();
  if (wanted.zoom)
    zoom = wanted.zoom, mode = "custom";
  else
    zoom = fit("auto");
  for (const p of pages) {
    size(p);
    observer.observe(p.el);
  }
  showZoom();
  if (info.outline && info.outline.length) {
    buildOutline(info.outline, $("outline"));
  } else {
    $("tab-outline").hidden = true;
  }
  buildThumbs();
  if (wanted.page)
    goTo(wanted.page);
  updateCurrent();
}

// A line at the foot for a moment: how printing went.
let toastTimer = 0;
function toast(text) {
  const box = $("toast");
  box.textContent = text;
  box.classList.add("on");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => box.classList.remove("on"), 4000);
}

// The system's print dialog, through the shell; its outcome is asked for
// until it has one.
async function printDocument() {
  const answer = await json("print", { method: "POST", body: "" });
  if (!answer.ok) {
    toast("Printing is not available on this system.");
    return;
  }
  const poll = async () => {
    const state = await json("print-status");
    if (state.status === "pending") {
      setTimeout(poll, 500);
      return;
    }
    if (state.status === "unavailable")
      toast("Printing is not available on this system.");
    else if (state.status === "failed")
      toast("The document could not be printed.");
  };
  setTimeout(poll, 500);
}

function askPassword(wrong) {
  const notice = $("notice");
  notice.textContent = "";
  const form = document.createElement("form");
  const label = document.createElement("div");
  label.textContent = "This document is protected by a password.";
  const field = document.createElement("input");
  field.type = "password";
  field.placeholder = "Password";
  field.autofocus = true;
  const bad = document.createElement("div");
  bad.className = "wrong";
  bad.textContent = "That password is not right.";
  if (wrong)
    bad.style.display = "block";
  const submit = document.createElement("button");
  submit.className = "wide";
  submit.type = "submit";
  submit.textContent = "Open";
  form.append(label, field, bad, submit);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const answer = await json("unlock", { method: "POST", body: field.value });
    if (answer.ok) {
      notice.classList.remove("on");
      start();
    } else {
      askPassword(true);
    }
  });
  notice.appendChild(form);
  notice.classList.add("on");
  field.focus();
}

function failed() {
  const notice = $("notice");
  const box = document.createElement("div");
  box.className = "box";
  box.textContent = "This PDF could not be opened: it is damaged or not a PDF.";
  notice.textContent = "";
  notice.appendChild(box);
  notice.classList.add("on");
}

async function start() {
  try {
    info = await json("info.json");
  } catch (error) {
    failed();
    return;
  }
  if (info.needsPassword) {
    askPassword(false);
    return;
  }
  if (info.error || !info.pages || !info.pages.length) {
    failed();
    return;
  }
  build();
}

$("toggle-side").addEventListener("click", () => {
  document.body.classList.toggle("side-open");
  if (mode !== "custom")
    setZoom(fit(mode), mode);
});
$("tab-thumbs").addEventListener("click", () => {
  $("thumbs").style.display = "flex";
  $("outline").style.display = "none";
  $("tab-thumbs").classList.add("on");
  $("tab-outline").classList.remove("on");
});
$("tab-outline").addEventListener("click", () => {
  $("thumbs").style.display = "none";
  $("outline").style.display = "block";
  $("tab-outline").classList.add("on");
  $("tab-thumbs").classList.remove("on");
});
$("zoom-in").addEventListener("click", () => zoomStep(1));
$("zoom-out").addEventListener("click", () => zoomStep(-1));
$("zoom").addEventListener("change", (event) => {
  const value = event.target.value;
  if (value === "auto" || value === "page-fit" || value === "page-width")
    setZoom(fit(value), value);
  else if (value !== "custom")
    setZoom(parseFloat(value), "custom");
});
$("rotate").addEventListener("click", () => rotate(90));
$("download").addEventListener("click", () => { location.href = BASE + "file"; });
$("print").addEventListener("click", () => printDocument());
$("page").addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    const n = parseInt(event.target.value, 10);
    if (n >= 1)
      goTo(n - 1);
    event.target.blur();
  }
});
$("page").addEventListener("focus", (event) => event.target.select());
document.addEventListener("keydown", (event) => {
  if (!event.ctrlKey && !event.metaKey) {
    if (event.target && event.target.tagName === "INPUT")
      return;
    if (mode === "page-fit" && (event.key === "ArrowRight" || event.key === "ArrowLeft")) {
      goTo(current() + (event.key === "ArrowRight" ? 1 : -1));
      event.preventDefault();
    }
    return;
  }
  if (event.key === "=" || event.key === "+") {
    zoomStep(1);
    event.preventDefault();
  } else if (event.key === "-") {
    zoomStep(-1);
    event.preventDefault();
  } else if (event.key === "0") {
    setZoom(fit("auto"), "auto");
    event.preventDefault();
  } else if (event.key === "]") {
    rotate(90);
    event.preventDefault();
  } else if (event.key === "[") {
    rotate(-90);
    event.preventDefault();
  } else if (event.key === "p" || event.key === "P") {
    printDocument();
    event.preventDefault();
  }
});
window.addEventListener("wheel", (event) => {
  if (!event.ctrlKey)
    return;
  event.preventDefault();
  zoomStep(event.deltaY < 0 ? 1 : -1);
}, { passive: false });
window.addEventListener("resize", () => {
  if (mode !== "custom" && pages.length)
    setZoom(fit(mode), mode);
});
window.addEventListener("scroll", () => updateCurrent(), { passive: true });
window.addEventListener("hashchange", () => {
  const wanted = readHash();
  if (wanted.zoom)
    setZoom(wanted.zoom, "custom");
  if (wanted.page !== undefined)
    goTo(wanted.page);
});
start();
</script>
</body>
</html>
)VIEWER";
}

}
