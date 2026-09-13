#include "js/Runtime.h"

// The Proxy exotic object (§10.5) and the Proxy constructor (§28.2): an
// object whose every essential internal method is a call to a trap of the
// same name on a handler, falling through to the target when the handler
// has no such trap. What makes it more than a forwarding layer is the
// invariant checks each method ends with: a target may already have
// committed to a non-configurable property, to a prototype, or to being
// non-extensible, and no trap may report the world as contradicting a
// commitment the target has made. Every such check is a TypeError.
//
// A trap can run any script at all, including script that revokes this
// very proxy — so each method roots the target and the handler it read
// before the call, not the proxy, since revocation lets the proxy's own
// slots go.

#include "js/Object.h"
#include "js/Strings.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

namespace {

// CompletePropertyDescriptor (§6.2.6.6): the fields a trap left out take
// their defaults, so that the invariant checks compare like with like.
void complete_property_descriptor(PropertyDescriptor& desc)
{
    if (desc.is_accessor()) {
        if (!desc.get)
            desc.get = nullptr;
        if (!desc.set)
            desc.set = nullptr;
    } else {
        if (!desc.value)
            desc.value = Value::undefined();
        if (!desc.writable)
            desc.writable = false;
    }
    if (!desc.enumerable)
        desc.enumerable = false;
    if (!desc.configurable)
        desc.configurable = false;
}

// A property the target cannot drop or redefine: present, and not
// configurable. Most of §10.5's invariants turn on exactly this.
bool is_non_configurable(std::optional<PropertyDescriptor> const& desc)
{
    return desc.has_value() && !desc->configurable.value_or(false);
}

// The first steps every internal method of §10.5 shares: the handler,
// which a revoked proxy no longer has; the target; and the trap of this
// name, read with GetMethod so that an absent or null one means "do what
// the target would" and a non-callable one is a TypeError.
struct Trap {
    Object* target = nullptr;
    Object* handler = nullptr;
    Value function = Value::undefined(); // undefined = the handler has no such trap
    bool present() const { return !function.is_undefined(); }
};

std::optional<Trap> find_trap(Interpreter& in, ProxyObject& proxy, std::string_view name)
{
    // A handler with no trap of this name answers by putting the same
    // question to the target, so a proxy whose target is a proxy is native
    // recursion one frame deep per link — and how deep the links go is the
    // script's to choose. The chain is held to the same stack budget as any
    // other recursion: past it a RangeError, never a crash.
    if (!in.stack_ok())
        return std::nullopt;
    if (proxy.is_revoked())
        return in.throw_type_error("Cannot perform '" + std::string(name) + "' on a proxy that has been revoked");
    Trap trap;
    trap.target = proxy.target();
    trap.handler = proxy.handler();
    // Rooted into the CALLER's scope, and deliberately so: a trap may
    // revoke this proxy, after which the proxy traces neither of them and
    // only these roots keep the two objects alive for the checks below.
    in.root(Value::object(trap.target));
    in.root(Value::object(trap.handler));
    std::optional<Value> const function = in.get_method(Value::object(trap.handler), in.key(name));
    if (!function)
        return std::nullopt;
    trap.function = *function;
    in.root(trap.function);
    return trap;
}

// A cell a proxy's internal method invented and is handing back: it is
// referenced by nothing else once the method's own root scope closes, so
// it is pushed into the caller's. Used for the two answers whose callers
// cannot root them themselves — a descriptor and a key list.
void root_descriptor(Interpreter& in, PropertyDescriptor const& desc)
{
    if (desc.value)
        in.root(*desc.value);
    if (desc.get && *desc.get)
        in.root(Value::object(*desc.get));
    if (desc.set && *desc.set)
        in.root(Value::object(*desc.set));
}

void root_keys(Interpreter& in, std::vector<PropertyKey> const& keys)
{
    for (PropertyKey const& key : keys) {
        if (key.is_symbol())
            in.root(Value::symbol(key.as_symbol()));
    }
}

// ProxyCreate (§10.5.14). Callability is decided once, here: [[Call]] and
// [[Construct]] are present only if the target had them when the proxy was
// made, whatever the target becomes afterwards.
std::optional<ProxyObject*> proxy_create(Interpreter& in, Value const& target, Value const& handler)
{
    if (!target.is_object())
        return in.throw_type_error("Cannot create proxy with a non-object as target");
    if (!handler.is_object())
        return in.throw_type_error("Cannot create proxy with a non-object as handler");
    return in.heap().allocate<ProxyObject>(target.as_object(), handler.as_object());
}

} // namespace

