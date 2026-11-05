#include "bindings/Internal.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// Attr objects and the NamedNodeMap (DOM §4.9.1, §4.9.2). An element's
// attributes are values in its list; what a script holds is an Attr object
// that stands for one of them, and it must stay that one object: the same
// from `attributes[i]`, getAttributeNode() and the map's named properties,
// given back to removeAttributeNode(), and kept by a script after the
// attribute is gone — with its last value, and no owner.
//
// The map is the element's one NamedNodeMap ([SameObject]), live over its
// list, and keeps the Attr objects made for it, one per attribute. A loop
// that removes as it goes (`a = el.attributes; el.removeAttributeNode(a[i]);
// i--`, as template compilers write it) reads the list as it is now.

namespace sashfold::bindings {

namespace {

class AttrObject final : public js::Object {
public:
    AttrObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype, Class::Host)
        , m_realm(realm)
    {
    }

    // The element it is an attribute of, or null: one made by a document's
    // createAttribute(), or removed, has none and keeps its own value.
    NodeWrapper* owner = nullptr;
    std::string namespace_uri;
    std::string prefix;
    std::string local_name;
    std::string value; // while it has no owner

    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(owner);
    }
    std::size_t size_in_bytes() const override
    {
        return Object::size_in_bytes() + namespace_uri.capacity() + prefix.capacity() + local_name.capacity() + value.capacity();
    }

    std::string qualified_name() const { return prefix.empty() ? local_name : prefix + ":" + local_name; }
    bool names(dom::Attr const& attribute) const
    {
        return attribute.local_name == local_name && attribute.namespace_uri == namespace_uri;
    }

private:
    js::RealmRecord* m_realm;
};

class NamedNodeMapObject final : public js::Object {
public:
    NamedNodeMapObject(js::Object* prototype, NodeWrapper& the_element)
        : Object(prototype, Class::Host)
        , element(&the_element)
    {
        mark_uncacheable();
    }

    NodeWrapper* element;
    // The Attr objects made for the element's attributes, which are theirs
    // while the attribute lasts.
    std::vector<AttrObject*> attrs;

