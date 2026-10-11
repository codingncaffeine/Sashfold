#include "bindings/Internal.h"
#include "bindings/NodeSupport.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// Live collections (DOM §4.2.10, HTML §2.7.2): a NodeList or HTMLCollection
// whose members are whatever its query finds under its root now. A script
// keeps one and reads it again after the tree changed —
// document.getElementsByTagName('script') read in one inline script and
// again in the next, which has parsed more of them since; a loop over
// childNodes that removes as it goes — and expects to see the change.
//
// The members are found again only when the tree, or for a query that reads
// attributes the attributes, changed since they were last found: the
// document counts both, so that a loop reading `length` and `[i]` over an
// unchanged tree costs nothing per read.
//
// An HTMLCollection also answers for its members' ids and names
// (namedItem, and `collection.someName`), as unenumerable properties a
// prototype member of the same name hides. A form's controls collection
// answers a name several controls share with a RadioNodeList of them.

namespace sashfold::bindings {

namespace {

// The elements document.all finds by their name attribute (HTML §2.7.2.1).
bool all_named_by_name(dom::Element const& element)
{
    if (!element.is_html())
        return false;
    static constexpr std::string_view names[] = { "a", "button", "embed", "form", "frame", "frameset", "iframe", "img", "input", "map",
        "meta", "object", "select", "textarea" };
    return std::find(std::begin(names), std::end(names), element.local_name()) != std::end(names);
}

class LiveCollectionObject final : public js::Object {
public:
    LiveCollectionObject(js::Object* prototype, NodeWrapper& the_root, CollectMembers the_collect, bool the_attributes, Naming the_naming)
        : Object(prototype, Class::Host)
        , root(&the_root)
        , collect(std::move(the_collect))
        , attributes(the_attributes)
        , naming(the_naming)
    {
        mark_uncacheable();
    }

    NodeWrapper* root;
    CollectMembers collect;
    bool attributes; // its query reads attributes, not only the tree
    Naming naming;

    js::RealmRecord* home_realm() const override { return root->home_realm(); }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(root);
    }
    std::size_t size_in_bytes() const override { return Object::size_in_bytes() + m_members.capacity() * sizeof(dom::Node*); }

    std::vector<dom::Node*> const& members() const
    {
        if (root->detached()) {
            m_members.clear();
            return m_members;
        }
        dom::Node& node = root->node();
        dom::Document const& document = node.type() == dom::NodeType::Document ? static_cast<dom::Document const&>(node) : node.document();
        std::uint64_t const tree = document.tree_version();
        std::uint64_t const attribute = attributes ? document.attribute_version() : 0;
        if (!m_found || tree != m_tree || attribute != m_attribute) {
            m_members.clear();
            collect(root->realm().internals(), node, m_members);
            m_found = true;
            m_tree = tree;
            m_attribute = attribute;
        }
        return m_members;
    }

    Realm::Internals& internals() const { return root->realm().internals(); }

    // What `name` names in the collection: nothing, one element, or (for a
    // form's controls) the several that share it.
    js::Value named(std::string_view name) const
    {
        if (naming == Naming::None || name.empty() || root->detached())
            return js::Value::undefined();
        std::vector<dom::Node*> found;
        for (dom::Node* node : members()) {
            if (!node->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*node);
            dom::Attr const* const id = element.find_attribute("id");
            bool const by_name = naming == Naming::All ? all_named_by_name(element) : element.is_html();
            dom::Attr const* const name_attribute = by_name ? element.find_attribute("name") : nullptr;
            if ((id != nullptr && id->value == name) || (name_attribute != nullptr && name_attribute->value == name)) {
                found.push_back(node);
                if (naming == Naming::Elements)
                    break;
            }
        }
        if (found.empty())
            return js::Value::undefined();
        Realm::Internals& in = internals();
        if (found.size() == 1)
            return js::Value::object(in.wrap(*found.front()));
        js::Interpreter::Roots const roots(in.interpreter);
        js::Value const list = in.interpreter.root(node_list(in, found));
        list.as_object()->set_prototype(in.prototype(naming == Naming::All ? "HTMLCollection" : "RadioNodeList"));
        return list;
    }

    // The names it answers for, in tree order: each member's id, then its
    // name, once each (HTMLCollection's supported property names).
    std::vector<std::string> names() const
    {
        std::vector<std::string> out;
        if (naming == Naming::None)
            return out;
        for (dom::Node const* node : members()) {
            if (!node->is_element())
                continue;
            auto const& element = static_cast<dom::Element const&>(*node);
            for (std::string_view const attribute : { "id", "name" }) {
                if (attribute == "name" && !(naming == Naming::All ? all_named_by_name(element) : element.is_html()))
                    continue;
                dom::Attr const* const found = element.find_attribute(attribute);
                if (found != nullptr && !found->value.empty() && std::find(out.begin(), out.end(), found->value) == out.end())
                    out.push_back(found->value);
            }
        }
        return out;
    }