// ------------------------------------------------ the internal methods

std::optional<Object*> ProxyObject::get_prototype_of(Interpreter& in)
{
    // §10.5.1: the answer must be an object or null, and a target that can
    // no longer change its prototype may not be reported as having a
    // different one than it has.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    std::optional<Trap> const trap = find_trap(in, *this, "getPrototypeOf");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.get_prototype_of(*trap->target);
    Value const arguments[1] = { Value::object(trap->target) };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    if (!result->is_object() && !result->is_null())
        return in.throw_type_error("'getPrototypeOf' on proxy: trap returned neither object nor null");
    in.root(*result);
    Object* const answer = result->is_null() ? nullptr : result->as_object();
    std::optional<bool> const extensible = in.is_extensible(*trap->target);
    if (!extensible)
        return std::nullopt;
    if (*extensible)
        return answer;
    std::optional<Object*> const target_prototype = in.get_prototype_of(*trap->target);
    if (!target_prototype)
        return std::nullopt;
    if (answer != *target_prototype)
        return in.throw_type_error("'getPrototypeOf' on proxy: proxy target is non-extensible but the trap did not return its actual prototype");
    return answer;
}

std::optional<bool> ProxyObject::set_prototype_of(Interpreter& in, Object* prototype)
{
    // §10.5.2: a refusal passes through as it is; a claim of success on a
    // non-extensible target must name the prototype the target already has.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    if (prototype != nullptr)
        in.root(Value::object(prototype));
    std::optional<Trap> const trap = find_trap(in, *this, "setPrototypeOf");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.set_prototype_of(*trap->target, prototype);
    Value const arguments[2] = { Value::object(trap->target), prototype ? Value::object(prototype) : Value::null() };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    if (!Interpreter::to_boolean(*result))
        return false;
    std::optional<bool> const extensible = in.is_extensible(*trap->target);
    if (!extensible)
        return std::nullopt;
    if (*extensible)
        return true;
    std::optional<Object*> const target_prototype = in.get_prototype_of(*trap->target);
    if (!target_prototype)
        return std::nullopt;
    if (prototype != *target_prototype)
        return in.throw_type_error("'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on the non-extensible proxy target");
    return true;
}

std::optional<bool> ProxyObject::is_extensible(Interpreter& in)
{
    // §10.5.3: a proxy has no extensibility of its own at all, and the
    // trap is not allowed to disagree with the target about the target's.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    std::optional<Trap> const trap = find_trap(in, *this, "isExtensible");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.is_extensible(*trap->target);
    Value const arguments[1] = { Value::object(trap->target) };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    bool const answer = Interpreter::to_boolean(*result);
    std::optional<bool> const target = in.is_extensible(*trap->target);
    if (!target)
        return std::nullopt;
    if (answer != *target)
        return in.throw_type_error("'isExtensible' on proxy: trap result does not reflect extensibility of proxy target");
    return answer;
}

std::optional<bool> ProxyObject::prevent_extensions(Interpreter& in)
{
    // §10.5.4: a trap may claim success only once the target really has
    // stopped being extensible.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    std::optional<Trap> const trap = find_trap(in, *this, "preventExtensions");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.prevent_extensions(*trap->target);
    Value const arguments[1] = { Value::object(trap->target) };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    if (!Interpreter::to_boolean(*result))
        return false;
    std::optional<bool> const extensible = in.is_extensible(*trap->target);
    if (!extensible)
        return std::nullopt;
    if (*extensible)
        return in.throw_type_error("'preventExtensions' on proxy: trap returned truish but the proxy target is extensible");
    return true;
}

