// Eleven kernels, each the kind of work a page's script is made of, timed
// inside the engine (tools/kernels.sh runs them). __scale, when a runner
// defines it, multiplies every count: a fast engine needs more iterations
// for a readable time, and the times are reported per scale 1. Each
// kernel's answer goes with its time, so two builds that disagree on what
// a kernel computes are caught as well as timed.
var __S = (typeof __scale !== "undefined") ? __scale : 1;
var gcount = 0, gstep = 3;
function time(f) { var t = Date.now(); var r = f(__S); return [Date.now() - t, r]; }
var kernels = {
  locals: function (SCALE) { var s = 0; for (var i = 0; i < 2000000 * SCALE; i++) { s = (s + i * 3) % 1000003; } return s; },
  props: function (SCALE) { var p = { x: 1, y: 2, z: 3 }; for (var i = 0; i < 1000000 * SCALE; i++) { p.x = p.x + p.y; p.z = p.x - p.z; } return p.z % 1000; },
  method: function (SCALE) {
    function P(x) { this.x = x; } P.prototype.add = function (n) { this.x = (this.x + n) % 65521; return this; };
    var p = new P(1); for (var i = 0; i < 600000 * SCALE; i++) p.add(i); return p.x; },
  calls: function (SCALE) { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } var r = 0; for (var k = 0; k < SCALE; k++) r = fib(25); return r; },
  closures: function (SCALE) { var c = 0; var inc = function (n) { c = (c + n) % 9973; }; for (var i = 0; i < 800000 * SCALE; i++) inc(i); return c; },
  arrays: function (SCALE) { var a = []; var n = 200000; for (var i = 0; i < n; i++) a.push(i); var s = 0; for (var k = 0; k < 4 * SCALE; k++) for (var j = 0; j < n; j++) { a[j] = a[j] + 1; s = (s + a[j]) % 1000003; } return s; },
  strings: function (SCALE) { var s = 0; for (var i = 0; i < 60000 * SCALE; i++) { var t = "item-" + i + ":" + (i % 7); s += t.length + t.charCodeAt(3) + t.indexOf(":"); if (i % 64 === 0) s += t.split("-").join("+").length; } return s; },
  poly: function (SCALE) { var os = [{ a: 1 }, { b: 2, a: 2 }, { c: 3, b: 1, a: 3 }, { d: 4, c: 1, b: 1, a: 4 }]; var s = 0; for (var i = 0; i < 1000000 * SCALE; i++) s = (s + os[i & 3].a) % 1000003; return s; },
  alloc: function (SCALE) { var keep = []; var s = 0; for (var i = 0; i < 300000 * SCALE; i++) { var o = { id: i, tags: [i, i + 1], name: "n" + (i & 255) }; if ((i & 1023) === 0) keep.push(o); s += o.tags.length; } return s + keep.length; },
  builtins: function (SCALE) { var m = new Map(); var s = 0; for (var i = 0; i < 300000 * SCALE; i++) { m.set(i & 4095, i); s += Math.floor(i / 3) + (m.get((i * 7) & 4095) || 0) % 3; s = s % 1000003; } return s + Object.keys({ a: 1, b: 2 }).length; },
  globals: function (SCALE) { for (var i = 0; i < 500000 * SCALE; i++) { gcount = (gcount + gstep) % 1000003; } return gcount; }
};
var __names = Object.keys(kernels), __row = [];
for (var __i = 0; __i < __names.length; __i++) { var __t = time(kernels[__names[__i]]); __row.push(__names[__i] + "=" + (__t[0] / __S).toFixed(2) + "/" + __t[1]); }
var __out = __row.join(" ");
