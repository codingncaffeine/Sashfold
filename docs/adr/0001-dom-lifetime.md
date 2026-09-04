# ADR 0001 — Who owns a DOM node once a script can hold it

**Status:** accepted 2026-09-04. **Applies to:** `src/dom`, `src/js`,
`src/bindings`.

## Context

The HTML parser builds a node soup: the `Document` owns every node it ever
created in a vector of `unique_ptr`, and the tree is raw parent/child links.
That was chosen for parsing, where reparenting (foster parenting, the adoption
agency) is constant and must never be an ownership move. It leaves one question
open, and it is the question every engine answers differently: **what keeps a
node alive when a script holds a reference to it and the tree does not?**

Three shapes ship today. Blink traces the DOM with a garbage collector of its
own and lets it cooperate with the script heap. WebKit counts references on
nodes and lets the script collector treat a connected tree as one *opaque
root*: a wrapper is live while its tree is. Gecko counts references and runs a
separate cycle collector over nodes, listeners and the script heap together.
The choice made here is the second family — **reference counts on nodes, with
the cycle work done by the script heap's own collector** — because Sashfold's
script engine is its own, so the collector that must understand the DOM is one
we write anyway, and because a counted node is a node C++ code can hold without
a collector in the loop, which the layout and paint code does everywhere.

## Decision

1. **A wrapper is one script object per node, cached on the node.** A node has
   at most one wrapper for its lifetime; `Node::wrapper()` is that object or
   null. Expando properties and event listeners live on the wrapper, never on
   the node, so a node the script cannot reach has nothing the script could
   miss.

2. **A connected tree is an opaque root.** When the script heap collects, it
   marks the wrapper of every node that is connected to a live document. A
   listener or an expando on a connected element therefore lives as long as the
   element is in the tree, whatever the script still references — which is what
   every page expects of `el.addEventListener` and `el.myData = …`.

3. **A detached subtree lives as long as a wrapper into it is reachable.** A
   wrapper holds a counted reference to its node; a node holds counted
   references to its children and a plain pointer to its parent. Reaching any
   node of a detached subtree from script keeps the whole subtree alive, and
   nothing else does. When the last wrapper into it is swept the counts fall to
   zero and the nodes are freed. There is no cycle between a wrapper and its
   node: the node does not own the wrapper, the collector does, and the
   collector consults the tree rather than the node's count.

4. **A realm dies with its document.** Navigation replaces the document and
   with it the script heap it was scripted by; nothing from the old realm
   survives. Cross-document adoption (`adoptNode`, frames) therefore carries a
   node between two owners, and is the one place a count has to be moved rather
   than touched.

5. **The counts are staged behind the arena.** Today the document still owns
   every node it created, and a node removed from the tree is kept in the
   document's arena until the document goes. Under that rule every wrapper's
   pointer is valid for the life of its realm without a count, and a detached
   node's memory is held at most until navigation. The counts in (3) replace the
   arena when a measurement says the arena is the problem: resident memory on a
   page that churns nodes for a long time. Until then the semantics are exactly
   those above and only the moment of freeing differs.

## Consequences

- `dom::Node` grows one slot, the wrapper pointer, and later a count. It does
  not grow listener storage.
- The script heap's collector walks the document tree once per collection to
  mark connected wrappers. That is a linear pass over the DOM, which is what
  the opaque-root rule costs everywhere it is used.
- Bindings code never holds a bare node pointer across a collection except
  through a wrapper it has rooted; the collector's stress mode, which collects
  at every allocation, is how a missing root is found.
- A document can be freed as a unit, which keeps navigation cheap and keeps
  the parser's ownership rule intact.