std::optional<std::optional<PropertyDescriptor>> ProxyObject::get_own_property(Interpreter& in, PropertyKey const& key)
{
    // §10.5.5, five rules deep. A property the target cannot drop may not
    // be reported missing; a property of a non-extensible target may not
    // be reported missing either; an invented descriptor must be one the
    // target could have accepted; and a descriptor claiming to be
    // non-configurable — read-only with it, when it says so — needs a
    // property of that kind actually standing behind it on the target.
    std::optional<std::optional<PropertyDescriptor>> answer;
    {
        Interpreter::Roots const roots(in);
        in.root(Value::object(this));
        if (key.is_symbol())
            in.root(Value::symbol(key.as_symbol()));
        std::optional<Trap> const trap = find_trap(in, *this, "getOwnPropertyDescriptor");
        if (!trap)
            return std::nullopt;
        if (!trap->present()) {
            answer = in.get_own_property(*trap->target, key);
            if (!answer)
                return std::nullopt;
        } else {
            Value const arguments[2] = { Value::object(trap->target), key_to_value(in, key) };
            std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
            if (!result)
                return std::nullopt;
            if (!result->is_object() && !result->is_undefined())
                return in.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined");
            in.root(*result);
            std::optional<std::optional<PropertyDescriptor>> const target_desc = in.get_own_property(*trap->target, key);
            if (!target_desc)
                return std::nullopt;
            if (target_desc->has_value())
                root_descriptor(in, **target_desc);
            if (result->is_undefined()) {
                if (!target_desc->has_value())
                    return std::optional<PropertyDescriptor> {};
                if (is_non_configurable(*target_desc))
                    return in.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned undefined for property '"
                        + key_description(key) + "' which is non-configurable in the proxy target");
                std::optional<bool> const extensible = in.is_extensible(*trap->target);
                if (!extensible)
                    return std::nullopt;
                if (!*extensible)
                    return in.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned undefined for property '"
                        + key_description(key) + "' which exists in the non-extensible proxy target");
                return std::optional<PropertyDescriptor> {};
            }
            std::optional<bool> const extensible = in.is_extensible(*trap->target);
            if (!extensible)
                return std::nullopt;
            std::optional<PropertyDescriptor> descriptor = to_property_descriptor(in, *result);
            if (!descriptor)
                return std::nullopt;
            complete_property_descriptor(*descriptor);
            root_descriptor(in, *descriptor);
            if (!is_compatible_property_descriptor(*extensible, *descriptor, *target_desc))
                return in.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap returned descriptor for property '"
                    + key_description(key) + "' that is incompatible with the existing property in the proxy target");
            if (!descriptor->configurable.value_or(false)) {
                if (!is_non_configurable(*target_desc))
                    return in.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap reported non-configurability for property '"
                        + key_description(key) + "' which is either non-existent or configurable in the proxy target");
                if (descriptor->writable.has_value() && !*descriptor->writable && (*target_desc)->writable.value_or(false))
                    return in.throw_type_error("'getOwnPropertyDescriptor' on proxy: trap reported non-configurable and non-writable for property '"
                        + key_description(key) + "' which is writable in the proxy target");
            }
            answer = descriptor;
        }
    }
    // The descriptor's own cells outlive the scope that made them only
    // because they are rooted again here, in the caller's.
    if (answer->has_value())
        root_descriptor(in, **answer);
    return answer;
}

