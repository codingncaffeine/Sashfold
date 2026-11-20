#include "pdf/FormScript.h"

namespace sashfold::pdf {

// The forms object model a PDF's scripts see (Adobe, JavaScript for
// Acrobat API Reference: event, Doc, Field, app, util, color, console,
// global, the display/border/style constants, and the AF functions of the
// forms documentation), run in a realm that has nothing else: the
// ECMAScript built-ins and this. Its state is the model the host hands
// it (form_script_model); each request (one event the viewer saw) comes
// back as what changed — values, formatted text, visibility, colors,
// items — with the alerts to show and console lines to print. Document
// scripts share the realm and may change anything here: they can only
// break their own form.
std::string_view form_script_prelude()
{
    static constexpr char const source[] = R"JS(
(function () {
"use strict";
const G = globalThis;
const stringify = JSON.stringify;
const parse = JSON.parse;

let model = null;
const data = new Map(); // name -> the field's state
let order = []; // field names in model order
let out = null; // the answer being built
let calculating = false;
let pendingCalculation = false;
const changed = new Set(); // fields whose value changed in this request
let timers = [];
let timerSerial = 0;

function begin() {
  out = { changes: {}, alerts: [], console: [], print: false };
  changed.clear();
  pendingCalculation = false;
}
function note(fd, key, value) {
  const entry = out.changes[fd.name] || (out.changes[fd.name] = {});
  entry[key] = value;
}
function log(text) {
  out.console.push(String(text));
}
function report(error, where) {
  let text;
  try {
    text = error && error.name ? error.name + ": " + error.message : String(error);
  } catch (e) {
    text = "error";
  }
  log(text + (where ? " (" + where + ")" : ""));
}

// ---- colors (API Reference: color object) ----
const colorObject = {
  transparent: ["T"],
  black: ["G", 0],
  white: ["G", 1],
  red: ["RGB", 1, 0, 0],
  green: ["RGB", 0, 1, 0],
  blue: ["RGB", 0, 0, 1],
  cyan: ["CMYK", 1, 0, 0, 0],
  magenta: ["CMYK", 0, 1, 0, 0],
  yellow: ["CMYK", 0, 0, 1, 0],
  dkGray: ["G", 0.25],
  gray: ["G", 0.5],
  ltGray: ["G", 0.75],
  convert(c, space) {
    if (!Array.isArray(c) || !c.length)
      return c;
    const kind = c[0];
    let rgb = null;
    if (kind === "G") rgb = [c[1], c[1], c[1]];
    else if (kind === "RGB") rgb = [c[1], c[2], c[3]];
    else if (kind === "CMYK") rgb = [1 - Math.min(1, c[1] + c[4]), 1 - Math.min(1, c[2] + c[4]), 1 - Math.min(1, c[3] + c[4])];
    if (space === "T" || kind === "T" || !rgb)
      return space === kind ? c.slice() : ["T"];
    if (space === "RGB") return ["RGB", rgb[0], rgb[1], rgb[2]];
    if (space === "G") return ["G", 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2]];
    if (space === "CMYK") {
      const k = 1 - Math.max(rgb[0], rgb[1], rgb[2]);
      return ["CMYK", 1 - rgb[0] - k, 1 - rgb[1] - k, 1 - rgb[2] - k, k];
    }
    return c.slice();
  },
  equal(a, b) {
    if (!Array.isArray(a) || !Array.isArray(b))
      return false;
    if (a[0] !== b[0])
      b = colorObject.convert(b, a[0]);
    if (a.length !== b.length)
      return false;
    for (let i = 1; i < a.length; i++) {
      if (Math.abs(a[i] - b[i]) > 1e-4)
        return false;
    }
    return a[0] === b[0];
  },
};
function colorOperators(c, stroke) {
  if (!Array.isArray(c) || c[0] === "T")
    return "";
  const n = (v) => String(Math.round(Math.min(1, Math.max(0, +v || 0)) * 10000) / 10000);
  if (c[0] === "G") return n(c[1]) + (stroke ? " G" : " g");
  if (c[0] === "RGB") return n(c[1]) + " " + n(c[2]) + " " + n(c[3]) + (stroke ? " RG" : " rg");
  if (c[0] === "CMYK") return n(c[1]) + " " + n(c[2]) + " " + n(c[3]) + " " + n(c[4]) + (stroke ? " K" : " k");
  return "";
}
function colorCss(c) {
  if (!Array.isArray(c) || c[0] === "T")
    return "";
  const rgb = colorObject.convert(c, "RGB");
  const b = (v) => Math.round(Math.min(1, Math.max(0, +v || 0)) * 255);
  return "rgb(" + b(rgb[1]) + ", " + b(rgb[2]) + ", " + b(rgb[3]) + ")";
}

// ---- util (API Reference: util object) ----
const MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"];
const DAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
function pad(n, width) {
  let s = String(Math.abs(Math.trunc(n)));
  while (s.length < width) s = "0" + s;
  return s;
}
// Digits with the separators of one of the five styles (§ util.printf's
// nDecSep: 0 "1,234.56", 1 "1234.56", 2 "1.234,56", 3 "1234,56", 4 "1'234.56").
function groupDigits(fixed, style) {
  const point = style === 2 || style === 3 ? "," : ".";
  const thousands = style === 0 ? "," : style === 2 ? "." : style === 4 ? "'" : "";
  let [whole, fraction] = fixed.split(".");
  if (thousands) {
    let grouped = "";
    while (whole.length > 3) {
      grouped = thousands + whole.slice(-3) + grouped;
      whole = whole.slice(0, -3);
    }
    whole += grouped;
  }
  return fraction !== undefined ? whole + point + fraction : whole;
}
function printf(format) {
  const args = Array.prototype.slice.call(arguments, 1);
  let next = 0;
  return String(format).replace(/%(,\d)?([+ 0#]*)(\d+)?(?:\.(\d+))?([dfsx%])/g, (all, sep, flags, width, precision, kind) => {
    if (kind === "%")
      return "%";
    const arg = args[next++];
    let text;
    if (kind === "s") {
      // A number as Acrobat writes one: fifteen significant digits.
      text = typeof arg === "number" && isFinite(arg) ? String(Number(arg.toPrecision(15))) : String(arg);
    } else if (kind === "x") {
      text = (Math.trunc(+arg) >>> 0).toString(16).toUpperCase();
    } else {
      let v = +arg;
      if (!isFinite(v)) v = 0;
      const negative = v < 0 || Object.is(v, -0);
      const digits = kind === "d" ? Math.trunc(Math.abs(v)).toString() : Math.abs(v).toFixed(precision === undefined ? 6 : +precision);
      text = groupDigits(digits, sep ? +sep[1] : 1);
      if (kind === "f" && precision === "0" && flags.includes("#"))
        text += sep && (sep[1] === "2" || sep[1] === "3") ? "," : ".";
      if (negative && (+text.replace(/[^0-9]/g, "")) !== 0) text = "-" + text;
      else if (flags.includes("+")) text = "+" + text;
      else if (flags.includes(" ")) text = " " + text;
    }
    if (width && text.length < +width) {
      const fill = flags.includes("0") && kind !== "s" ? "0" : " ";
      if (fill === "0") {
        const sign = /^[+\- ]/.test(text) ? text[0] : "";
        let body = sign ? text.slice(1) : text;
        while (body.length + sign.length < +width) body = "0" + body;
        text = sign + body;
      } else {
        while (text.length < +width) text = " " + text;
      }
    }
    return text;
  });
}
function printd(format, date) {
  if (!(date instanceof Date) || isNaN(date.getTime()))
    return "";
  if (typeof format === "number") {
    format = format === 0 ? "D:yyyymmddHHMMss" : format === 1 ? "yyyy.mm.dd HH:MM:ss" : "m/d/yy h:MM:ss tt";
  }
  const h12 = (h) => (h % 12 === 0 ? 12 : h % 12);
  return String(format).replace(/mmmm|mmm|mm|m|dddd|ddd|dd|d|yyyy|yy|HH|H|hh|h|MM|M|ss|s|tt|t|\\./g, (token) => {
    switch (token) {
    case "mmmm": return MONTHS[date.getMonth()];
    case "mmm": return MONTHS[date.getMonth()].slice(0, 3);
    case "mm": return pad(date.getMonth() + 1, 2);
    case "m": return String(date.getMonth() + 1);
    case "dddd": return DAYS[date.getDay()];
    case "ddd": return DAYS[date.getDay()].slice(0, 3);
    case "dd": return pad(date.getDate(), 2);
    case "d": return String(date.getDate());
    case "yyyy": return pad(date.getFullYear(), 4);
    case "yy": return pad(date.getFullYear() % 100, 2);
    case "HH": return pad(date.getHours(), 2);
    case "H": return String(date.getHours());
    case "hh": return pad(h12(date.getHours()), 2);
    case "h": return String(h12(date.getHours()));
    case "MM": return pad(date.getMinutes(), 2);
    case "M": return String(date.getMinutes());
    case "ss": return pad(date.getSeconds(), 2);
    case "s": return String(date.getSeconds());
    case "tt": return date.getHours() < 12 ? "am" : "pm";
    case "t": return date.getHours() < 12 ? "a" : "p";
    default: return token.slice(1); // a backslash's character, as written
    }
  });
}
function scand(format, text) {
  if (typeof text !== "string" && typeof text !== "number")
    return null;
  text = String(text);
  const parts = [];
  let pattern = "";
  const re = /mmmm|mmm|mm|m|dddd|ddd|dd|d|yyyy|yy|HH|H|hh|h|MM|M|ss|s|tt|t|\\.|[^]/g;
  let match;
  while ((match = re.exec(String(format))) !== null) {
    const token = match[0];
    const group = (p, kind) => { parts.push(kind); pattern += "(" + p + ")"; };
    switch (token) {
    case "mmmm": case "mmm": group("[A-Za-z]+", "monthName"); break;
    case "mm": case "m": group("\\d{1,2}", "month"); break;
    case "dddd": case "ddd": group("[A-Za-z]+", "dayName"); break;
    case "dd": case "d": group("\\d{1,2}", "day"); break;
    case "yyyy": group("\\d{4}", "year"); break;
    case "yy": group("\\d{2}", "year2"); break;
    case "HH": case "H": case "hh": case "h": group("\\d{1,2}", token[0] === "H" ? "hour" : "hour12"); break;
    case "MM": case "M": group("\\d{1,2}", "minute"); break;
    case "ss": case "s": group("\\d{1,2}", "second"); break;
    case "tt": group("[AaPp][Mm]", "ampm"); break;
    case "t": group("[AaPp]", "ampm"); break;
    default: {
      const ch = token.length === 2 ? token[1] : token;
      pattern += ch.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    }
    }
  }
  const found = new RegExp("^\\s*" + pattern + "\\s*$").exec(text);
  if (!found)
    return null;
  const now = new Date();
  let year = now.getFullYear(), month = 0, day = 1, hour = 0, minute = 0, second = 0, pm = null, h12 = false;
  for (let i = 0; i < parts.length; i++) {
    const v = found[i + 1];
    switch (parts[i]) {
    case "monthName": {
      const index = MONTHS.findIndex((name) => name.toLowerCase().startsWith(v.toLowerCase()) && v.length >= 3);
      if (index < 0) return null;
      month = index;
      break;
    }
    case "month": month = +v - 1; break;
    case "day": day = +v; break;
    case "year": year = +v; break;
    case "year2": year = +v < 50 ? 2000 + +v : 1900 + +v; break;
    case "hour": hour = +v; break;
    case "hour12": hour = +v; h12 = true; break;
    case "minute": minute = +v; break;
    case "second": second = +v; break;
    case "ampm": pm = /^p/i.test(v); break;
    }
  }
  if (h12 || pm !== null) {
    if (hour > 12 || hour === 0 && h12) return null;
    if (pm === true && hour < 12) hour += 12;
    if (pm === false && hour === 12) hour = 0;
  }
  if (month < 0 || month > 11 || day < 1 || hour > 23 || minute > 59 || second > 59)
    return null;
  const date = new Date(year, month, day, hour, minute, second);
  if (date.getMonth() !== month || date.getDate() !== day)
    return null;
  return date;
}
function printx(format, source) {
  source = String(source);
  let out = "", at = 0, mode = 0; // mode: 0 keep, 1 upper, -1 lower
  const cased = (c) => (mode > 0 ? c.toUpperCase() : mode < 0 ? c.toLowerCase() : c);
  const take = (test) => {
    while (at < source.length && !test(source[at])) at++;
    return at < source.length ? source[at++] : null;
  };
  format = String(format);
  for (let i = 0; i < format.length; i++) {
    const f = format[i];
    let c = null;
    switch (f) {
    case "?": c = at < source.length ? source[at++] : null; if (c !== null) out += cased(c); break;
    case "X": c = take((x) => /[0-9A-Za-z]/.test(x)); if (c !== null) out += cased(c); break;
    case "A": c = take((x) => /[A-Za-z]/.test(x)); if (c !== null) out += cased(c); break;
    case "9": c = take((x) => /[0-9]/.test(x)); if (c !== null) out += c; break;
    case "*": out += cased(source.slice(at)); at = source.length; break;
    case "\\": if (i + 1 < format.length) out += format[++i]; break;
    case ">": mode = 1; break;
    case "<": mode = -1; break;
    case "=": mode = 0; break;
    default: out += f;
    }
  }
  return out;
}
const utilObject = {
  printf, printd, scand, printx,
  byteToChar(n) { return String.fromCharCode(n); },
  crackURL(url) {
    const m = /^([a-z][a-z0-9+.-]*):\/\/(?:([^:@\/]*)(?::([^@\/]*))?@)?([^:\/?#]*)(?::(\d+))?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$/i.exec(String(url));
    if (!m) throw new TypeError("not a URL");
    return { cScheme: m[1], cUser: m[2], cPassword: m[3], cHost: m[4], nPort: m[5] ? +m[5] : undefined, cPath: m[6], cParameters: m[7], cFragments: m[8] };
  },
};

// ---- the field model ----
const TYPES = { text: "text", checkbox: "checkbox", radio: "radiobutton", combo: "combobox", list: "listbox", button: "button", signature: "signature" };
const TRIGGERS = { Keystroke: "K", Format: "F", Validate: "V", Calculate: "C", MouseUp: "U", MouseDown: "D", MouseEnter: "E", MouseExit: "X", OnFocus: "Fo", OnBlur: "Bl" };
const NUMERIC = /^\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?\s*$/;
const fieldObjects = new Map();

function valueOut(fd) {
  const v = fd.value;
  if (Array.isArray(v)) return v.slice();
  if ((fd.type === "text" || fd.type === "combo") && typeof v === "string" && NUMERIC.test(v))
    return Number(v);
  return v;
}
function asStored(fd, v) {
  if (fd.type === "list" && Array.isArray(v)) return v.map(String);
  if (v === null || v === undefined) return "";
  return String(v);
}
function setValue(fd, v) {
  const stored = asStored(fd, v);
  if (stringify(stored) === stringify(fd.value))
    return;
  fd.value = stored;
  changed.add(fd.name);
  if (!calculating)
    pendingCalculation = true;
}
function displayCode(fd) {
  return fd.display;
}

class Field {
  constructor(fd, widget) {
    Object.defineProperty(this, "_fd", { value: fd });
    Object.defineProperty(this, "_widget", { value: widget === undefined ? -1 : widget });
  }
  get name() { return this._fd.name; }
  get type() { return TYPES[this._fd.type] || "text"; }
  get value() { return valueOut(this._fd); }
  set value(v) {
    if (this._fd.type === "checkbox" || this._fd.type === "radio") {
      const on = this._fd.widgets.some((w) => w.exportValue === String(v));
      v = on ? String(v) : "Off";
    }
    setValue(this._fd, v);
  }
  get valueAsString() { const v = this._fd.value; return Array.isArray(v) ? v.join(",") : String(v); }
  get defaultValue() { return this._fd.defaultValue; }
  set defaultValue(v) { this._fd.defaultValue = String(v); }
  get readonly() { return this._fd.readonly; }
  set readonly(v) { this._fd.readonly = !!v; note(this._fd, "readonly", !!v); }
  get required() { return this._fd.required; }
  set required(v) { this._fd.required = !!v; note(this._fd, "required", !!v); }
  get display() { return displayCode(this._fd); }
  set display(v) {
    v = +v;
    this._fd.display = v;
    note(this._fd, "hidden", v === 1 || v === 3);
    note(this._fd, "noPrint", v === 2);
  }
  get hidden() { return this._fd.display === 1; }
  set hidden(v) { this.display = v ? 1 : 0; }
  get print() { return this._fd.display !== 2; }
  set print(v) { this.display = v ? 0 : 2; }
  get fillColor() { return this._fd.fillColor.slice(); }
  set fillColor(c) { this._fd.fillColor = c; note(this._fd, "fillColor", colorCss(c)); }
  get bgColor() { return this.fillColor; }
  set bgColor(c) { this.fillColor = c; }
  get textColor() { return this._fd.textColor.slice(); }
  set textColor(c) {
    this._fd.textColor = c;
    note(this._fd, "textColor", colorCss(c));
    note(this._fd, "textColorOperators", colorOperators(c, false));
  }
  get fgColor() { return this.textColor; }
  set fgColor(c) { this.textColor = c; }
  get strokeColor() { return this._fd.strokeColor.slice(); }
  set strokeColor(c) { this._fd.strokeColor = c; note(this._fd, "strokeColor", colorCss(c)); }
  get borderColor() { return this.strokeColor; }
  set borderColor(c) { this.strokeColor = c; }
  get borderStyle() { return this._fd.borderStyle; }
  set borderStyle(v) { this._fd.borderStyle = String(v); note(this._fd, "borderStyle", String(v)); }
  get lineWidth() { return this._fd.lineWidth; }
  set lineWidth(v) { this._fd.lineWidth = +v; note(this._fd, "lineWidth", +v); }
  get textSize() { return this._fd.textSize; }
  set textSize(v) { this._fd.textSize = +v; note(this._fd, "textSize", +v); }
  get textFont() { return this._fd.textFont; }
  set textFont(v) { this._fd.textFont = String(v); note(this._fd, "textFont", String(v)); }
  get alignment() { return this._fd.alignment; }
  set alignment(v) { this._fd.alignment = String(v); note(this._fd, "alignment", String(v)); }
  get multiline() { return this._fd.multiline; }
  set multiline(v) { this._fd.multiline = !!v; }
  get charLimit() { return this._fd.charLimit; }
  set charLimit(v) { this._fd.charLimit = +v; note(this._fd, "charLimit", +v); }
  get comb() { return this._fd.comb; }
  set comb(v) { this._fd.comb = !!v; }
  get editable() { return this._fd.editable; }
  set editable(v) { this._fd.editable = !!v; }
  get password() { return this._fd.password; }
  get doNotScroll() { return this._fd.doNotScroll; }
  set doNotScroll(v) { this._fd.doNotScroll = !!v; }
  get doNotSpellCheck() { return this._fd.doNotSpellCheck; }
  set doNotSpellCheck(v) { this._fd.doNotSpellCheck = !!v; }
  get multipleSelection() { return this._fd.multipleSelection; }
  set multipleSelection(v) { this._fd.multipleSelection = !!v; }
  get userName() { return this._fd.userName; }
  set userName(v) { this._fd.userName = String(v); }
  get page() {
    const pages = this._fd.widgets.map((w) => w.page);
    return this._widget >= 0 ? pages[this._widget] : pages.length === 1 ? pages[0] : pages;
  }
  get rect() {
    const w = this._fd.widgets[this._widget >= 0 ? this._widget : 0];
    return w ? w.rect.slice() : [0, 0, 0, 0];
  }
  set rect(v) { const w = this._fd.widgets[this._widget >= 0 ? this._widget : 0]; if (w) w.rect = v.slice(0, 4); }
  get exportValues() { return this._fd.widgets.map((w) => w.exportValue); }
  set exportValues(v) { this._fd.widgets.forEach((w, i) => { if (v[i] !== undefined) w.exportValue = String(v[i]); }); }
  get numItems() { return this._fd.options.length; }
  get currentValueIndices() {
    const v = this._fd.value;
    const values = Array.isArray(v) ? v : [v];
    const indices = values.map((x) => this._fd.options.findIndex((o) => o[0] === x)).filter((i) => i >= 0);
    return Array.isArray(v) && v.length > 1 ? indices : indices.length ? indices[0] : -1;
  }
  set currentValueIndices(v) {
    const list = Array.isArray(v) ? v : [v];
    const values = list.map((i) => this._fd.options[i]).filter(Boolean).map((o) => o[0]);
    setValue(this._fd, this._fd.type === "list" && values.length > 1 ? values : values[0] === undefined ? "" : values[0]);
  }
  getItemAt(n, exportValue) {
    const o = this._fd.options[n < 0 ? this._fd.options.length - 1 : n];
    if (!o) throw new RangeError("no item " + n);
    return exportValue === false ? o[1] : o[0];
  }
  setItems(items) {
    this._fd.options = Array.from(items, (item) => Array.isArray(item) ? [String(item[1] === undefined ? item[0] : item[1]), String(item[0])] : [String(item), String(item)]);
    note(this._fd, "items", this._fd.options);
  }
  insertItemAt(cName, cExport, nIdx) {
    const item = [String(cExport === undefined ? cName : cExport), String(cName)];
    const at = nIdx === undefined || nIdx < 0 || nIdx > this._fd.options.length ? 0 : nIdx;
    this._fd.options.splice(at, 0, item);
    note(this._fd, "items", this._fd.options);
  }
  deleteItemAt(nIdx) {
    if (!this._fd.options.length) return;
    const at = nIdx === undefined || nIdx < 0 ? this.currentValueIndices : nIdx;
    this._fd.options.splice(at < 0 ? 0 : at, 1);
    note(this._fd, "items", this._fd.options);
  }
  clearItems() { this._fd.options = []; note(this._fd, "items", []); }
  getArray() {
    return order.filter((name) => name.startsWith(this._fd.name + ".")).map((name) => fieldObject(name));
  }
  getLock() { return false; }
  setLock() { return false; }
  setFocus() { out.focus = this._fd.name; }
  setAction(trigger, script) {
    const key = TRIGGERS[trigger];
    if (!key) throw new TypeError("no trigger " + trigger);
    const target = ["U", "D", "E", "X", "Fo", "Bl"].includes(key) ? (this._fd.widgets[0] || { actions: {} }).actions : this._fd.actions;
    target[key] = [{ js: String(script) }];
  }
  buttonGetCaption() { return this._fd.caption; }
  buttonSetCaption(text) { this._fd.caption = String(text); note(this._fd, "caption", String(text)); }
  buttonGetIcon() { return null; }
  buttonSetIcon() {}
  buttonImportIcon() { return -1; }
  checkThisBox(nWidget, bCheckIt) {
    const w = this._fd.widgets[nWidget];
    if (!w) return;
    if (bCheckIt === false) {
      if (this._fd.value === w.exportValue) setValue(this._fd, "Off");
    } else {
      setValue(this._fd, w.exportValue);
    }
  }
  isBoxChecked(nWidget) {
    const w = this._fd.widgets[nWidget];
    return !!w && this._fd.value === w.exportValue;
  }
  isDefaultChecked(nWidget) {
    const w = this._fd.widgets[nWidget];
    return !!w && this._fd.defaultValue === w.exportValue;
  }
  defaultIsChecked(nWidget, bIsDefaultChecked) {
    const w = this._fd.widgets[nWidget];
    if (!w) return false;
    this._fd.defaultValue = bIsDefaultChecked === false ? "Off" : w.exportValue;
    return true;
  }
  signatureInfo() { return { status: 0 }; }
  signatureValidate() { return 0; }
  signatureSign() { return false; }
  browseForFileToSubmit() {}
}
// A name that is a node of the field tree, not a terminal: the group.
function groupData(name) {
  return { name, type: "text", value: "", defaultValue: "", options: [], widgets: [], actions: {}, group: true,
    display: 0, fillColor: ["T"], textColor: ["G", 0], strokeColor: ["T"] };
}
function fieldObject(name) {
  if (fieldObjects.has(name))
    return fieldObjects.get(name);
  let fd = data.get(name);
  if (!fd) {
    // "name.0": the first widget of the field "name".
    const widget = /^(.*)\.(\d+)$/.exec(name);
    if (widget && data.has(widget[1])) {
      const object = new Field(data.get(widget[1]), +widget[2]);
      fieldObjects.set(name, object);
      return object;
    }
    if (!order.some((n) => n.startsWith(name + ".")))
      return null;
    fd = groupData(name);
  }
  const object = new Field(fd);
  fieldObjects.set(name, object);
  return object;
}

// ---- event and running scripts ----
function makeEvent(name, type, fd, extra) {
  const target = fd ? fieldObject(fd.name) : doc;
  const ev = {
    name, type, rc: true, value: fd ? (Array.isArray(fd.value) ? fd.value.slice() : fd.value) : "",
    change: "", changeEx: "", willCommit: false, selStart: 0, selEnd: 0, commitKey: 0,
    shift: false, modifier: false, keyDown: false, fieldFull: false,
    target, source: target, targetName: fd ? fd.name : "",
  };
  return Object.assign(ev, extra || {});
}
function runSource(source, ev, where) {
  const saved = G.event;
  G.event = ev;
  try {
    (0, eval)(String(source));
  } catch (error) {
    report(error, where);
  } finally {
    G.event = saved;
  }
}
// An action and the ones after it: JavaScript run, the form's own
// actions done (ResetForm, Hide, Named), the rest left to the viewer.
function runActions(actions, ev, where) {
  for (const action of actions || []) {
    if (action.js !== undefined)
      runSource(action.js, ev, where);
    else if (action.type === "ResetForm")
      resetForm(action.fields && action.fields.length ? action.fields : undefined, action.exclude);
    else if (action.type === "Hide") {
      for (const name of action.fields || []) {
        const f = fieldObject(name);
        if (f) f.display = action.hide === false ? 0 : 1;
      }
    } else if (action.type === "Named") {
      if (action.name === "Print") doc.print();
      else out.named = action.name;
    }
  }
}

function calculate(source) {
  if (calculating || !docState.calculate)
    return;
  calculating = true;
  try {
    // One pass in the calculation order (/CO): a calculation that changes
    // a field calculated earlier does not start another (what keeps two
    // fields calculated from each other from running forever).
    const names = model.calculationOrder.filter((n) => data.has(n));
    for (const name of order) {
      if (!names.includes(name) && (data.get(name).actions.C || []).length)
        names.push(name);
    }
    for (const name of names) {
      const fd = data.get(name);
      if (!(fd.actions.C || []).length)
        continue;
      const ev = makeEvent("Calculate", "Field", fd, { source: source ? fieldObject(source.name) : null });
      runActions(fd.actions.C, ev, name + " calculate");
      if (ev.rc)
        setValue(fd, ev.value);
    }
  } finally {
    calculating = false;
    pendingCalculation = false;
  }
}
function formatted(fd) {
  if (fd.type !== "text" && fd.type !== "combo")
    return null;
  const text = Array.isArray(fd.value) ? fd.value.join(",") : String(fd.value);
  if (!(fd.actions.F || []).length)
    return text;
  const ev = makeEvent("Format", "Field", fd, { value: fd.value });
  fd.formatting = true;
  try {
    runActions(fd.actions.F, ev, fd.name + " format");
  } finally {
    fd.formatting = false;
  }
  return ev.rc ? (ev.value === null || ev.value === undefined ? "" : String(ev.value)) : text;
}
// What the request changed: each field's new value and the text shown.
function finish() {
  if (pendingCalculation)
    calculate(null);
  for (const name of changed) {
    const fd = data.get(name);
    note(fd, "value", fd.value);
    const text = formatted(fd);
    if (text !== null)
      note(fd, "display", text);
  }
  const now = Date.now();
  let due = null;
  for (const t of timers)
    due = due === null ? t.due : Math.min(due, t.due);
  out.timer = due === null ? null : Math.max(0, due - now);
  return stringify(out);
}

function resetForm(names, exclude) {
  const all = order.filter((name) => {
    if (!names) return true;
    const listed = names.some((n) => name === n || name.startsWith(n + "."));
    return exclude ? !listed : listed;
  });
  for (const name of all) {
    const fd = data.get(name);
    setValue(fd, fd.defaultValue);
  }
}

// ---- Doc, app, console, global ----
const docState = { calculate: true, dirty: false, pageNum: 0 };
const doc = {
  get numFields() { return order.length; },
  getNthFieldName(n) { return order[n] === undefined ? null : order[n]; },
  getField(name) { return fieldObject(String(name)); },
  calculateNow() { calculate(null); },
  resetForm(names) { resetForm(names === undefined || names === null ? undefined : Array.isArray(names) ? names : [names], false); },
  print() { out.print = true; },
  get numPages() { return model.numPages; },
  get pageNum() { return docState.pageNum; },
  set pageNum(n) { n = Math.trunc(+n); if (n >= 0 && n < model.numPages) { docState.pageNum = n; out.page = n; } },
  get info() { return Object.assign({}, model.info); },
  get dirty() { return docState.dirty; },
  set dirty(v) { docState.dirty = !!v; },
  get calculate() { return docState.calculate; },
  set calculate(v) { docState.calculate = !!v; },
  get documentFileName() { return model.fileName; },
  get title() { return model.info.Title || ""; },
  get path() { return "/" + model.fileName; },
  get URL() { return model.url || ""; },
  get filesize() { return model.fileSize || 0; },
  get layout() { return "SinglePage"; },
  get zoom() { return 100; },
  set zoom(v) {},
  get zoomType() { return "NoVary"; },
  get external() { return true; },
  get hidden() { return false; },
  get author() { return model.info.Author || ""; },
  get subject() { return model.info.Subject || ""; },
  get keywords() { return model.info.Keywords || ""; },
  get creator() { return model.info.Creator || ""; },
  get producer() { return model.info.Producer || ""; },
  getPageNumWords() { return 0; },
  getPageNthWord() { return ""; },
  mailDoc() { log("mailDoc: not available in this viewer"); },
  mailForm() { log("mailForm: not available in this viewer"); },
  submitForm() { log("submitForm: not available in this viewer"); },
  getURL() { log("getURL: refused"); },
  exportAsFDF() {},
  exportAsXFDF() {},
  importAnFDF() {},
  syncAnnotScan() {},
  getAnnots() { return []; },
  getOCGs() { return []; },
  removeField() {},
  addField() { return null; },
  bringToFront() {},
  closeDoc() { return false; },
  saveAs() {},
  scroll() {},
};

const appObject = {
  get platform() { return "UNIX"; },
  get language() { return "ENU"; },
  get viewerType() { return "Reader"; },
  get viewerVariation() { return "Reader"; },
  get viewerVersion() { return 21; },
  get formsVersion() { return 21; },
  get fs() { return { isFullScreen: false }; },
  get fullscreen() { return false; },
  set fullscreen(v) {},
  get focusRect() { return true; },
  set focusRect(v) {},
  get runtimeHighlight() { return true; },
  set runtimeHighlight(v) {},
  get calculate() { return docState.calculate; },
  set calculate(v) { docState.calculate = !!v; },
  get activeDocs() { return [doc]; },
  get thermometer() { return { begin() {}, end() {}, value: 0, duration: 0, text: "" }; },
  // A dialog of the viewer's own that does not wait: the script goes on as
  // if the reader closed it without choosing (OK; Cancel, No where those
  // are offered).
  alert(message, icon, type) {
    let text = message, kind = type;
    if (message && typeof message === "object") {
      text = message.cMsg;
      kind = message.nType;
    }
    out.alerts.push(text === undefined ? "" : String(text));
    kind = +kind || 0;
    return kind === 0 ? 1 : kind === 1 ? 2 : kind === 2 ? 3 : 2;
  },
  beep() {},
  response() { return null; },
  // Pages are not opened by a document's script, as Chrome's viewer
  // refuses it.
  launchURL(url) { log("launchURL refused: " + String(url)); },
  goBack() {},
  goForward() {},
  execMenuItem(name) {
    if (name === "Print") doc.print();
    else if (["NextPage", "PrevPage", "FirstPage", "LastPage"].includes(name)) out.named = name;
  },
  mailMsg() {},
  newDoc() { return null; },
  openDoc() { return null; },
  popUpMenu() { return null; },
  popUpMenuEx() { return null; },
  setTimeOut(expression, ms) { return addTimer(expression, ms, false); },
  setInterval(expression, ms) { return addTimer(expression, ms, true); },
  clearTimeOut(timer) { removeTimer(timer); },
  clearInterval(timer) { removeTimer(timer); },
};
function addTimer(expression, ms, repeat) {
  const delay = Math.max(0, +ms || 0);
  const timer = { id: ++timerSerial, expression: String(expression), every: repeat ? Math.max(10, delay) : 0, due: Date.now() + delay };
  timers.push(timer);
  return { _timer: timer.id };
}
function removeTimer(t) {
  if (t && typeof t === "object")
    timers = timers.filter((x) => x.id !== t._timer);
}
const consoleObject = {
  println(text) { log(text); },
  show() {},
  hide() {},
  clear() {},
};
const globalObject = { setPersistent() {} };

function constant(name, value) {
  Object.defineProperty(G, name, { value, writable: true, configurable: true, enumerable: false });
}
constant("app", appObject);
constant("util", utilObject);
constant("color", colorObject);
constant("console", consoleObject);
constant("global", globalObject);
constant("display", Object.freeze({ visible: 0, hidden: 1, noPrint: 2, noView: 3 }));
constant("border", Object.freeze({ s: "solid", d: "dashed", b: "beveled", i: "inset", u: "underline" }));
constant("style", Object.freeze({ ch: "check", cr: "cross", di: "diamond", ci: "circle", st: "star", sq: "square" }));
constant("font", Object.freeze({ Times: "Times-Roman", TimesB: "Times-Bold", TimesI: "Times-Italic", TimesBI: "Times-BoldItalic",
  Helv: "Helvetica", HelvB: "Helvetica-Bold", HelvI: "Helvetica-Oblique", HelvBI: "Helvetica-BoldOblique",
  Cour: "Courier", CourB: "Courier-Bold", CourI: "Courier-Oblique", CourBI: "Courier-BoldOblique", Symbol: "Symbol", ZapfD: "ZapfDingbats" }));
constant("highlight", Object.freeze({ n: "none", i: "invert", p: "push", o: "outline" }));
constant("position", Object.freeze({ textOnly: 0, iconOnly: 1, iconTextV: 2, textIconV: 3, iconTextH: 4, textIconH: 5, overlay: 6 }));
constant("scaleHow", Object.freeze({ proportional: 0, anamorphic: 1 }));
constant("scaleWhen", Object.freeze({ always: 0, never: 1, tooBig: 2, tooSmall: 3 }));
constant("zoomtype", Object.freeze({ none: "NoVary", fitP: "FitPage", fitW: "FitWidth", fitH: "FitHeight", fitV: "FitVisibleWidth", pref: "Preferred", refW: "ReflowWidth" }));
constant("event", undefined);
// The global object answers as the Doc does: a document's scripts run
// with `this` the Doc, and calling its methods by `this.` works the same.
for (const key of Object.getOwnPropertyNames(doc)) {
  const descriptor = Object.getOwnPropertyDescriptor(doc, key);
  if (typeof descriptor.value === "function")
    constant(key, descriptor.value.bind(doc));
  else
    Object.defineProperty(G, key, { get: descriptor.get, set: descriptor.set, configurable: true, enumerable: false });
}

// ---- the AF functions (Acrobat forms documentation) ----
function AFMakeNumber(value) {
  if (typeof value === "number")
    return value;
  if (typeof value !== "string")
    return null;
  // A comma is read as the decimal point, as the European styles write it.
  const text = value.trim().replace(",", ".");
  if (!text || !/^[+-]?(\d+\.?\d*|\.\d+)$/.test(text))
    return null;
  return Number(text);
}
function AFMergeChange(ev) {
  ev = ev || G.event;
  if (ev.willCommit)
    return String(ev.value);
  const value = String(ev.value);
  return value.slice(0, ev.selStart >= 0 ? ev.selStart : 0) + ev.change + value.slice(ev.selEnd >= 0 ? ev.selEnd : value.length);
}
function AFExtractNums(text) {
  if (typeof text === "number")
    return [String(text)];
  const runs = String(text).match(/\d+/g);
  return runs ? runs : null;
}
function formatAlert(ev) {
  const name = ev.target && ev.target.name;
  appObject.alert("The value entered does not match the format of the field [ " + name + " ]");
}
// Digits with the style's separators, a currency where asked.
function AFNumber_Format(nDec, sepStyle, negStyle, currStyle, strCurrency, bCurrencyPrepend) {
  const ev = G.event;
  let value = AFMakeNumber(ev.value);
  if (value === null) {
    ev.value = "";
    return;
  }
  const negative = value < 0;
  let text = groupDigits(Math.abs(value).toFixed(Math.max(0, nDec | 0)), sepStyle | 0);
  if (+text.replace(/[^0-9]/g, "") === 0) text = text.replace(/^-/, "");
  const currency = strCurrency ? String(strCurrency) : "";
  if (currency)
    text = bCurrencyPrepend ? currency + text : text + currency;
  const isZero = +text.replace(/[^0-9]/g, "") === 0;
  if (negative && !isZero) {
    if (negStyle === 2 || negStyle === 3)
      text = "(" + text + ")";
    else if (negStyle !== 1)
      text = "-" + text;
  }
  if (negStyle === 1 || negStyle === 3)
    ev.target.textColor = negative && !isZero ? colorObject.red : colorObject.black;
  ev.value = text;
}
function AFNumber_Keystroke(nDec, sepStyle) {
  const ev = G.event;
  let value = AFMergeChange(ev);
  if (ev.willCommit) {
    if (value.trim() && AFMakeNumber(value) === null) {
      formatAlert(ev);
      ev.rc = false;
    }
    return;
  }
  const point = sepStyle === 2 || sepStyle === 3 ? "," : ".";
  const pattern = point === "," ? /^[+-]?\d*,?\d*$/ : /^[+-]?\d*\.?\d*$/;
  if (!pattern.test(value))
    ev.rc = false;
}
function AFPercent_Format(nDec, sepStyle, bPercentPrepend) {
  const ev = G.event;
  const value = AFMakeNumber(ev.value);
  if (value === null) {
    ev.value = "";
    return;
  }
  let text = groupDigits(Math.abs(value * 100).toFixed(Math.max(0, nDec | 0)), sepStyle | 0);
  if (value < 0 && +text.replace(/[^0-9]/g, "") !== 0) text = "-" + text;
  ev.value = bPercentPrepend ? "%" + text : text + "%";
}
function AFPercent_Keystroke(nDec, sepStyle) {
  AFNumber_Keystroke(nDec, sepStyle, 0, 0, "", true);
}
const DATE_FORMATS = ["m/d", "m/d/yy", "mm/dd/yy", "mm/yy", "d-mmm", "d-mmm-yy", "dd-mmm-yy", "yy-mm-dd", "mmm-yy", "mmmm-yy",
  "mmm d, yyyy", "mmmm d, yyyy", "m/d/yy h:MM tt", "m/d/yy HH:MM"];
const TIME_FORMATS = ["HH:MM", "h:MM tt", "HH:MM:ss", "h:MM:ss tt"];
// A date read from text the way the forms read one: by the format first,
// then leniently — the numbers in the order the format puts day, month
// and year, a month written as a word, a time after the date.
function AFParseDateEx(text, format) {
  const strict = scand(format, text);
  if (strict)
    return strict;
  text = String(text);
  format = String(format);
  const numbers = (text.match(/\d+/g) || []).map(Number);
  const words = text.match(/[A-Za-z]+/g) || [];
  let month = null;
  for (const w of words) {
    if (w.length < 3) continue;
    const index = MONTHS.findIndex((m) => m.toLowerCase().startsWith(w.toLowerCase()));
    if (index >= 0) { month = index; break; }
  }
  const positions = [];
  for (const letter of ["d", "m", "y"]) {
    const at = format.indexOf(letter);
    if (at >= 0) positions.push([at, letter]);
  }
  positions.sort((a, b) => a[0] - b[0]);
  const hasTime = /[Hh]/.test(format);
  const now = new Date();
  let day = 1, year = now.getFullYear(), at = 0;
  if (!positions.length && !hasTime)
    return null;
  for (const [, letter] of positions) {
    if (letter === "m" && month !== null) continue;
    if (at >= numbers.length) {
      if (letter === "y") break;
      return null;
    }
    const v = numbers[at++];
    if (letter === "d") day = v;
    else if (letter === "m") month = v - 1;
    else year = v < 100 ? (v < 50 ? 2000 + v : 1900 + v) : v;
  }
  if (month === null) month = positions.length ? 0 : now.getMonth();
  if (!positions.length) day = now.getDate();
  let hour = 0, minute = 0, second = 0;
  if (hasTime) {
    hour = numbers[at++] || 0;
    minute = numbers[at++] || 0;
    second = numbers[at++] || 0;
    if (/pm|\bp\b/i.test(text) && hour < 12) hour += 12;
    if (/am|\ba\b/i.test(text) && hour === 12) hour = 0;
  }
  if (month < 0 || month > 11 || day < 1 || hour > 23 || minute > 59 || second > 59)
    return null;
  const date = new Date(year, month, day, hour, minute, second);
  if (date.getMonth() !== month || date.getDate() !== day)
    return null;
  return date;
}
function AFDate_FormatEx(format) {
  const ev = G.event;
  if (!ev.value) return;
  const date = AFParseDateEx(ev.value, format);
  ev.value = date ? printd(format, date) : "";
}
function AFDate_Format(index) {
  AFDate_FormatEx(DATE_FORMATS[index] || DATE_FORMATS[0]);
}
function AFDate_KeystrokeEx(format) {
  const ev = G.event;
  if (!ev.willCommit || !ev.value)
    return;
  if (!AFParseDateEx(AFMergeChange(ev), format)) {
    appObject.alert("Invalid date/time: please ensure that the date/time exists. Field [ " + (ev.target && ev.target.name) + " ] should match format " + format);
    ev.rc = false;
  }
}
function AFDate_Keystroke(index) {
  AFDate_KeystrokeEx(DATE_FORMATS[index] || DATE_FORMATS[0]);
}
function AFTime_FormatEx(format) { AFDate_FormatEx(format); }
function AFTime_Format(index) { AFDate_FormatEx(TIME_FORMATS[index] || TIME_FORMATS[0]); }
function AFTime_KeystrokeEx(format) { AFDate_KeystrokeEx(format); }
function AFTime_Keystroke(index) { AFDate_KeystrokeEx(TIME_FORMATS[index] || TIME_FORMATS[0]); }

// The masks of the special formats: zip, zip+4, phone, social security.
function specialMask(psf, value) {
  switch (psf | 0) {
  case 0: return "99999";
  case 1: return "99999-9999";
  case 2: return String(value || "").replace(/\D/g, "").length > 7 ? "(999) 999-9999" : "999-9999";
  case 3: return "999-99-9999";
  default: return "";
  }
}
function AFSpecial_Format(psf) {
  const ev = G.event;
  if (!ev.value) return;
  const mask = specialMask(psf, ev.value);
  ev.value = mask ? printx(mask, String(ev.value)) : String(ev.value);
}
function maskAccepts(mask, text, whole) {
  let i = 0;
  for (const c of text) {
    if (i >= mask.length) return false;
    let m = mask[i];
    if (m === "*") return true;
    if (m === "\\") { m = mask[++i]; if (c !== m) return false; i++; continue; }
    const ok = m === "9" ? /\d/.test(c) : m === "A" ? /[A-Za-z]/.test(c) : m === "O" ? /[A-Za-z0-9]/.test(c) : m === "X" ? true : c === m;
    if (!ok) return false;
    i++;
  }
  return !whole || i >= mask.length || mask[i] === "*";
}
function AFSpecial_KeystrokeEx(mask) {
  const ev = G.event;
  mask = String(mask);
  if (!mask) return;
  const value = AFMergeChange(ev);
  if (ev.willCommit) {
    if (value && !maskAccepts(mask, value, true)) {
      formatAlert(ev);
      ev.rc = false;
    }
    return;
  }
  if (maskAccepts(mask, value, false))
    return;
  // The literal characters the mask has next put in for the reader.
  const before = value.slice(0, ev.selStart) ;
  let inserted = "";
  let at = before.length;
  while (at < mask.length && !"9AOX*\\".includes(mask[at])) {
    inserted += mask[at];
    at++;
  }
  const withLiterals = before + inserted + ev.change + value.slice(ev.selStart + ev.change.length);
  if (inserted && maskAccepts(mask, withLiterals, false)) {
    ev.change = inserted + ev.change;
    return;
  }
  ev.rc = false;
}
// The special formats take digits (with the separators their format
// writes, typed or not): a zip code five, zip+4 nine, a phone number
// seven or ten, a social security number nine.
function AFSpecial_Keystroke(psf) {
  const ev = G.event;
  const counts = [[5], [9], [7, 10], [9]][psf | 0];
  if (!counts)
    return;
  const value = AFMergeChange(ev);
  const digits = value.replace(/\D/g, "").length;
  if (!/^[0-9()\- .]*$/.test(value) || (!ev.willCommit && digits > Math.max.apply(null, counts))) {
    if (ev.willCommit) formatAlert(ev);
    ev.rc = false;
    return;
  }
  if (ev.willCommit && value && !counts.includes(digits)) {
    formatAlert(ev);
    ev.rc = false;
  }
}
function AFSimple(cFunction, nValue1, nValue2) {
  const a = AFMakeNumber(nValue1), b = AFMakeNumber(nValue2);
  switch (String(cFunction).toUpperCase()) {
  case "AVG": return (a + b) / 2;
  case "SUM": return a + b;
  case "PRD": return a * b;
  case "MIN": return Math.min(a, b);
  case "MAX": return Math.max(a, b);
  }
  throw new TypeError("AFSimple: no function " + cFunction);
}
function AFSimple_Calculate(cFunction, cFields) {
  const kind = String(cFunction).toUpperCase();
  let names = typeof cFields === "string" ? cFields.split(",") : Array.from(cFields || []);
  names = names.map((n) => String(n).trim()).filter(Boolean);
  const values = [];
  for (const name of names) {
    const f = doc.getField(name);
    if (!f) continue;
    const parts = f._fd.group ? f.getArray() : [f];
    for (const part of parts) {
      const v = AFMakeNumber(part.valueAsString);
      values.push(v === null ? 0 : v);
    }
  }
  let result = 0;
  if (values.length) {
    switch (kind) {
    case "AVG": result = values.reduce((s, v) => s + v, 0) / values.length; break;
    case "SUM": result = values.reduce((s, v) => s + v, 0); break;
    case "PRD": result = values.reduce((s, v) => s * v, 1); break;
    case "MIN": result = Math.min.apply(null, values); break;
    case "MAX": result = Math.max.apply(null, values); break;
    default: throw new TypeError("AFSimple_Calculate: no function " + cFunction);
    }
  }
  // Rounded to the precision a number shows (Acrobat rounds to 15 digits).
  G.event.value = Number(result.toPrecision(15));
}
function AFRange_Validate(bGreaterThan, nGreaterThan, bLessThan, nLessThan) {
  const ev = G.event;
  if (ev.value === "" || ev.value === null || ev.value === undefined)
    return;
  const value = AFMakeNumber(ev.value);
  if (value === null)
    return;
  let message = "";
  if (bGreaterThan && bLessThan) {
    if (value < nGreaterThan || value > nLessThan)
      message = "must be greater than or equal to " + nGreaterThan + " and less than or equal to " + nLessThan;
  } else if (bGreaterThan) {
    if (value < nGreaterThan)
      message = "must be greater than or equal to " + nGreaterThan;
  } else if (bLessThan) {
    if (value > nLessThan)
      message = "must be less than or equal to " + nLessThan;
  }
  if (message) {
    appObject.alert("Invalid value: " + message + ".");
    ev.rc = false;
  }
}
const AF = { AFMakeNumber, AFMergeChange, AFExtractNums, AFNumber_Format, AFNumber_Keystroke, AFPercent_Format, AFPercent_Keystroke,
  AFDate_Format, AFDate_FormatEx, AFDate_Keystroke, AFDate_KeystrokeEx, AFTime_Format, AFTime_FormatEx, AFTime_Keystroke, AFTime_KeystrokeEx,
  AFParseDateEx, AFSpecial_Format, AFSpecial_Keystroke, AFSpecial_KeystrokeEx, AFSimple, AFSimple_Calculate, AFRange_Validate };
for (const name of Object.keys(AF))
  constant(name, AF[name]);

// ---- the host's entry points ----
function load(json) {
  model = parse(json);
  model.calculationOrder = model.calculationOrder || [];
  model.info = model.info || {};
  for (const fd of model.fields) {
    if (data.has(fd.name)) continue;
    fd.actions = fd.actions || {};
    fd.widgets = fd.widgets || [];
    for (const w of fd.widgets) w.actions = w.actions || {};
    data.set(fd.name, fd);
    order.push(fd.name);
  }
}
function fieldFor(request) {
  const fd = data.get(request.field);
  if (!fd) throw new TypeError("no field " + request.field);
  return fd;
}
function widgetActions(fd, request, key) {
  const w = fd.widgets[request.widget | 0];
  return (w && w.actions[key]) || [];
}
function dispatch(json) {
  begin();
  const request = parse(json);
  switch (request.type) {
  case "open": {
    for (const source of model.documentScripts || [])
      runSource(source, makeEvent("Init", "Doc", null), "document script");
    runActions(model.openAction, makeEvent("Open", "Doc", null), "open action");
    // Every field with a format action shown formatted from the start.
    for (const name of order) {
      const fd = data.get(name);
      if ((fd.actions.F || []).length) changed.add(name);
    }
    break;
  }
  case "docEvent": {
    const key = { WillClose: "WC", WillSave: "WS", DidSave: "DS", WillPrint: "WP", DidPrint: "DP" }[request.name];
    if (key) runActions((model.documentActions || {})[key], makeEvent(request.name, "Doc", null), request.name);
    break;
  }
  case "keystroke": {
    const fd = fieldFor(request);
    const ev = makeEvent("Keystroke", "Field", fd, {
      value: String(request.value), change: String(request.change || ""), changeEx: String(request.change || ""),
      selStart: request.selStart | 0, selEnd: request.selEnd | 0, willCommit: false,
    });
    runActions(fd.actions.K, ev, fd.name + " keystroke");
    out.rc = !!ev.rc;
    out.change = String(ev.change);
    out.selStart = ev.selStart;
    out.selEnd = ev.selEnd;
    break;
  }
  case "commit": {
    const fd = fieldFor(request);
    let value = request.value;
    const old = fd.value;
    if ((fd.actions.K || []).length) {
      const ev = makeEvent("Keystroke", "Field", fd, { value, willCommit: true, commitKey: request.commitKey | 0 });
      runActions(fd.actions.K, ev, fd.name + " keystroke");
      if (!ev.rc) { out.rc = false; break; }
      value = ev.value;
    }
    if ((fd.actions.V || []).length) {
      const ev = makeEvent("Validate", "Field", fd, { value });
      runActions(fd.actions.V, ev, fd.name + " validate");
      if (!ev.rc) { out.rc = false; break; }
      value = ev.value;
    }
    out.rc = true;
    setValue(fd, value);
    if (stringify(old) !== stringify(fd.value)) {
      calculate(fd);
      docState.dirty = true;
    }
    changed.add(fd.name);
    break;
  }
  case "focus":
  case "blur": {
    const fd = fieldFor(request);
    const key = request.type === "focus" ? "Fo" : "Bl";
    runActions(widgetActions(fd, request, key), makeEvent(request.type === "focus" ? "Focus" : "Blur", "Field", fd), fd.name + " " + request.type);
    break;
  }
  case "mouse": {
    const fd = fieldFor(request);
    const key = request.action;
    if (request.value !== undefined && key === "U") {
      const old = fd.value;
      setValue(fd, request.value);
      if (stringify(old) !== stringify(fd.value)) calculate(fd);
    }
    const names = { D: "Mouse Down", U: "Mouse Up", E: "Mouse Enter", X: "Mouse Exit" };
    const ev = makeEvent(names[key] || key, "Field", fd);
    runActions(widgetActions(fd, request, key), ev, fd.name + " " + (names[key] || key));
    if (key === "U")
      runActions(widgetActions(fd, request, "A"), ev, fd.name + " action");
    break;
  }
  case "timer": {
    const now = Date.now();
    const due = timers.filter((t) => t.due <= now);
    for (const t of due) {
      if (t.every) t.due = now + t.every;
      else timers = timers.filter((x) => x !== t);
      runSource(t.expression, makeEvent("Timer", "App", null), "timer");
    }
    break;
  }
  case "eval": {
    let result;
    try {
      result = (0, eval)(String(request.source));
    } catch (error) {
      result = "Error: " + (error && error.name ? error.name + ": " + error.message : String(error));
    }
    out.result = result === undefined ? "undefined" : typeof result === "string" ? result : stringify(result);
    break;
  }
  default:
    throw new TypeError("no request " + request.type);
  }
  return finish();
}
Object.defineProperty(G, "__sashfold_form_load", { value: load });
Object.defineProperty(G, "__sashfold_form_dispatch", { value: dispatch });
})();
)JS";
    return { source, sizeof source - 1 };
}

}