    js::RealmRecord* home_realm() const override { return element->home_realm(); }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(element);
        for (AttrObject* attr : attrs)
            tracer.visit(attr);
    }
    std::size_t size_in_bytes() const override { return Object::size_in_bytes() + attrs.capacity() * sizeof(AttrObject*); }

    dom::Element* target() const
    {
        if (element->detached())
            return nullptr;
        return static_cast<dom::Element*>(&element->node());
    }
    std::size_t count() const
    {
        dom::Element const* const e = target();
        return e ? std::as_const(*e).attributes().size() : 0;
    }

    // The Attr object of the element's attribute at `index`, made the first
    // time it is asked for.
    js::Value attr_at(std::size_t index)
    {
        dom::Element* const e = target();
        if (!e || index >= std::as_const(*e).attributes().size())
            return js::Value::null();
        dom::Attr const& attribute = std::as_const(*e).attributes()[index];
        for (AttrObject* attr : attrs) {
            if (attr->names(attribute))
                return js::Value::object(attr);
        }
        Realm::Internals& in = element->realm().internals();
        js::Heap::NoCollect const guard(in.interpreter.heap());
        auto* attr = in.interpreter.heap().allocate<AttrObject>(in.prototype("Attr"), element->home_realm());
        attr->owner = element;
        attr->namespace_uri = attribute.namespace_uri;
        attr->prefix = attribute.prefix;
        attr->local_name = attribute.local_name;
        attrs.push_back(attr);
        return js::Value::object(attr);
    }

    // The first attribute whose qualified name is `name` (lowercased first
    // for an HTML element), as its index.
    std::optional<std::size_t> index_named(std::string_view name) const
    {
        dom::Element const* const e = target();
        if (!e)
            return std::nullopt;
        std::string const wanted = e->is_html() ? ascii_lower(name) : std::string(name);
        std::vector<dom::Attr> const& list = e->attributes();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].has_qualified_name(wanted))
                return i;
        }
        return std::nullopt;
    }
    std::optional<std::size_t> index_of(std::string_view namespace_uri, std::string_view local_name) const
    {
        dom::Element const* const e = target();
        if (!e)
            return std::nullopt;
        std::vector<dom::Attr> const& list = e->attributes();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].namespace_uri == namespace_uri && list[i].local_name == local_name)
                return i;
        }
        return std::nullopt;
    }

    // Its Attr no longer stands for an attribute of the element: it keeps
    // the value it had and has no owner.
    void let_go(std::string_view namespace_uri, std::string_view local_name, std::string value)
    {
        for (auto it = attrs.begin(); it != attrs.end(); ++it) {
            AttrObject* attr = *it;
            if (attr->local_name == local_name && attr->namespace_uri == namespace_uri) {
                attr->owner = nullptr;
                attr->value = std::move(value);
                attrs.erase(it);
                return;
            }
        }
    }

    // The names a script may read as properties (WebIDL's supported
    // property names: the qualified names, but for an HTML element those
    // with an ASCII upper-case letter, which no lookup could reach), when
    // the map or its prototypes have no property of that name.
    std::optional<std::size_t> visible_name(js::PropertyKey const& key) const
    {
        if (key.is_symbol() || key.is_index())
            return std::nullopt;
        if (Object::get_own_property(key) || (prototype() != nullptr && prototype()->has_property(key)))
            return std::nullopt;
        std::string const name = key.as_atom()->to_utf8();
        dom::Element const* const e = target();
        if (!e || (e->is_html() && ascii_lower(name) != name))
            return std::nullopt;
        return index_named(name);
    }

    std::optional<js::PropertyDescriptor> get_own_property(js::PropertyKey const& key) const override
    {
        auto* self = const_cast<NamedNodeMapObject*>(this);
        if (key.is_index()) {
            if (key.as_index() >= count())
                return std::nullopt;
            return js::PropertyDescriptor::data(self->attr_at(key.as_index()), js::Enumerable | js::Configurable);
        }
        if (std::optional<std::size_t> const index = visible_name(key))
            return js::PropertyDescriptor::data(self->attr_at(*index), js::Configurable);
        return Object::get_own_property(key);
    }

    std::optional<js::Value> get(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& receiver) override
    {
        if (key.is_index()) {
            if (key.as_index() < count())
                return attr_at(key.as_index());
            return Object::get(interpreter, key, receiver);
        }
        if (std::optional<std::size_t> const index = visible_name(key))
            return attr_at(*index);
        return Object::get(interpreter, key, receiver);
    }

    bool has_property(js::PropertyKey const& key) const override
    {
        if (key.is_index() && key.as_index() < count())
            return true;
        if (visible_name(key))
            return true;
        return Object::has_property(key);
    }

    std::optional<bool> set(js::Interpreter& interpreter, js::PropertyKey const& key, js::Value const& value, js::Value const& receiver) override
    {
        if (key.is_index() && key.as_index() < count())
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
            return key.as_index() >= count();
        if (visible_name(key))
            return false;
        return Object::delete_property(key);
    }

    std::vector<js::PropertyKey> own_keys() const override
    {
        std::vector<js::PropertyKey> keys;
        std::size_t const n = count();
        for (std::size_t i = 0; i < n; ++i)
            keys.push_back(js::PropertyKey::index(static_cast<std::uint32_t>(i)));
        std::vector<js::PropertyKey> const own = Object::own_keys();
        if (dom::Element const* const e = target()) {
            std::vector<std::string> seen;
            for (dom::Attr const& attribute : e->attributes()) {
                std::string const name = attribute.qualified_name();
                if ((e->is_html() && ascii_lower(name) != name) || std::find(seen.begin(), seen.end(), name) != seen.end())
                    continue;
                bool const looks_like_index = !name.empty() && name.find_first_not_of("0123456789") == std::string::npos;
                if (looks_like_index)
                    continue;
                seen.push_back(name);
                js::PropertyKey const key = heap()->key(name);
                if (!Object::get_own_property(key) && !(prototype() != nullptr && prototype()->has_property(key)))
                    keys.push_back(key);
            }
        }
        keys.insert(keys.end(), own.begin(), own.end());
        return keys;
    }
};

AttrObject* attr_of(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<AttrObject*>(value.as_object()) : nullptr;
}

NamedNodeMapObject* map_of(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<NamedNodeMapObject*>(value.as_object()) : nullptr;
}

