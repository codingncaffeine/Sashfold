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

// "S:url" for a stylesheet, "J:" a script, "F:" a font, "I:" a picture,
// "P:" a connection to warm and "D:" a name to look up, in the order found.
std::string said(PreloadScan const& scan)
{
    std::string out;
    for (Preload const& resource : scan.resources) {
        if (!out.empty())
            out += " ";
        switch (resource.kind) {
        case Preload::Kind::Stylesheet: out += "S:"; break;
        case Preload::Kind::Script: out += "J:"; break;
        case Preload::Kind::Font: out += "F:"; break;
        case Preload::Kind::Image: out += "I:"; break;
        case Preload::Kind::Preconnect: out += "P:"; break;
        case Preload::Kind::DnsPrefetch: out += "D:"; break;
        }
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
<link rel="stylesheet" href="site.css">
<script src="app.js" nonce="r4nd0m"></script><script src="plain.js"></script>)html");
        CHECK_EQ(scan.base_href, std::string("https://cdn.example/assets/"));
        CHECK(scan.meta_policy);
        CHECK_EQ(scan.meta_policies.size(), std::size_t { 1 });
        CHECK(!scan.meta_policies.empty() && scan.meta_policies[0] == "script-src 'self'");
        CHECK_EQ(said(scan), std::string("S:site.css J:app.js J:plain.js"));
        CHECK(scan.resources.size() == 3 && scan.resources[1].nonce == "r4nd0m" && scan.resources[2].nonce.empty()
            && scan.resources[0].nonce.empty());
        CHECK(!html::scan_for_preloads("<meta http-equiv=refresh content=5><meta charset=utf-8>").meta_policy);
        CHECK(html::scan_for_preloads("<meta http-equiv=refresh content=5><meta charset=utf-8>").meta_policies.empty());
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
    // The hints a page gives about what it will want: connections to warm
    // (preconnect, dns-prefetch) and resources to fetch now (preload by its
    // as=, modulepreload as a script). A preload of a kind not fetched ahead,
    // or with no as=, or for print, is passed over.
    {
        PreloadScan const scan = html::scan_for_preloads(R"html(<head>
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel=dns-prefetch href="//cdn.example">
<link rel="preload" href="/f.woff2" as="font" type="font/woff2" crossorigin>
<link rel="preload" as="image" href="hero.jpg">
<link rel="preload" as="script" href="app.js">
<link rel="PRELOAD" AS="Style" href="late.css">
<link rel="modulepreload" href="mod.js">
<link rel="preload" as="fetch" href="/api/data">
<link rel="preload" href="no-as.js">
<link rel="preload" as="style" media="print" href="print.css">
<link rel="preconnect dns-prefetch" href="https://both.example">
</head>)html");
        CHECK_EQ(said(scan), std::string("P:https://fonts.gstatic.com D://cdn.example F:/f.woff2 I:hero.jpg J:app.js S:late.css "
                                         "J:mod.js P:https://both.example"));
        // What a hint fetches is marked so, and nothing waits for it.
        bool marked = scan.resources.size() == 8;
        for (std::size_t i = 0; i < scan.resources.size(); ++i) {
            bool const fetched_by_hint = i >= 2 && i <= 6;
            marked = marked && scan.resources[i].hint == fetched_by_hint;
        }
        CHECK(marked);
        CHECK(!html::scan_for_preloads("<script src=a.js></script><link rel=stylesheet href=b.css>").resources[0].hint);
    }
    return test::report("preload scanner");
}
