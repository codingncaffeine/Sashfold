#!/usr/bin/env python3
"""An independent AT-SPI2 reader (the system's libatspi, through gi) for the
browser's accessibility bridge: finds the application named on the
accessibility bus and prints its tree, one line per object: role, name,
states, attributes, text, bounds (window coordinates), relations, actions,
value; with --deep every other interface entry by entry; with --do it acts
through the protocol first; with --events it records the events it hears.
Run only through tools/atspi-session.sh, on a bus of its own."""
import argparse
import json
import os
import sys
import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib  # noqa: E402


def find_app(name, wait_s):
    deadline = time.time() + wait_s
    while time.time() < deadline:
        desktop = Atspi.get_desktop(0)
        for i in range(desktop.get_child_count()):
            child = desktop.get_child_at_index(i)
            if child is not None and child.get_name() == name:
                return child
        time.sleep(0.2)
    return None


def states_of(acc):
    out = []
    for s in acc.get_state_set().get_states():
        out.append(Atspi.StateType(s).value_nick if hasattr(Atspi.StateType(s), "value_nick") else str(s))
    return sorted(out)


def describe(acc, with_text=True):
    d = {}
    d["role"] = acc.get_role_name()
    d["name"] = acc.get_name()
    desc = acc.get_description()
    if desc:
        d["description"] = desc
    d["states"] = states_of(acc)
    ifaces = acc.get_interfaces()
    d["interfaces"] = sorted(ifaces)
    try:
        attrs = acc.get_attributes()
        if attrs:
            d["attributes"] = dict(sorted(attrs.items()))
    except GLib.Error as e:
        d["attributes_error"] = str(e)
    if "Component" in ifaces:
        try:
            r = Atspi.Component.get_extents(acc, Atspi.CoordType.WINDOW)
            d["bounds"] = [r.x, r.y, r.width, r.height]
        except GLib.Error as e:
            d["bounds_error"] = str(e)
    if with_text and "Text" in ifaces:
        try:
            n = Atspi.Text.get_character_count(acc)
            t = Atspi.Text.get_text(acc, 0, -1)
            d["text"] = t.replace("￼", "<obj>")
            d["chars"] = n
            caret = Atspi.Text.get_caret_offset(acc)
            if caret >= 0:
                d["caret"] = caret
        except GLib.Error as e:
            d["text_error"] = str(e)
    if "Value" in ifaces:
        try:
            d["value"] = [Atspi.Value.get_current_value(acc), Atspi.Value.get_minimum_value(acc), Atspi.Value.get_maximum_value(acc)]
        except GLib.Error as e:
            d["value_error"] = str(e)
    if "Action" in ifaces:
        try:
            d["actions"] = [Atspi.Action.get_action_name(acc, i) for i in range(Atspi.Action.get_n_actions(acc))]
        except GLib.Error as e:
            d["actions_error"] = str(e)
    if DEEP:
        deep(acc, ifaces, d)
    rels = []
    try:
        for rel in acc.get_relation_set():
            targets = [rel.get_target(i).get_name() for i in range(rel.get_n_targets())]
            rels.append("%s->%s" % (Atspi.RelationType(rel.get_relation_type()).value_nick, targets))
    except GLib.Error as e:
        rels.append("error: %s" % e)
    if rels:
        d["relations"] = rels
    return d


DEEP = False


def short(acc):
    if acc is None:
        return None
    if not isinstance(acc, Atspi.Accessible):
        return "<%s %s>" % (type(acc).__name__, getattr(acc, "path", "?"))
    return "%s %r" % (acc.get_role_name(), acc.get_name())


def attempt(d, key, fn):
    try:
        d[key] = fn()
    except GLib.Error as e:
        d[key + "_error"] = str(e)
    except Exception as e:  # noqa: BLE001 - a reader reports, never stops
        d[key + "_error"] = "%s: %s" % (type(e).__name__, e)