NamedNodeMapObject& map_for(Realm::Internals& in, dom::Element& element)
{
    NodeWrapper& wrapper = wrapper_for(in, element);
    if (auto* const kept = dynamic_cast<NamedNodeMapObject*>(wrapper.same_object("attributes")))
        return *kept;
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(js::Value::object(&wrapper));
    auto* map = in.interpreter.heap().allocate<NamedNodeMapObject>(in.prototype("NamedNodeMap"), wrapper);
    wrapper.keep_same_object("attributes", map);
    return *map;
}

// The element an Attr stands for an attribute of, while it does.
dom::Element* owner_element(AttrObject const& attr)
{
    if (attr.owner == nullptr || attr.owner->detached())
        return nullptr;
    auto* const element = static_cast<dom::Element*>(&attr.owner->node());
    for (dom::Attr const& attribute : std::as_const(*element).attributes()) {
        if (attr.names(attribute))
            return element;
    }
    return nullptr;
}

dom::Attr const* attribute_of(AttrObject const& attr)
{
    dom::Element const* const element = owner_element(attr);
    if (!element)
        return nullptr;
    for (dom::Attr const& attribute : element->attributes()) {
        if (attr.names(attribute))
            return &attribute;
    }
    return nullptr;
}

std::optional<AttrObject*> this_attr(js::Interpreter& interp, js::Value const& this_value)
{
    if (AttrObject* const attr = attr_of(this_value))
        return attr;
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

std::optional<NamedNodeMapObject*> this_map(js::Interpreter& interp, js::Value const& this_value)
{
    if (NamedNodeMapObject* const map = map_of(this_value))
        return map;
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

js::Value nullable(Realm::Internals& in, std::string const& text)
{
    return text.empty() ? js::Value::null() : in.string(text);
}

Native attr_value(js::Interpreter& interp, js::Value const& this_value, Args)
{
    std::optional<AttrObject*> const attr = this_attr(interp, this_value);
    if (!attr)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    if (dom::Attr const* const attribute = attribute_of(**attr))
        return in.string(attribute->value);
    return in.string((*attr)->value);
}

// Setting the value of an Attr that stands for an attribute changes the
// attribute (DOM §4.9.2 "set an existing attribute value"); one with no
// owner keeps it.
Native set_attr_value(js::Interpreter& interp, js::Value const& this_value, Args args)
{
    std::optional<AttrObject*> const attr = this_attr(interp, this_value);
    if (!attr)
        return std::nullopt;
    Realm::Internals& in = internals_of(interp);
    js::Value const given = js::argument(args, 0);
    std::optional<std::string> value = given.is_null() ? std::optional<std::string>("") : in.to_utf8(given);
    if (!value)
        return std::nullopt;
    if (dom::Element* const element = owner_element(**attr)) {
        AttrObject const& a = **attr;
        js::Interpreter::Roots const roots(interp);
        interp.root(this_value);
        std::optional<std::string> verified = trusted_attribute_value(in, *element, a.namespace_uri, a.local_name, interp.root(in.string(*value)));
        if (!verified)
            return std::nullopt;
        set_attribute_ns(in, *element, a.namespace_uri, a.prefix, a.local_name, std::move(*verified));
    } else {
        (*attr)->value = std::move(*value);
    }
    return js::Value::undefined();
}

} // namespace

js::Value attributes_of(Realm::Internals& in, dom::Element& element)
{
    return js::Value::object(&map_for(in, element));
}

js::Value attribute_node(Realm::Internals& in, dom::Element& element, std::size_t index)
{
    return map_for(in, element).attr_at(index);
}

js::Value new_attr(Realm::Internals& in, std::string namespace_uri, std::string prefix, std::string local_name, std::string value)
{
    js::Heap::NoCollect const guard(in.interpreter.heap());
    auto* attr = in.interpreter.heap().allocate<AttrObject>(in.prototype("Attr"), in.realm_record);
    attr->namespace_uri = std::move(namespace_uri);
    attr->prefix = std::move(prefix);
    attr->local_name = std::move(local_name);
    attr->value = std::move(value);
    return js::Value::object(attr);
}

void attribute_let_go(dom::Element& element, std::string_view namespace_uri, std::string_view local_name, std::string const& old_value)
{
    auto* const wrapper = dynamic_cast<NodeWrapper*>(element.wrapper);
    if (!wrapper)
        return;
    if (auto* const map = dynamic_cast<NamedNodeMapObject*>(wrapper->same_object("attributes")))
        map->let_go(namespace_uri, local_name, old_value);
}

// setAttributeNode / setAttributeNodeNS / setNamedItem (DOM §4.9 "set an
// attribute"): the given Attr becomes the element's attribute of its
// namespace and local name, in the place of the one there (which is
// answered, with no owner now), and is answered as the same object after.
Native set_attribute_node(Realm::Internals& in, dom::Element& element, js::Value const& given)
{
    AttrObject* const attr = attr_of(given);
    if (!attr)
        return in.interpreter.throw_type_error("parameter 1 is not of type 'Attr'.");
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(given);
    // The value is verified for the element first (Trusted Types), before
    // either check below.
    dom::Attr const* const standing = attribute_of(*attr);
    std::optional<std::string> verified = trusted_attribute_value(in, element, attr->namespace_uri, attr->local_name,
        in.interpreter.root(in.string(standing ? standing->value : attr->value)));
    if (!verified)
        return std::nullopt;
    NamedNodeMapObject& map = map_for(in, element);
    in.interpreter.root(js::Value::object(&map));
    if (dom::Element const* const holder = owner_element(*attr)) {
        if (holder != &element)
            return in.throw_dom_exception("InUseAttributeError", "The attribute is in use by another element.");
        return given;
    }
    js::Value previous = js::Value::null();
    std::string old_value;
    if (std::optional<std::size_t> const index = map.index_of(attr->namespace_uri, attr->local_name)) {
        previous = in.interpreter.root(map.attr_at(*index));
        old_value = std::as_const(element).attributes()[*index].value;
    }
    if (attr_of(previous) == attr)
        return given;
    // The old Attr lets go first: the new one takes its place in the map.
    map.let_go(attr->namespace_uri, attr->local_name, old_value);
    attr->owner = &wrapper_for(in, element);
    map.attrs.push_back(attr);
    attr->value.clear();
    set_attribute_ns(in, element, attr->namespace_uri, attr->prefix, attr->local_name, std::move(*verified));
    for (dom::Attr& attribute : element.attributes()) {
        if (attr->names(attribute))
            attribute.prefix = attr->prefix;
    }
    return previous;
}

// removeAttributeNode / removeNamedItem: the attribute the given Attr stands
// for, which must be one of the element's.
Native remove_attribute_node(Realm::Internals& in, dom::Element& element, js::Value const& given)
{
    AttrObject* const attr = attr_of(given);
    if (!attr)
        return in.interpreter.throw_type_error("parameter 1 is not of type 'Attr'.");
    if (owner_element(*attr) != &element)
        return in.throw_dom_exception("NotFoundError", "The node provided is not an attribute of this element.");
    js::Interpreter::Roots const roots(in.interpreter);
    in.interpreter.root(given);
    remove_attribute_ns(in, element, attr->namespace_uri, attr->local_name);
    return given;
}

void install_attributes(Realm::Internals& setup, js::Object& attr_prototype, js::Object& map_prototype)
{
    js::Interpreter& interpreter = setup.interpreter;
    define_getter(setup, attr_prototype, "name", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AttrObject*> const attr = this_attr(interp, this_value);
        if (!attr)
            return std::nullopt;
        return internals_of(interp).string((*attr)->qualified_name());
    });
    define_getter(setup, attr_prototype, "nodeName", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AttrObject*> const attr = this_attr(interp, this_value);
        if (!attr)
            return std::nullopt;
        return internals_of(interp).string((*attr)->qualified_name());
    });
    define_getter(setup, attr_prototype, "localName", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AttrObject*> const attr = this_attr(interp, this_value);
        if (!attr)
            return std::nullopt;
        return internals_of(interp).string((*attr)->local_name);
    });
    define_getter(setup, attr_prototype, "namespaceURI", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AttrObject*> const attr = this_attr(interp, this_value);
        if (!attr)
            return std::nullopt;
        return nullable(internals_of(interp), (*attr)->namespace_uri);
    });
    define_getter(setup, attr_prototype, "prefix", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AttrObject*> const attr = this_attr(interp, this_value);
        if (!attr)
            return std::nullopt;
        return nullable(internals_of(interp), (*attr)->prefix);
    });
    define_getter(setup, attr_prototype, "ownerElement", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AttrObject*> const attr = this_attr(interp, this_value);
        if (!attr)
            return std::nullopt;
        if (owner_element(**attr) == nullptr)
            return js::Value::null();
        return js::Value::object((*attr)->owner);
    });
    define_getter(setup, attr_prototype, "specified", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_attr(interp, this_value))
            return std::nullopt;
        return js::Value::boolean(true);
    });
    define_getter(setup, attr_prototype, "nodeType", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_attr(interp, this_value))
            return std::nullopt;
        return js::Value::number(2);
    });
    for (std::string_view const name : { "value", "nodeValue", "textContent" })
        define_getter(setup, attr_prototype, name, attr_value, set_attr_value);

    define_getter(setup, map_prototype, "length", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
        if (!map)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*map)->count()));
    });
    define_operation(interpreter, map_prototype, "item", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
        if (!map)
            return std::nullopt;
        std::optional<double> const index = interp.to_number(js::argument(args, 0));
        if (!index)
            return std::nullopt;
        return (*map)->attr_at(to_unsigned_long(*index));
    });
    define_operation(interpreter, map_prototype, "getNamedItem", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
        if (!map)
            return std::nullopt;
        std::optional<std::string> const name = internals_of(interp).to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<std::size_t> const index = (*map)->index_named(*name);
        return index ? (*map)->attr_at(*index) : js::Value::null();
    });
    define_operation(interpreter, map_prototype, "getNamedItemNS", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
        if (!map)
            return std::nullopt;
        Realm::Internals& in = internals_of(interp);
        js::Value const space = js::argument(args, 0);
        std::optional<std::string> const namespace_uri = space.is_nullish() ? std::optional<std::string>("") : in.to_utf8(space);
        if (!namespace_uri)
            return std::nullopt;
        std::optional<std::string> const local = in.to_utf8(js::argument(args, 1));
        if (!local)
            return std::nullopt;
        std::optional<std::size_t> const index = (*map)->index_of(*namespace_uri, *local);
        return index ? (*map)->attr_at(*index) : js::Value::null();
    });
    for (std::string_view const name : { "setNamedItem", "setNamedItemNS" }) {
        define_operation(interpreter, map_prototype, name, 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
            std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
            if (!map)
                return std::nullopt;
            dom::Element* const element = (*map)->target();
            if (!element)
                return js::Value::null();
            return set_attribute_node(internals_of(interp), *element, js::argument(args, 0));
        });
    }
    define_operation(interpreter, map_prototype, "removeNamedItem", 1, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
        if (!map)
            return std::nullopt;
        Realm::Internals& in = internals_of(interp);
        std::optional<std::string> const name = in.to_utf8(js::argument(args, 0));
        if (!name)
            return std::nullopt;
        std::optional<std::size_t> const index = (*map)->index_named(*name);
        dom::Element* const element = (*map)->target();
        if (!index || !element)
            return in.throw_dom_exception("NotFoundError", "No item with name '" + *name + "' was found.");
        js::Interpreter::Roots const roots(interp);
        js::Value const attr = interp.root((*map)->attr_at(*index));
        return remove_attribute_node(in, *element, attr);
    });
    define_operation(interpreter, map_prototype, "removeNamedItemNS", 2, [](js::Interpreter& interp, js::Value const& this_value, Args args) -> Native {
        std::optional<NamedNodeMapObject*> const map = this_map(interp, this_value);
        if (!map)
            return std::nullopt;
        Realm::Internals& in = internals_of(interp);
        js::Value const space = js::argument(args, 0);
        std::optional<std::string> const namespace_uri = space.is_nullish() ? std::optional<std::string>("") : in.to_utf8(space);
        if (!namespace_uri)
            return std::nullopt;
        std::optional<std::string> const local = in.to_utf8(js::argument(args, 1));
        if (!local)
            return std::nullopt;
        std::optional<std::size_t> const index = (*map)->index_of(*namespace_uri, *local);
        dom::Element* const element = (*map)->target();
        if (!index || !element)
            return in.throw_dom_exception("NotFoundError", "No item with that namespace and local name was found.");
        js::Interpreter::Roots const roots(interp);
        js::Value const attr = interp.root((*map)->attr_at(*index));
        return remove_attribute_node(in, *element, attr);
    });
}

}