std::optional<bool> ProxyObject::define_own_property(Interpreter& in, PropertyKey const& key, PropertyDescriptor const& desc)
{
    // §10.5.6: a trap that claims to have defined the property is held to
    // what the target allows — nothing new on a non-extensible target,
    // nothing non-configurable that the target does not itself hold that
    // way, and no freezing of a property the target keeps writable.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    if (key.is_symbol())
        in.root(Value::symbol(key.as_symbol()));
    root_descriptor(in, desc);
    std::optional<Trap> const trap = find_trap(in, *this, "defineProperty");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.define_own_property(*trap->target, key, desc);
    Object* const described = from_property_descriptor(in, desc);
    in.root(Value::object(described));
    Value const arguments[3] = { Value::object(trap->target), key_to_value(in, key), Value::object(described) };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    if (!Interpreter::to_boolean(*result))
        return false;
    std::optional<std::optional<PropertyDescriptor>> const target_desc = in.get_own_property(*trap->target, key);
    if (!target_desc)
        return std::nullopt;
    if (target_desc->has_value())
        root_descriptor(in, **target_desc);
    std::optional<bool> const extensible = in.is_extensible(*trap->target);
    if (!extensible)
        return std::nullopt;
    bool const setting_config_false = desc.configurable.has_value() && !*desc.configurable;
    if (!target_desc->has_value()) {
        if (!*extensible)
            return in.throw_type_error("'defineProperty' on proxy: trap returned truish for adding property '"
                + key_description(key) + "' to the non-extensible proxy target");
        if (setting_config_false)
            return in.throw_type_error("'defineProperty' on proxy: trap returned truish for defining non-configurable property '"
                + key_description(key) + "' which is either non-existent or configurable in the proxy target");
        return true;
    }
    PropertyDescriptor const& current = **target_desc;
    if (!is_compatible_property_descriptor(*extensible, desc, *target_desc))
        return in.throw_type_error("'defineProperty' on proxy: trap returned truish for adding property '"
            + key_description(key) + "' that is incompatible with the existing property in the proxy target");
    if (setting_config_false && current.configurable.value_or(false))
        return in.throw_type_error("'defineProperty' on proxy: trap returned truish for defining non-configurable property '"
            + key_description(key) + "' which is either non-existent or configurable in the proxy target");
    if (current.is_data() && !current.configurable.value_or(false) && current.writable.value_or(false)
        && desc.writable.has_value() && !*desc.writable) {
        return in.throw_type_error("'defineProperty' on proxy: trap returned truish for defining non-configurable, non-writable property '"
            + key_description(key) + "' which is non-configurable and writable in the proxy target");
    }
    return true;
}

std::optional<bool> ProxyObject::has_property(Interpreter& in, PropertyKey const& key)
{
    // §10.5.7: a "no" is checked, a "yes" is not — the target may not be
    // made to disown a property it cannot drop, nor one it holds while
    // non-extensible.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    if (key.is_symbol())
        in.root(Value::symbol(key.as_symbol()));
    std::optional<Trap> const trap = find_trap(in, *this, "has");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.has_property(*trap->target, key);
    Value const arguments[2] = { Value::object(trap->target), key_to_value(in, key) };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    bool const answer = Interpreter::to_boolean(*result);
    if (answer)
        return true;
    std::optional<std::optional<PropertyDescriptor>> const target_desc = in.get_own_property(*trap->target, key);
    if (!target_desc)
        return std::nullopt;
    if (!target_desc->has_value())
        return false;
    if (is_non_configurable(*target_desc))
        return in.throw_type_error("'has' on proxy: trap returned falsish for property '" + key_description(key)
            + "' which exists in the proxy target as non-configurable");
    std::optional<bool> const extensible = in.is_extensible(*trap->target);
    if (!extensible)
        return std::nullopt;
    if (!*extensible)
        return in.throw_type_error("'has' on proxy: trap returned falsish for property '" + key_description(key)
            + "' but the proxy target is not extensible");
    return false;
}

std::optional<Value> ProxyObject::get(Interpreter& in, PropertyKey const& key, Value const& receiver)
{
    // §10.5.8: a read-only, non-configurable data property on the target
    // fixes the value the trap may report, and a non-configurable accessor
    // with no getter fixes it at undefined.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    in.root(receiver);
    if (key.is_symbol())
        in.root(Value::symbol(key.as_symbol()));
    std::optional<Trap> const trap = find_trap(in, *this, "get");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return trap->target->get(in, key, receiver);
    Value const arguments[3] = { Value::object(trap->target), key_to_value(in, key), receiver };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    in.root(*result);
    std::optional<std::optional<PropertyDescriptor>> const target_desc = in.get_own_property(*trap->target, key);
    if (!target_desc)
        return std::nullopt;
    if (is_non_configurable(*target_desc)) {
        PropertyDescriptor const& current = **target_desc;
        if (current.is_data() && !current.writable.value_or(false)) {
            if (!Interpreter::same_value(*result, current.value.value_or(Value::undefined())))
                return in.throw_type_error("'get' on proxy: property '" + key_description(key)
                    + "' is a read-only and non-configurable data property on the proxy target but the proxy did not return its actual value");
        } else if (current.is_accessor() && current.get.value_or(nullptr) == nullptr) {
            if (!result->is_undefined())
                return in.throw_type_error("'get' on proxy: property '" + key_description(key)
                    + "' is a non-configurable accessor property on the proxy target and does not have a getter function");
        }
    }
    return *result;
}