    // A name a script may see as a property: one the collection answers for
    // that is neither its own property nor one up its prototypes.
    std::optional<std::string> visible_name(js::PropertyKey const& key) const
    {
        if (naming == Naming::None || key.is_symbol() || key.is_index())
            return std::nullopt;
        if (Object::get_own_property(key) || (prototype() != nullptr && prototype()->has_property(key)))
            return std::nullopt;
        std::string name = key.as_atom()->to_utf8();
        if (named(name).is_undefined())
            return std::nullopt;
        return name;
    }

    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const& key) const override
    {
        if (key.is_index()) {
            std::vector<dom::Node*> const& list = members();
            if (key.as_index() >= list.size())
                return std::nullopt;
            return js::PropertyDescriptor::data(js::Value::object(internals().wrap(*list[key.as_index()])), js::Enumerable | js::Configurable);
        }
        if (std::optional<std::string> const name = visible_name(key))
            return js::PropertyDescriptor::data(named(*name), js::Configurable);
        return Object::get_own_property(key);
    }

    std::optional<js::Value> get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver) override
    {
        if (key.is_index()) {
            std::vector<dom::Node*> const& list = members();
            if (key.as_index() < list.size())
                return js::Value::object(internals().wrap(*list[key.as_index()]));
            return Object::get(interpreter, key, receiver);
        }
        if (std::optional<std::string> const name = visible_name(key))
            return named(*name);
        return Object::get(interpreter, key, receiver);
    }

    bool has_property(js::PropertyKey const& key) const override
    {
        if (key.is_index() && key.as_index() < members().size())
            return true;
        if (visible_name(key))
            return true;
        return Object::has_property(key);
    }

    std::optional<bool> set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver) override
    {
        // No indexed or named setter: what they answer is read-only.
        if (key.is_index() && key.as_index() < members().size())
            return false;
        if (visible_name(key))
            return false;
        return Object::set(interpreter, key, value, receiver);
    }

    bool define_own_property(js::PropertyKey const& key, js::PropertyDescriptor const& descriptor) override
    {
        if (key.is_index())
            return false;
        if (visible_name(key))
            return false;
        return Object::define_own_property(key, descriptor);
    }

    bool delete_property(js::PropertyKey const& key) override
    {
        if (key.is_index())
            return key.as_index() >= members().size();
        if (visible_name(key))
            return false;
        return Object::delete_property(key);
    }

    std::vector<js::PropertyKey> own_keys() const override
    {
        std::vector<js::PropertyKey> keys;
        std::size_t const count = members().size();
        for (std::size_t i = 0; i < count; ++i)
            keys.push_back(js::PropertyKey::index(static_cast<std::uint32_t>(i)));
        std::vector<js::PropertyKey> const own = Object::own_keys();
        for (std::string const& name : names()) {
            js::PropertyKey const key = heap()->key(name);
            if (!Object::get_own_property(key))
                keys.push_back(key);
        }
        keys.insert(keys.end(), own.begin(), own.end());
        return keys;
    }

private:
    mutable std::vector<dom::Node*> m_members;
    mutable bool m_found = false;
    mutable std::uint64_t m_tree = 0;
    mutable std::uint64_t m_attribute = 0;
};

LiveCollectionObject* live_of(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<LiveCollectionObject*>(value.as_object()) : nullptr;
}

}

js::Value live_collection(Realm::Internals& in, dom::Node& root, std::string_view interface, CollectMembers collect, bool attributes, Naming naming)
{
    NodeWrapper& wrapper = wrapper_for(in, root);
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&wrapper));
    return js::Value::object(
        in.interpreter.heap().allocate<LiveCollectionObject>(in.prototype(interface), wrapper, std::move(collect), attributes, naming));
}

js::Value same_live_collection(Realm::Internals& in, dom::Node& root, std::string_view key, std::string_view interface, CollectMembers collect,
    bool attributes, Naming naming)
{
    NodeWrapper& wrapper = wrapper_for(in, root);
    if (js::Object* const kept = wrapper.same_object(key))
        return js::Value::object(kept);
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&wrapper));
    js::Value const made = live_collection(in, root, interface, std::move(collect), attributes, naming);
    wrapper.keep_same_object(key, made.as_object());
    return made;
}

std::optional<std::size_t> collection_length(js::Value const& value)
{
    if (LiveCollectionObject const* const live = live_of(value))
        return live->members().size();
    if (js::Interpreter::is_array(value))
        return static_cast<js::ArrayObject const*>(value.as_object())->length();
    return std::nullopt;
}

js::Value collection_item(js::Value const& value, std::size_t index)
{
    if (LiveCollectionObject const* const live = live_of(value)) {
        std::vector<dom::Node*> const& list = live->members();
        return index < list.size() ? js::Value::object(live->internals().wrap(*list[index])) : js::Value::null();
    }
    auto const* const array = static_cast<js::ArrayObject const*>(value.as_object());
    if (index >= array->length())
        return js::Value::null();
    js::Value const element = array->element(static_cast<std::uint32_t>(index));
    return element.is_empty() ? js::Value::null() : element;
}

std::optional<js::Value> collection_named_item(js::Value const& value, std::string_view name)
{
    LiveCollectionObject const* const live = live_of(value);
    if (live == nullptr)
        return std::nullopt;
    js::Value const found = live->named(name);
    return found.is_undefined() ? js::Value::null() : found;
}

}
