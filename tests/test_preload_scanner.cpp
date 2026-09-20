#include "Test.h"

#include "html/PreloadScanner.h"

#include <string>
#include <vector>

// What a document will ask for, read off its bytes before the parse: only
// what it is sure of.

using namespace sashfold;
using html::Preload;
using html::PreloadScan;

namespace {

// "S:url" for a stylesheet, "J:url" for a script, in the order found.
std::string said(PreloadScan const& scan)
{
    std::string out;
    for (Preload const& resource : scan.resources) {
        if (!out.empty())
            out += " ";
        out += resource.kind == Preload::Kind::Stylesheet ? "S:" : "J:";
        out += resource.url;
    }
    return out;
}

}

int main()
{
    // The stylesheets and the scripts a page names, in its order, with the
    // attributes quoted either way or not at all and the tags in any case.
    {
        PreloadScan const scan = html::scan_for_preloads(R"html(<!doctype html><html><head>
<LINK REL="Stylesheet" HREF="/a.css">
<link href='b.css?x=1&amp;y=2' rel='preload stylesheet'>
<link rel=stylesheet href=c.css>
<script src="/one.js"></script>
<script type="module" src="two.mjs" async></script>
<SCRIPT SRC=three.js defer></SCRIPT>
</head><body><script src=" four.js "></script></body></html>)html");
        CHECK_EQ(said(scan), std::string("S:/a.css S:b.css?x=1&y=2 S:c.css J:/one.js J:two.mjs J:three.js J:four.js"));
        CHECK(scan.base_href.empty());
        CHECK(!scan.meta_policy);
    }
    // What the page will not ask for, or not as that: an alternate or a print
    // sheet, a disabled one, an icon, a script that is data, a nomodule
    // fallback, one with no address.
    {
        PreloadScan const scan = html::scan_for_preloads(R"html(
<link rel="alternate stylesheet" href="alt.css">
<link rel="stylesheet" media="print" href="print.css">
<link rel="stylesheet" media="screen and (min-width: 1px)" href="screen.css">
<link rel="stylesheet" href="off.css" disabled>
<link rel="icon" href="icon.png">
<link rel="stylesheet" href="">
<script type="application/json" src="data.json"></script>
<script type="text/template" src="tpl.html"></script>
<script nomodule src="legacy.js"></script>
<script src=""></script>
<script type="text/javascript" src="plain.js"></script>)html");
        CHECK_EQ(said(scan), std::string("S:screen.css J:plain.js"));
    }
    // What is not markup is passed over: comments, the text of a script, a
    // style, a textarea, a title, a template, and what a page shows only
    // when scripts are off.
    {
        PreloadScan const scan = html::scan_for_preloads(R"html(
<!-- <script src="commented.js"></script> -->
<script>document.write('<script src="written.js"><\/script>'); var s = "<link rel=stylesheet href=in-script.css>";</script>
<style>/* <link rel="stylesheet" href="in-style.css"> */</style>
<title><script src="in-title.js"></script></title>
<textarea><script src="in-textarea.js"></script></textarea>
<template><script src="in-template.js"></script></template>
<noscript><link rel="stylesheet" href="noscript.css"></noscript>
<script src="real.js"></script>)html");
        CHECK_EQ(said(scan), std::string("J:real.js"));
    }
    // The first <base> with an href is what the addresses are relative to,
    // and a policy stated in a <meta> is told.
    {
        PreloadScan const scan = html::scan_for_preloads(R"html(<base target="_blank"><base href=" https://cdn.example/assets/ "><base href="/other/">
<meta http-equiv="Content-Security-Policy" content="script-src 'self'">
<link rel="stylesheet" href="site.css">)html");
        CHECK_EQ(scan.base_href, std::string("https://cdn.example/assets/"));
        CHECK(scan.meta_policy);
        CHECK_EQ(said(scan), std::string("S:site.css"));
        CHECK(!html::scan_for_preloads("<meta http-equiv=refresh content=5><meta charset=utf-8>").meta_policy);
    }
    // Markup that ends in the middle of anything ends the scan, not the program.
    {
        for (char const* broken : { "<", "<link", "<link rel=", "<link rel=\"stylesheet", "<script src=x.js", "<script src=x.js>",
                 "<!-- never closed", "<style>never closed", "<link rel=stylesheet href='x.css", "</", "<>", "< link>" }) {
            PreloadScan const scan = html::scan_for_preloads(broken);
            CHECK(scan.resources.size() <= 1);
        }
        CHECK_EQ(said(html::scan_for_preloads("<script src=x.js>")), std::string("J:x.js"));
        CHECK(html::scan_for_preloads("").resources.empty());
    }
    return test::report("preload scanner");
}