std::optional<bool> ProxyObject::set(Interpreter& in, PropertyKey const& key, Value const& value, Value const& receiver)
{
    // §10.5.9: a claim that the write landed is held to what the target
    // committed to — a read-only data property's value, and a
    // non-configurable accessor's missing setter.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    in.root(value);
    in.root(receiver);
    if (key.is_symbol())
        in.root(Value::symbol(key.as_symbol()));
    std::optional<Trap> const trap = find_trap(in, *this, "set");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return trap->target->set(in, key, value, receiver);
    Value const arguments[4] = { Value::object(trap->target), key_to_value(in, key), value, receiver };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    if (!Interpreter::to_boolean(*result))
        return false;
    std::optional<std::optional<PropertyDescriptor>> const target_desc = in.get_own_property(*trap->target, key);
    if (!target_desc)
        return std::nullopt;
    if (is_non_configurable(*target_desc)) {
        PropertyDescriptor const& current = **target_desc;
        if (current.is_data() && !current.writable.value_or(false)) {
            if (!Interpreter::same_value(value, current.value.value_or(Value::undefined())))
                return in.throw_type_error("'set' on proxy: trap returned truish for property '" + key_description(key)
                    + "' which exists in the proxy target as a non-configurable and non-writable data property with a different value");
        } else if (current.is_accessor() && current.set.value_or(nullptr) == nullptr) {
            return in.throw_type_error("'set' on proxy: trap returned truish for property '" + key_description(key)
                + "' which exists in the proxy target as a non-configurable and non-writable accessor property without a setter");
        }
    }
    return true;
}

std::optional<bool> ProxyObject::delete_property(Interpreter& in, PropertyKey const& key)
{
    // §10.5.10: a property the target cannot drop may not be reported
    // deleted, and nor may one of a non-extensible target.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    if (key.is_symbol())
        in.root(Value::symbol(key.as_symbol()));
    std::optional<Trap> const trap = find_trap(in, *this, "deleteProperty");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.delete_property(*trap->target, key);
    Value const arguments[2] = { Value::object(trap->target), key_to_value(in, key) };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
    if (!result)
        return std::nullopt;
    if (!Interpreter::to_boolean(*result))
        return false;
    std::optional<std::optional<PropertyDescriptor>> const target_desc = in.get_own_property(*trap->target, key);
    if (!target_desc)
        return std::nullopt;
    if (!target_desc->has_value())
        return true;
    if (is_non_configurable(*target_desc))
        return in.throw_type_error("'deleteProperty' on proxy: trap returned truish for property '" + key_description(key)
            + "' which is non-configurable in the proxy target");
    std::optional<bool> const extensible = in.is_extensible(*trap->target);
    if (!extensible)
        return std::nullopt;
    if (!*extensible)
        return in.throw_type_error("'deleteProperty' on proxy: trap returned truish for property '" + key_description(key)
            + "' but the proxy target is non-extensible");
    return true;
}