def deep(acc, ifaces, d):
    """Every other interface the object offers, entry by entry."""
    W = Atspi.CoordType.WINDOW
    if "Component" in ifaces:
        def at_center():
            r = Atspi.Component.get_extents(acc, W)
            if r.width <= 0 or r.height <= 0:
                return None
            hit = Atspi.Component.get_accessible_at_point(acc, r.x + r.width // 2, r.y + r.height // 2, W)
            return short(hit)
        attempt(d, "hit_center", at_center)
        attempt(d, "layer", lambda: Atspi.Component.get_layer(acc).value_nick)
        def positions():
            s = Atspi.Component.get_position(acc, Atspi.CoordType.SCREEN)
            p = Atspi.Component.get_position(acc, Atspi.CoordType.PARENT)
            return {"screen": [s.x, s.y], "parent": [p.x, p.y]}
        attempt(d, "positions", positions)
    if "Text" in ifaces:
        def words():
            out = []
            n = Atspi.Text.get_character_count(acc)
            off = 0
            while off < n and len(out) < 12:
                r = Atspi.Text.get_string_at_offset(acc, off, Atspi.TextGranularity.WORD)
                out.append([r.start_offset, r.end_offset, r.content])
                if r.end_offset <= off:
                    break
                off = r.end_offset
            return out
        attempt(d, "words", words)

        def lines():
            out = []
            n = Atspi.Text.get_character_count(acc)
            off = 0
            while off < n and len(out) < 8:
                r = Atspi.Text.get_string_at_offset(acc, off, Atspi.TextGranularity.LINE)
                out.append([r.start_offset, r.end_offset, r.content.replace("￼", "<obj>")])
                if r.end_offset <= off:
                    break
                off = r.end_offset
            return out
        attempt(d, "lines", lines)

        def run0():
            res = Atspi.Text.get_attribute_run(acc, 0, False)
            if isinstance(res, tuple):
                attrs, start, end = res
                return [start, end, dict(sorted(attrs.items()))]
            return str(res)
        attempt(d, "attr_run0", run0)
        attempt(d, "default_attrs", lambda: dict(sorted(Atspi.Text.get_default_attributes(acc).items())))

        def ext0():
            n = Atspi.Text.get_character_count(acc)
            if n == 0:
                return None
            r = Atspi.Text.get_character_extents(acc, 0, W)
            back = Atspi.Text.get_offset_at_point(acc, r.x + max(r.width, 1) // 2, r.y + max(r.height, 1) // 2, W)
            return [[r.x, r.y, r.width, r.height], back]
        attempt(d, "char0", ext0)
        attempt(d, "nselections", lambda: Atspi.Text.get_n_selections(acc))
    if "Hypertext" in ifaces:
        def links():
            out = []
            for i in range(min(Atspi.Hypertext.get_n_links(acc), 10)):
                link = Atspi.Hypertext.get_link(acc, i)
                if isinstance(link, Atspi.Accessible):  # the client hands back the object itself
                    link = link.get_hyperlink()
                obj = Atspi.Hyperlink.get_object(link, 0)
                out.append([Atspi.Hyperlink.get_start_index(link), Atspi.Hyperlink.get_end_index(link), short(obj), Atspi.Hyperlink.get_uri(link, 0)])
            return out
        attempt(d, "links", links)
    if "Table" in ifaces:
        def table():
            rows, cols = Atspi.Table.get_n_rows(acc), Atspi.Table.get_n_columns(acc)
            grid = []
            for r in range(min(rows, 6)):
                grid.append([(Atspi.Table.get_accessible_at(acc, r, c).get_name() if Atspi.Table.get_accessible_at(acc, r, c) else None) for c in range(min(cols, 6))])
            caption = Atspi.Table.get_caption(acc)
            headers = [short(Atspi.Table.get_column_header(acc, c)) for c in range(min(cols, 6))]
            return {"rows": rows, "cols": cols, "grid": grid, "caption": short(caption), "column_headers": headers,
                    "extent_1_1": [Atspi.Table.get_row_extent_at(acc, min(1, rows - 1), 1), Atspi.Table.get_column_extent_at(acc, min(2, rows - 1), 1)] if rows and cols > 1 else None}
        attempt(d, "table", table)
    if "TableCell" in ifaces:
        def cell():
            pos = Atspi.TableCell.get_position(acc)
            return {"position": [pos[1], pos[2]] if isinstance(pos, tuple) else str(pos),
                    "spans": [Atspi.TableCell.get_row_span(acc), Atspi.TableCell.get_column_span(acc)],
                    "column_headers": [short(h) for h in Atspi.TableCell.get_column_header_cells(acc)],
                    "row_headers": [short(h) for h in Atspi.TableCell.get_row_header_cells(acc)],
                    "table": short(Atspi.TableCell.get_table(acc))}
        attempt(d, "cell", cell)
    if "Selection" in ifaces:
        def sel():
            n = Atspi.Selection.get_n_selected_children(acc)
            return [short(Atspi.Selection.get_selected_child(acc, i)) for i in range(n)]
        attempt(d, "selected", sel)
    if "Document" in ifaces:
        attempt(d, "doc", lambda: {"locale": Atspi.Document.get_locale(acc), "attrs": dict(sorted(Atspi.Document.get_document_attributes(acc).items())),
                                   "pages": [Atspi.Document.get_current_page_number(acc), Atspi.Document.get_page_count(acc)]})

        def headings():
            rule = Atspi.MatchRule.new(Atspi.StateSet.new([]), Atspi.CollectionMatchType.ALL, None, Atspi.CollectionMatchType.ALL,
                                       [Atspi.Role.HEADING], Atspi.CollectionMatchType.ANY, [], Atspi.CollectionMatchType.ALL, False)
            found = Atspi.Collection.get_matches(acc, rule, Atspi.CollectionSortOrder.CANONICAL, 0, True)
            return [short(f) for f in found]
        attempt(d, "collection_headings", headings)
    if "Image" in ifaces:
        attempt(d, "image", lambda: [Atspi.Image.get_image_description(acc), Atspi.Image.get_image_locale(acc)])
    if "Hyperlink" in ifaces and d.get("role") == "link":
        attempt(d, "hyperlink", lambda: [Atspi.Hyperlink.get_n_anchors(acc.get_hyperlink()), Atspi.Hyperlink.get_uri(acc.get_hyperlink(), 0)])


def walk(acc, depth, out, max_depth, seen):
    d = describe(acc)
    d["depth"] = depth
    try:
        d["index"] = acc.get_index_in_parent()
    except GLib.Error:
        d["index"] = None
    out.append(d)
    if depth >= max_depth:
        return
    n = acc.get_child_count()
    for i in range(n):
        child = acc.get_child_at_index(i)
        if child is None:
            out.append({"depth": depth + 1, "role": "<null child %d>" % i})
            continue
        # The child's parent must be this object (a check of the tree's shape).
        parent = child.get_parent()
        if parent is None or parent.get_name() != acc.get_name() or parent.get_role_name() != acc.get_role_name():
            out.append({"depth": depth + 1, "role": "<parent mismatch at child %d>" % i})
        key = (child.get_bus_name() if hasattr(child, "get_bus_name") else "", id(child))
        walk(child, depth + 1, out, max_depth, seen)


def line_of(d):
    parts = ["  " * d.get("depth", 0) + d.get("role", "?")]
    if d.get("name"):
        parts.append(json.dumps(d["name"]))
    for k in ("description", "states", "attributes", "text", "caret", "bounds", "value", "actions", "relations", "index",
              "hit_center", "layer", "positions", "words", "lines", "attr_run0", "default_attrs", "char0", "nselections", "links",
              "table", "cell", "selected", "doc", "collection_headings", "image", "hyperlink"):
        if k in d and d[k] not in (None, [], {}, ""):
            parts.append("%s=%s" % (k, json.dumps(d[k], ensure_ascii=False)))
    for k in d:
        if k.endswith("_error"):
            parts.append("%s=%s" % (k, d[k]))
    return " ".join(parts)


def record_events(types, go_file, done_after_s, quiet_s):
    """Listens for the event types, says it listens (go_file), and records
    every event until nothing has come for quiet_s (or done_after_s)."""
    events = []
    started = time.time()
    last = [time.time()]

    def on_event(e):
        src = e.source
        try:
            if os.environ.get("READER_BARE"):
                role, name = "-", src.path if hasattr(src, "path") else "-"
            else:
                role = src.get_role_name() if src else None
                name = src.get_name() if src else None
        except GLib.Error:
            role, name = "?", "?"
        any_data = e.any_data
        if isinstance(any_data, Atspi.Accessible):
            try:
                any_data = "%s %r" % (any_data.get_role_name(), any_data.get_name())
            except GLib.Error:
                any_data = "<gone>"
        elif any_data is not None and not isinstance(any_data, (str, int, float, bool)):
            any_data = str(any_data)
        events.append({"t": round(time.time() - started, 3), "type": e.type, "source": "%s %r" % (role, name),
                       "detail1": e.detail1, "detail2": e.detail2, "any": any_data})
        last[0] = time.time()

    listener = Atspi.EventListener.new(on_event)
    for t in types:
        listener.register(t)
    # Let the registration reach the application through the registry.
    def tick():
        if go_file and not os.path.exists(go_file):
            open(go_file, "w").close()
        now = time.time()
        if now - started > done_after_s or (now - last[0] > quiet_s and now - started > quiet_s):
            Atspi.event_quit()
            return False
        return True
    GLib.timeout_add(100, tick)
    Atspi.event_main()
    for t in types:
        listener.deregister(t)
    return events


def find_by(app, role, name):
    stack = [app]
    while stack:
        acc = stack.pop()
        try:
            if acc.get_role_name() == role and acc.get_name() == name:
                return acc
            for i in range(acc.get_child_count() - 1, -1, -1):
                c = acc.get_child_at_index(i)
                if c is not None:
                    stack.append(c)
        except GLib.Error:
            pass
    return None


def perform(app, spec):
    """role|name|what: action:<name>, focus, set-text:<text>, set-value:<n>, select:<a>:<b>, caret:<n>"""
    role, name, what = spec.split("|", 2)
    acc = find_by(app, role, name)
    if acc is None:
        return "no %s %r" % (role, name)
    try:
        if what.startswith("action:"):
            wanted = what[7:]
            for i in range(Atspi.Action.get_n_actions(acc)):
                if Atspi.Action.get_action_name(acc, i) == wanted:
                    return "do_action(%s) -> %s" % (wanted, Atspi.Action.do_action(acc, i))
            return "no action %r" % wanted
        if what == "focus":
            return "grab_focus -> %s" % Atspi.Component.grab_focus(acc)
        if what.startswith("set-text:"):
            return "set_text_contents -> %s" % Atspi.EditableText.set_text_contents(acc, what[9:])
        if what.startswith("insert:"):
            _, pos, text = what.split(":", 2)
            return "insert_text -> %s" % Atspi.EditableText.insert_text(acc, int(pos), text, len(text))
        if what.startswith("set-value:"):
            return "set_current_value -> %s" % Atspi.Value.set_current_value(acc, float(what[10:]))
        if what.startswith("select:"):
            _, a, b = what.split(":")
            return "set_selection -> %s" % Atspi.Text.set_selection(acc, 0, int(a), int(b))
        if what.startswith("caret:"):
            return "set_caret_offset -> %s" % Atspi.Text.set_caret_offset(acc, int(what[6:]))
        if what.startswith("select-child:"):
            return "select_child -> %s" % Atspi.Selection.select_child(acc, int(what[13:]))
        if what == "scroll":
            return "scroll_to -> %s" % Atspi.Component.scroll_to(acc, Atspi.ScrollType.ANYWHERE)
    except GLib.Error as e:
        return "error: %s" % e
    return "unknown %r" % what


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default="Sashfold")
    ap.add_argument("--wait", type=float, default=20)
    ap.add_argument("--start-after", type=float, default=0, help="seconds before asking anything (a tool that comes late)")
    ap.add_argument("--settle", type=float, default=1.0, help="seconds after the app appears before reading")
    ap.add_argument("--max-depth", type=int, default=60)
    ap.add_argument("--stop-file")
    ap.add_argument("--json")
    ap.add_argument("--interfaces", action="store_true", help="also print the interfaces list")
    ap.add_argument("--deep", action="store_true", help="also every other interface, entry by entry")
    ap.add_argument("--events", help="comma-separated event types to record (object:,focus:,window:,document:)")
    ap.add_argument("--go-file", help="created once the listeners are registered")
    ap.add_argument("--events-for", type=float, default=30)
    ap.add_argument("--quiet", type=float, default=4)
    ap.add_argument("--no-tree", action="store_true")
    ap.add_argument("--do", action="append", default=[], help="role|name|what, performed in order before the tree is read")
    args = ap.parse_args()
    global DEEP
    DEEP = args.deep
    time.sleep(args.start_after)
    started = time.time()
    app = find_app(args.app, args.wait)
    if app is None:
        print("NO APP named %r on the accessibility bus" % args.app)
        names = []
        desktop = Atspi.get_desktop(0)
        for i in range(desktop.get_child_count()):
            c = desktop.get_child_at_index(i)
            names.append(c.get_name() if c else None)
        print("apps there: %r" % names)
        return 3
    print("found app after %.2f s" % (time.time() - started))
    if args.events:
        events = record_events(args.events.split(","), args.go_file, args.events_for, args.quiet)
        for e in events:
            print("EVENT %(t)7.3f %(type)s %(source)s d1=%(detail1)s d2=%(detail2)s any=%(any)r" % e)
        print("recorded %d events" % len(events))
        if args.json:
            with open(args.json, "w") as f:
                json.dump(events, f, indent=1, ensure_ascii=False)
        if args.no_tree:
            if args.stop_file:
                open(args.stop_file, "w").close()
            return 0
    time.sleep(args.settle)
    for spec in args.do:
        print("DO %s: %s" % (spec, perform(app, spec)))
        time.sleep(0.6)
    out = []
    walk(app, 0, out, args.max_depth, set())
    for d in out:
        if not args.interfaces:
            d = dict(d)
            d.pop("interfaces", None)
        print(line_of(d))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1, ensure_ascii=False)
    print("read %d objects in %.2f s" % (len(out), time.time() - started))
    if args.stop_file:
        open(args.stop_file, "w").close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