std::optional<std::vector<PropertyKey>> ProxyObject::own_keys(Interpreter& in)
{
    // §10.5.11: the list must be string and symbol keys, free of
    // duplicates, and must name every key the target cannot drop. On a
    // non-extensible target it must name exactly the target's own keys —
    // no fewer, and none invented.
    std::optional<std::vector<PropertyKey>> answer;
    {
        Interpreter::Roots const roots(in);
        in.root(Value::object(this));
        std::optional<Trap> const trap = find_trap(in, *this, "ownKeys");
        if (!trap)
            return std::nullopt;
        if (!trap->present()) {
            answer = in.own_keys(*trap->target);
            if (!answer)
                return std::nullopt;
        } else {
            Value const arguments[1] = { Value::object(trap->target) };
            std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), arguments);
            if (!result)
                return std::nullopt;
            in.root(*result);
            std::optional<std::vector<Value>> const list = in.create_list_from_array_like(*result);
            if (!list)
                return std::nullopt;
            std::vector<PropertyKey> keys;
            std::unordered_set<PropertyKey, PropertyKeyHash> claimed;
            keys.reserve(list->size());
            for (Value const& element : *list) {
                if (!element.is_string() && !element.is_symbol())
                    return in.throw_type_error("'ownKeys' on proxy: trap result must be an array with only string and symbol elements");
                in.root(element);
                PropertyKey const key = element.is_symbol() ? PropertyKey::symbol(element.as_symbol())
                                                            : in.heap().key(element.as_string());
                if (key.is_symbol())
                    in.root(Value::symbol(key.as_symbol()));
                if (!claimed.insert(key).second)
                    return in.throw_type_error("'ownKeys' on proxy: trap returned duplicate entries");
                keys.push_back(key);
            }
            std::optional<bool> const extensible = in.is_extensible(*trap->target);
            if (!extensible)
                return std::nullopt;
            std::optional<std::vector<PropertyKey>> const target_keys = in.own_keys(*trap->target);
            if (!target_keys)
                return std::nullopt;
            std::vector<PropertyKey> configurable;
            std::vector<PropertyKey> non_configurable;
            for (PropertyKey const& target_key : *target_keys) {
                std::optional<std::optional<PropertyDescriptor>> const desc = in.get_own_property(*trap->target, target_key);
                if (!desc)
                    return std::nullopt;
                if (is_non_configurable(*desc))
                    non_configurable.push_back(target_key);
                else
                    configurable.push_back(target_key);
            }
            if (*extensible && non_configurable.empty()) {
                answer = std::move(keys);
            } else {
                for (PropertyKey const& target_key : non_configurable) {
                    if (claimed.erase(target_key) == 0)
                        return in.throw_type_error("'ownKeys' on proxy: trap result did not include '" + key_description(target_key) + "'");
                }
                if (!*extensible) {
                    for (PropertyKey const& target_key : configurable) {
                        if (claimed.erase(target_key) == 0)
                            return in.throw_type_error("'ownKeys' on proxy: trap result did not include '" + key_description(target_key) + "'");
                    }
                    if (!claimed.empty())
                        return in.throw_type_error("'ownKeys' on proxy: trap returned extra keys but proxy target is non-extensible");
                }
                answer = std::move(keys);
            }
        }
    }
    // A symbol a trap invented is referenced by nothing but this list.
    root_keys(in, *answer);
    return answer;
}

std::optional<Value> ProxyObject::call(Interpreter& in, Value const& this_value, std::span<Value const> arguments)
{
    // §10.5.12: the trap takes the target, the `this` and the arguments as
    // one array. [[Call]] exists at all only because the target was
    // callable when the proxy was made.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    in.root(this_value);
    for (Value const& argument : arguments)
        in.root(argument);
    std::optional<Trap> const trap = find_trap(in, *this, "apply");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.call(Value::object(trap->target), this_value, arguments);
    ArrayObject* const list = in.new_array(arguments);
    in.root(Value::object(list));
    Value const trap_arguments[3] = { Value::object(trap->target), this_value, Value::object(list) };
    return in.call(trap->function, Value::object(trap->handler), trap_arguments);
}

std::optional<Value> ProxyObject::construct(Interpreter& in, std::span<Value const> arguments, Object* new_target)
{
    // §10.5.13: the trap must answer an object — `new` has no other
    // outcome to offer its caller.
    Interpreter::Roots const roots(in);
    in.root(Value::object(this));
    if (new_target != nullptr)
        in.root(Value::object(new_target));
    for (Value const& argument : arguments)
        in.root(argument);
    std::optional<Trap> const trap = find_trap(in, *this, "construct");
    if (!trap)
        return std::nullopt;
    if (!trap->present())
        return in.construct(Value::object(trap->target), arguments, new_target);
    ArrayObject* const list = in.new_array(arguments);
    in.root(Value::object(list));
    Value const trap_arguments[3] = { Value::object(trap->target), Value::object(list),
        new_target != nullptr ? Value::object(new_target) : Value::undefined() };
    std::optional<Value> const result = in.call(trap->function, Value::object(trap->handler), trap_arguments);
    if (!result)
        return std::nullopt;
    if (!result->is_object())
        return in.throw_type_error("'construct' on proxy: trap returned non-object");
    return *result;
}

// ------------------------------------------------ the non-throwing twins
//
// No trap can run without the interpreter, so each of these answers what
// the TARGET answers — which is what the method above it answers for a
// handler with no such trap. Every script-visible use of these internal
// methods goes through Interpreter's wrappers, which route a proxy to the
// throwing method instead; these keep the engine's internal uses sane.

std::optional<PropertyDescriptor> ProxyObject::get_own_property(PropertyKey const& key) const
{
    if (m_target == nullptr)
        return std::nullopt;
    return m_target->get_own_property(key);
}

bool ProxyObject::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    if (m_target == nullptr)
        return false;
    return m_target->define_own_property(key, desc);
}

bool ProxyObject::has_property(PropertyKey const& key) const
{
    if (m_target == nullptr)
        return false;
    return m_target->has_property(key);
}

bool ProxyObject::delete_property(PropertyKey const& key)
{
    if (m_target == nullptr)
        return false;
    return m_target->delete_property(key);
}

std::vector<PropertyKey> ProxyObject::own_keys() const
{
    if (m_target == nullptr)
        return {};
    return m_target->own_keys();
}

void ProxyObject::trace(Tracer& tracer)
{
    Function::trace(tracer);
    tracer.visit(m_target);
    tracer.visit(m_handler);
}

// ------------------------------------------------------------- the realm

void install_proxy(Interpreter& in)
{
    Heap::NoCollect const guard(in.heap());
    Intrinsics& i = in.intrinsics();
    // §28.2: Proxy is a constructor with no `prototype` property and no
    // @@toStringTag — a proxy's prototype is whatever its trap answers, so
    // there is nothing for a %Proxy.prototype% to be.
    NativeFunction* constructor = in.new_native(
        "Proxy", 2,
        [](Interpreter& interp, Value const&, Args) -> std::optional<Value> {
            return interp.throw_type_error("Constructor Proxy requires 'new'");
        },
        [](Interpreter& interp, Args args, Object*) -> std::optional<Value> {
            // §28.2.1.1: new.target is only checked for being present,
            // which the call entry point above reports; ProxyCreate makes
            // the same object however Proxy was subclassed.
            std::optional<ProxyObject*> const proxy = proxy_create(interp, argument(args, 0), argument(args, 1));
            if (!proxy)
                return std::nullopt;
            return Value::object(*proxy);
        });
    in.global()->put(in.key("Proxy"), Value::object(constructor), builtin_attributes);
    i.proxy_constructor = constructor;

    define_method(in, *constructor, "revocable", 2, [](Interpreter& interp, Value const&, Args args) -> std::optional<Value> {
        // §28.2.2.1 with §10.5.15: the revoker holds the proxy in a traced
        // slot and lets it go the first time it runs, so a second call does
        // nothing and the proxy is not kept alive by its own revoker.
        Interpreter::Roots const roots(interp);
        std::optional<ProxyObject*> const proxy = proxy_create(interp, argument(args, 0), argument(args, 1));
        if (!proxy)
            return std::nullopt;
        interp.root(Value::object(*proxy));
        std::vector<Value> slots { Value::object(*proxy) };
        ClosureFunction* const revoke = interp.new_closure("", 0, std::move(slots),
            [](Interpreter&, ClosureFunction& self, Value const&, Args) -> std::optional<Value> {
                Value const held = self.slot(0);
                if (held.is_object()) {
                    static_cast<ProxyObject*>(held.as_object())->revoke();
                    self.set_slot(0, Value::null());
                }
                return Value::undefined();
            });
        interp.root(Value::object(revoke));
        Object* const result = interp.new_object();
        interp.root(Value::object(result));
        result->put(interp.key("proxy"), Value::object(*proxy));
        result->put(interp.key("revoke"), Value::object(revoke));
        return Value::object(result);
    });
}

}
