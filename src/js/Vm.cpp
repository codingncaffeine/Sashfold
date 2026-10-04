#include "js/Vm.h"

// The run loop over a Frame, and the drivers that start and resume the
// bodies that need one: a generator's, resumed by next/return/throw; an
// async function's, resumed by the reaction jobs of the promise it
// awaits. Every instruction calls the evaluator's mechanisms
// (Interpreter::Impl): the machine is the one tier, since the tree-walking
// interpreter it grew beside was proved to agree with it and deleted.
//
// Rooting: an instruction's inputs stay on the frame's operand stack —
// traced through the frame stack — until the operation has finished; only then
// are they popped and the result pushed. A value that must be held in a
// local across an allocation is rooted explicitly.

#include "js/Compiler.h"
#include "js/Evaluator.h"
#include "js/Runtime.h"
#include "js/Strings.h"
#include "platform/Memory.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::js {

using Args = std::span<Value const>;

void ClassBuilder::trace(Tracer& tracer)
{
    tracer.visit(class_env);
    tracer.visit(outer_private);
    tracer.visit(private_env);
    if (has_name_key)
        tracer.visit(name_key);
    tracer.visit(proto);
    tracer.visit(constructor);
    for (StaticElement const& item : statics)
        tracer.visit(item.key);
}

namespace {

void trace_references(Tracer& tracer, std::vector<Reference> const& refs)
{
    for (Reference const& reference : refs) {
        tracer.visit(reference.environment);
        tracer.visit(reference.name);
        tracer.visit(reference.base);
        tracer.visit(reference.key);
        tracer.visit(reference.key_value);
        tracer.visit(reference.this_value);
    }
}

}

void FrameState::trace_state(Tracer& tracer) const
{
    tracer.visit(field_key);
    tracer.visit(variable);
    tracer.visit(function);
    tracer.visit(private_environment);
    tracer.visit(this_value);
    tracer.visit(new_target);
    tracer.visit(function_env);
    tracer.visit(result);
    tracer.visit(resume_value);
}

void Frame::trace(Tracer& tracer) const
{
    trace_state(tracer);
    for (std::size_t i = 0; i < registers.size(); ++i)
        tracer.visit(registers[i]);
    // The operand stack to its depth, which the frame keeps current: the
    // slots above it are not read before they are pushed to again.
    for (std::size_t i = 0; i < stack.size(); ++i)
        tracer.visit(stack[i]);
    for (std::size_t i = 0; i < envs.size(); ++i)
        tracer.visit(envs[i]);
    trace_references(tracer, refs);
    for (ClassBuilder* builder : builders)
        tracer.visit(builder);
}

void SavedFrame::trace(Tracer& tracer)
{
    state.trace_state(tracer);
    for (Value const& value : registers)
        tracer.visit(value);
    for (Value const& value : stack)
        tracer.visit(value);
    for (Environment* env : envs)
        tracer.visit(env);
    trace_references(tracer, refs);
    for (ClassBuilder* builder : builders)
        tracer.visit(builder);
}

void OperandStack::overflow()
{
    // The compiler counts every frame's deepest operand stack, and a frame
    // is pushed only when that much room is free: a push past the end of
    // the whole value stack is the compiler's miscount, not the page's.
    std::fprintf(stderr, "internal: the machine's value stack overflowed its end\n");
    std::abort();
}

namespace {

// A key that went through ToPropertyKey and back onto the stack: a symbol,
// or a string whose canonical form Heap::key tells apart from an index.
PropertyKey key_from_value(Heap& heap, Value const& value)
{
    if (value.is_symbol())
        return PropertyKey::symbol(value.as_symbol());
    return heap.key(value.as_string());
}

Value key_to_value(Heap& heap, PropertyKey const& key)
{
    if (key.is_symbol())
        return Value::symbol(key.as_symbol());
    return Value::string(heap.key_to_string(key));
}

// The elements of an argument array built by the compiler for a spread
// call: dense, no holes.
std::vector<Value> array_arguments(Value const& array_value)
{
    auto* array = static_cast<ArrayObject*>(array_value.as_object());
    std::vector<Value> arguments;
    arguments.reserve(array->length());
    for (std::uint32_t i = 0; i < array->length(); ++i) {
        Value const element = array->element(i);
        arguments.push_back(element.is_empty() ? Value::undefined() : element);
    }
    return arguments;
}

} // namespace

// ---- compiling and framing ------------------------------------------------

CodeBlock const* Interpreter::Impl::compiled_body(FunctionNode const& node)
{
    if (auto const found = code_blocks.find(&node); found != code_blocks.end())
        return found->second.get();
    std::string error;
    auto const compile_started = std::chrono::steady_clock::now();
    std::unique_ptr<CodeBlock> code = compile_function_body(node, heap(), &error);
    self.note_compiled(compile_started);
    if (!code) {
        self.throw_syntax_error(error);
        return nullptr;
    }
    CodeBlock const* raw = code.get();
    code_blocks.emplace(&node, std::move(code));
    return raw;
}

// A plain body on the machine: compiled at its first call, run on a frame
// over the call's environments until it returns or throws. It cannot
// yield or await — the compiler refuses those outside a generator or an
// async body — so the run ends Completed with the return value in the
// frame's result, or Threw with the exception pending.
std::optional<Value> Interpreter::Impl::run_compiled_body(ScriptFunction& function, Context const& cx, PropertyKey const* field_key)
{
    return run_compiled_node(function.node(), cx, field_key);
}

// A parameter list that is not simple — a default, a pattern, a rest —
// bound on the machine (IteratorBindingInitialization of the formals,
// §10.2.11 step 24–26): the block reads the call's arguments by index and
// initializes each parameter in the environment the prologue made for
// them, with the defaults evaluated there in order.
CodeBlock const* Interpreter::Impl::compiled_parameters(FunctionNode const& node)
{
    if (auto const found = parameter_blocks.find(&node); found != parameter_blocks.end())
        return found->second.get();
    std::string error;
    std::unique_ptr<CodeBlock> code = compile_parameter_list(node, heap(), &error);
    if (!code) {
        self.throw_syntax_error(error);
        return nullptr;
    }
    CodeBlock const* raw = code.get();
    parameter_blocks.emplace(&node, std::move(code));
    return raw;
}

// ---- the inline caches (js/Feedback.h) -------------------------------------

FeedbackVector* Interpreter::Impl::feedback_for(CodeBlock const& code)
{
    if (code.feedback == nullptr && (code.property_sites | code.call_sites | code.operand_sites | code.element_sites) != 0) {
        code.feedback = std::make_unique<FeedbackVector>(code.property_sites, code.call_sites, code.operand_sites, code.element_sites);
        feedback_vectors.push_back(code.feedback.get());
    }
    return code.feedback.get();
}

void Interpreter::Impl::clear_dead_feedback()
{
    Heap const& h = heap();
    for (FeedbackVector* vector : feedback_vectors)
        vector->clear_dead(h);
    if (stub_cache)
        stub_cache->clear();
}

void Interpreter::Impl::note_miss(PropertySite const& site, PropertyEntry const* found, JsString const* name, char const* where, bool recorded)
{
    CacheCensus& census = *cache_census;
    if (found != nullptr)
        ++census.stale;
    else if (site.state == PropertySite::Megamorphic)
        ++census.megamorphic;
    else if (site.state == PropertySite::Empty)
        ++census.first;
    else
        ++census.other_shape;
    if (!recorded)
        ++census.unrecorded;
    ++census.by_name[std::string(where) + (recorded ? " " : " unrecorded ") + name->to_utf8()];
}

namespace {

// The answer a site has for this shape, if it has one.
PropertyEntry const* entry_for(PropertySite const& site, Shape const* shape, StubCache* stubs, JsString const* name, bool store)
{
    switch (site.state) {
    case PropertySite::Monomorphic:
        return site.first.shape == shape ? &site.first : nullptr;
    case PropertySite::Polymorphic:
        for (std::uint8_t i = 0; i < site.count; ++i) {
            if (site.more[i].shape == shape)
                return &site.more[i];
        }
        return nullptr;
    case PropertySite::Megamorphic:
        return stubs != nullptr ? stubs->find(shape, name, store) : nullptr;
    default:
        return nullptr;
    }
}

// Whether an answer over the receiver's own property still holds: a shared
// shape's always; a dictionary's while the place the answer found the
// property at still holds its key, its slot and its kind (and, for a
// write, its being writable) — whatever else the dictionary did.
bool own_holds(PropertyEntry const& entry, Shape const& shape, PropertyKey const& key, bool accessor, bool for_write = false)
{
    if (!shape.is_dictionary())
        return true;
    ShapeTable const& table = *shape.table();
    if (entry.version >= table.size())
        return false;
    ShapeEntry const& there = table[entry.version];
    return there.key == key && there.slot == entry.slot && there.accessor == accessor && (!for_write || (there.attributes & Writable) != 0);
}

// Whether an answer from up the chain still holds: the prototype epoch
// stands, and a dictionary receiver has not come by the key itself (what
// it inherits does not change without the epoch moving).
bool chain_holds(PropertyEntry const& entry, Shape const& shape, Heap const& heap, PropertyKey const& key)
{
    return entry.stamp == heap.prototype_epoch() && (!shape.is_dictionary() || shape.position_of(key) == ShapeTable::npos);
}

// An answer that rests on a chain: its key goes into the heap's filter.
bool rests_on_chain(CacheKind kind)
{
    return kind == CacheKind::ProtoData || kind == CacheKind::ProtoAccessor || kind == CacheKind::Absent || kind == CacheKind::StoreAdd;
}

// An accessor's pair, as a slot holds it once made.
AccessorPair const* pair_in(Value const& held)
{
    return held.is_lazy_mark() ? nullptr : static_cast<AccessorPair const*>(held.as_cell());
}

// A call site's target: the one it has called, or that it has called many.
void note_call(CallSite& site, Value const& callee)
{
    if (site.many)
        return;
    Object* const target = callee.is_object() ? callee.as_object() : nullptr;
    if (site.target == nullptr)
        site.target = target;
    else if (site.target != target)
        site.many = true;
}

// An element site's base and key, past the dense read the loop answers.
void note_element(ElementSite& site, Value const& base, Value const& key)
{
    if (base.is_object() && key.is_number()) {
        Object const& object = *base.as_object();
        if (object.class_id() == Object::Class::Array && key.is_int32()) {
            site.kinds |= ElementDense;
            return;
        }
        if (object.class_id() == Object::Class::TypedArray) {
            auto const type = static_cast<std::uint8_t>(static_cast<TypedArrayObject const&>(object).element_type());
            site.kinds |= ElementTyped;
            site.typed = site.typed == 0xff || site.typed == type ? type : 0xfe;
            return;
        }
    }
    site.kinds |= ElementGeneric;
}

}

// The object a named read or write of `object` is answered from: the
// object itself, the window a same-origin window proxy stands for, or none
// (any other proxy, whose handler answers).
Object* Interpreter::Impl::named_access_target(Object& object)
{
    if (!object.is_proxy())
        return &object;
    return static_cast<ProxyObject&>(object).forwards_named_access(self);
}

std::optional<Value> Interpreter::Impl::get_named(PropertySite* site, Value const& base, JsString* name)
{
    Heap& h = heap();
    Object* const target = site != nullptr && base.is_object() ? named_access_target(*base.as_object()) : nullptr;
    if (target == nullptr)
        return self.get(base, h.key(name));
    Object& object = *target;
    Shape const* const shape = object.shape();
    PropertyKey const key = h.key(name);
    PropertyEntry const* const entry = entry_for(*site, shape, stub_cache.get(), name, false);
    if (entry != nullptr) {
        switch (entry->kind) {
        case CacheKind::OwnData:
            if (own_holds(*entry, *shape, key, false)) {
                if (Value const& value = object.slot_value(entry->slot); !value.is_lazy_mark()) {
                    ++ic_hits;
                    return value;
                }
            }
            break;
        case CacheKind::OwnAccessor:
            if (own_holds(*entry, *shape, key, true)) {
                if (AccessorPair const* const pair = pair_in(object.slot_value(entry->slot))) {
                    ++ic_hits;
                    if (pair->getter == nullptr)
                        return Value::undefined();
                    return self.call(Value::object(pair->getter), base, {});
                }
            }
            break;
        case CacheKind::ProtoData:
            if (chain_holds(*entry, *shape, h, key)) {
                if (Value const& value = static_cast<Object*>(entry->other)->slot_value(entry->slot); !value.is_lazy_mark()) {
                    ++ic_hits;
                    return value;
                }
            }
            break;
        case CacheKind::ProtoAccessor:
            if (chain_holds(*entry, *shape, h, key)) {
                if (AccessorPair const* const pair = pair_in(static_cast<Object*>(entry->other)->slot_value(entry->slot))) {
                    ++ic_hits;
                    if (pair->getter == nullptr)
                        return Value::undefined();
                    return self.call(Value::object(pair->getter), base, {});
                }
            }
            break;
        case CacheKind::Absent:
            if (chain_holds(*entry, *shape, h, key)) {
                ++ic_hits;
                return Value::undefined();
            }
            break;
        case CacheKind::ArrayLength:
            if (object.class_id() == Object::Class::Array) {
                ++ic_hits;
                return Value::number(static_cast<double>(static_cast<ArrayObject const&>(object).length()));
            }
            break;
        default:
            break;
        }
    }
    ++ic_misses;
    PropertyEntry const answer = answer_for_load(object, key, h.prototype_epoch());
    if (rests_on_chain(answer.kind))
        h.note_cached_key(key);
    if (cache_census) [[unlikely]]
        note_miss(*site, entry, name, "get", answer.kind != CacheKind::None);
    std::optional<Value> const value = self.get(base, key);
    if (answer.kind != CacheKind::None)
        record(*site, answer, stubs(), name, false);
    return value;
}

bool Interpreter::Impl::put_named(PropertySite* site, Value const& base, JsString* name, Value const& value, bool strict)
{
    Heap& h = heap();
    Object* const target = named_access_target(*base.as_object());
    if (target == nullptr)
        return self.set(base, h.key(name), value, strict).has_value();
    Object& object = *target;
    Shape* const shape = object.shape();
    PropertyKey const key = h.key(name);
    PropertyEntry const* const entry = entry_for(*site, shape, stub_cache.get(), name, true);
    if (entry != nullptr) {
        switch (entry->kind) {
        case CacheKind::StoreAdd:
            if (entry->stamp == h.prototype_epoch()) {
                ++ic_hits;
                object.cache_add(static_cast<Shape*>(entry->other), entry->slot, value);
                return true;
            }
            break;
        case CacheKind::OwnData:
            if (own_holds(*entry, *shape, key, false, true) && !object.slot_value(entry->slot).is_lazy_mark()) {
                ++ic_hits;
                object.cache_store(entry->slot, value);
                return true;
            }
            break;
        case CacheKind::OwnAccessor:
        case CacheKind::ProtoAccessor: {
            bool const own = entry->kind == CacheKind::OwnAccessor;
            if (!(own ? own_holds(*entry, *shape, key, true) : chain_holds(*entry, *shape, h, key)))
                break;
            Object const& holder = own ? object : *static_cast<Object*>(entry->other);
            AccessorPair const* const pair = pair_in(holder.slot_value(entry->slot));
            // No setter: the slow path says what that is (a TypeError in
            // strict code).
            if (pair == nullptr || pair->setter == nullptr)
                break;
            ++ic_hits;
            Value const arguments[1] = { value };
            return self.call(Value::object(pair->setter), base, arguments).has_value();
        }
        default:
            break;
        }
    }
    ++ic_misses;
    std::uint32_t const epoch = h.prototype_epoch();
    PropertyEntry answer = answer_for_store(object, key, epoch);
    std::optional<bool> const stored = self.set(base, key, value, strict);
    if (!stored)
        return false;
    if (answer.kind == CacheKind::None && target == base.as_object())
        answer = answer_for_add(object, shape, key, epoch);
    if (rests_on_chain(answer.kind))
        h.note_cached_key(key);
    if (cache_census) [[unlikely]]
        note_miss(*site, entry, name, "put", answer.kind != CacheKind::None);
    if (answer.kind != CacheKind::None)
        record(*site, answer, stubs(), name, true);
    return true;
}

namespace {

// A global's answer over the global object's own property still holds
// while the realm's scripts have declared no let, const or class since
// (one could stand in front of it) and the property is still at its place.
bool global_holds(PropertyEntry const& entry, Shape const& shape, RealmRecord const& realm, PropertyKey const& key, bool accessor, bool for_write = false)
{
    return entry.stamp == realm.lexical_generation && own_holds(entry, shape, key, accessor, for_write);
}

}

// A global name (Identifier::global): a script's lexical binding, or the
// global object's own property, through the site's cache; anything else —
// a property the global inherits, a name nothing binds — by the slow path.
std::optional<Value> Interpreter::Impl::get_global(PropertySite& site, JsString* name, Environment* environment, bool strict,
    bool typeof_name)
{
    RealmRecord& current = realm();
    Object& global = *current.intrinsics.global;
    Shape const* const shape = global.shape();
    PropertyKey const key = PropertyKey::atom(name);
    PropertyEntry const* const entry = entry_for(site, shape, nullptr, name, false);
    if (entry != nullptr) {
        switch (entry->kind) {
        case CacheKind::GlobalLexical:
            if (entry->other == current.global_lexical) {
                Environment::Binding const& binding = current.global_lexical->binding_at(entry->slot);
                if (binding.initialized && binding.import_module == nullptr) {
                    ++ic_hits;
                    return binding.value;
                }
            }
            break;
        case CacheKind::GlobalData:
            if (global_holds(*entry, *shape, current, key, false)) {
                if (Value const& value = global.slot_value(entry->slot); !value.is_lazy_mark()) {
                    ++ic_hits;
                    return value;
                }
            }
            break;
        case CacheKind::GlobalAccessor:
            if (global_holds(*entry, *shape, current, key, true)) {
                if (AccessorPair const* const pair = pair_in(global.slot_value(entry->slot))) {
                    ++ic_hits;
                    if (pair->getter == nullptr)
                        return Value::undefined();
                    return self.call(Value::object(pair->getter), Value::object(&global), {});
                }
            }
            break;
        default:
            break;
        }
    }
    ++ic_misses;
    Reference reference = resolve(name, environment);
    if (typeof_name && reference.kind == Reference::Kind::Unresolvable)
        return Value::empty();
    // What the name was found as, before its value is read (a getter may
    // change what the global has).
    PropertyEntry answer;
    if (reference.kind == Reference::Kind::Binding && reference.environment == current.global_lexical) {
        std::size_t const place = current.global_lexical->place_of(name);
        if (place < current.global_lexical->binding_count())
            answer = { const_cast<Shape*>(shape), current.global_lexical, static_cast<std::uint32_t>(place), 0, 0, CacheKind::GlobalLexical };
    } else if (reference.kind == Reference::Kind::ObjectEnvironment && reference.environment->object() == &global) {
        std::uint32_t const place = shape->position_of(reference.key);
        if (place != ShapeTable::npos && !global.slot_value(shape->at(place).slot).is_lazy_mark())
            answer = { const_cast<Shape*>(shape), &global, shape->at(place).slot, current.lexical_generation, place,
                shape->at(place).accessor ? CacheKind::GlobalAccessor : CacheKind::GlobalData };
    }
    if (cache_census) [[unlikely]]
        note_miss(site, entry, name, "global", answer.kind != CacheKind::None);
    std::optional<Value> const value = get_value(reference, strict);
    if (answer.kind != CacheKind::None && shape->is_dictionary())
        record(site, answer, stubs(), name, false);
    return value;
}

// A global name's reference (RefName): from the site's cache when it knows
// the name — a script's lexical binding, or the global object's own
// writable data property, which the reference then reads and writes in its
// slot while the property is still at its place — else resolved by name.
Reference Interpreter::Impl::global_reference(PropertySite& site, JsString* name, Environment* environment)
{
    RealmRecord& current = realm();
    Object& global = *current.intrinsics.global;
    Shape const* const shape = global.shape();
    PropertyKey const key = PropertyKey::atom(name);
    if (PropertyEntry const* const entry = entry_for(site, shape, nullptr, name, false)) {
        if (entry->kind == CacheKind::GlobalLexical && entry->other == current.global_lexical) {
            ++ic_hits;
            Reference reference;
            reference.name = name;
            reference.kind = Reference::Kind::Binding;
            reference.environment = current.global_lexical;
            return reference;
        }
        if (entry->kind == CacheKind::GlobalData && global_holds(*entry, *shape, current, key, false, true)) {
            ++ic_hits;
            Reference reference;
            reference.name = name;
            reference.kind = Reference::Kind::ObjectEnvironment;
            reference.environment = current.intrinsics.global_environment;
            reference.key = key;
            reference.slot = entry->slot;
            reference.cached_shape = shape;
            reference.cached_version = entry->version;
            return reference;
        }
    }
    ++ic_misses;
    Reference reference = resolve(name, environment);
    if (!shape->is_dictionary())
        return reference;
    if (reference.kind == Reference::Kind::Binding && reference.environment == current.global_lexical) {
        std::size_t const place = current.global_lexical->place_of(name);
        if (place < current.global_lexical->binding_count())
            record(site, { const_cast<Shape*>(shape), current.global_lexical, static_cast<std::uint32_t>(place), 0, 0, CacheKind::GlobalLexical }, stubs(), name, false);
    } else if (reference.kind == Reference::Kind::ObjectEnvironment && reference.environment->object() == &global
        && reference.environment == current.intrinsics.global_environment) {
        std::uint32_t const place = shape->position_of(reference.key);
        if (place != ShapeTable::npos) {
            ShapeEntry const& own = shape->at(place);
            if (!own.accessor && (own.attributes & Writable) != 0 && !global.slot_value(own.slot).is_lazy_mark()) {
                record(site, { const_cast<Shape*>(shape), &global, own.slot, current.lexical_generation, place, CacheKind::GlobalData }, stubs(), name, false);
                reference.slot = own.slot;
                reference.cached_shape = shape;
                reference.cached_version = place;
            }
        }
    }
    return reference;
}

// Whether a global reference's cached place still holds its property, a
// writable data one, as it did when the name was resolved.
bool Interpreter::Impl::cached_reference_holds(Reference const& reference) const
{
    Object const& global = *reference.environment->object();
    if (global.shape() != reference.cached_shape)
        return false;
    ShapeTable const& table = *reference.cached_shape->table();
    if (reference.cached_version >= table.size())
        return false;
    ShapeEntry const& there = table[reference.cached_version];
    return there.key == reference.key && there.slot == reference.slot && !there.accessor && (there.attributes & Writable) != 0;
}

bool Interpreter::Impl::run_parameter_block(FunctionNode const& node, Environment* env, std::span<Value const> arguments, Context const& cx)
{
    CodeBlock const* code = compiled_parameters(node);
    if (code == nullptr)
        return false;
    Roots const roots(self);
    Context binding_context = cx;
    binding_context.lexical = env;
    Frame* frame = push_frame(*code, binding_context);
    if (frame == nullptr)
        return false;
    frame->incoming = arguments;
    RunStatus const status = vm_run(*frame);
    pop_frame(*frame);
    return status == RunStatus::Completed;
}

// The same for a body that is no function's: a script's or an eval's
// statement list, compiled as a function body that tracks its completion
// value and returns it.
std::optional<Value> Interpreter::Impl::run_compiled_node(FunctionNode const& node, Context const& cx, PropertyKey const* field_key)
{
    CodeBlock const* code = compiled_body(node);
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    Frame* frame = push_frame(*code, cx);
    if (frame == nullptr)
        return std::nullopt;
    if (field_key != nullptr)
        frame->field_key = key_to_value(heap(), *field_key);
    RunStatus const status = vm_run(*frame);
    Value const result = frame->result.is_empty() ? Value::undefined() : frame->result;
    // Rooted by the caller's scope before the frame that held it is emptied.
    self.root(result);
    pop_frame(*frame);
    if (status == RunStatus::Threw)
        return std::nullopt;
    if (status != RunStatus::Completed)
        return self.throw_syntax_error("a plain function body suspended");
    return result;
}

// [[Call]] and [[Construct]] of a function compiled with its bindings
// resolved: PrepareForOrdinaryCall and OrdinaryCallBindThis here (`this`
// and new.target onto the frame, a base constructor's fields defined
// first), FunctionDeclarationInstantiation as the body's own first
// instructions, then the body. The caller rooted the function, `this` and
// the arguments; the frame reads the arguments where they are until its
// prologue is done.
std::optional<Value> Interpreter::Impl::run_resolved_function(ScriptFunction& function, CodeBlock const& code, Value const& this_argument,
    std::span<Value const> arguments, Object* new_target, PropertyKey const* field_key, PromiseCapability const* async_capability,
    RealmRecord* caller_realm)
{
    FunctionNode const& node = function.node();
    Roots const roots(self);
    Value this_value = Value::undefined();
    if (!node.is_arrow) {
        if (node.is_derived_constructor) {
            // A derived constructor's `this` waits for super() (§10.2.1.1).
            this_value = Value::empty();
        } else {
            // OrdinaryCallBindThis: sloppy code sees its realm's global
            // `this` for a nullish `this` and a wrapper for a primitive one.
            this_value = this_argument;
            if (!node.is_strict) {
                if (this_value.is_nullish()) {
                    this_value = Value::object(self.global_this());
                } else if (!this_value.is_object()) {
                    std::optional<Object*> const boxed = self.to_object(this_value);
                    if (!boxed)
                        return std::nullopt;
                    this_value = Value::object(*boxed);
                    self.root(this_value);
                }
            }
        }
        // A base class constructor defines its fields on the fresh
        // instance before anything else runs (§10.2.2 step 6.b).
        if (node.is_class_constructor && !node.is_derived_constructor && new_target != nullptr && this_argument.is_object()) {
            if (!initialize_instance_elements(*this_argument.as_object(), function))
                return std::nullopt;
        }
    }

    Context const cx { function.scope(), function.scope(), node.program, &function, node.is_strict, function.private_environment() };
    // Every body starts on the stacks; one that suspends is copied out when
    // it does (start_generator and the rest take the frame from here).
    Frame* frame = push_frame(code, cx);
    if (frame == nullptr)
        return std::nullopt;
    frame->incoming = arguments;
    frame->this_value = this_value;
    frame->new_target = new_target;
    if (field_key != nullptr)
        frame->field_key = key_to_value(heap(), *field_key);
    // A generator's prologue runs now and its body at the first next()
    // (§15.5.2, §15.6.2 for the async kind); an async function's body runs
    // to its first await (§15.8.4).
    if (node.is_generator)
        return node.is_async ? start_async_generator(function, cx, frame) : start_generator(function, cx, frame);
    if (async_capability != nullptr)
        return start_async(node, cx, *async_capability, frame);

    RunStatus const status = vm_run(*frame);
    Value const result = frame->result.is_empty() ? Value::undefined() : frame->result;
    self.root(result);
    // The `this` super() bound, read before the frame is given back.
    Value bound_this = frame->this_value;
    if (frame->function_env != nullptr)
        bound_this = frame->function_env->this_initialized() ? frame->function_env->this_value() : Value::empty();
    self.root(bound_this);
    pop_frame(*frame);
    if (status == RunStatus::Threw)
        return std::nullopt;
    if (status != RunStatus::Completed)
        return self.throw_syntax_error("a plain function body suspended");
    // [[Construct]] of a derived class (§10.2.2 steps 9–12), back in the
    // caller's context: an object returned is the result, anything else but
    // undefined a TypeError, and undefined yields the `this` that super()
    // bound — each error made in the caller's realm.
    if (new_target != nullptr && node.is_derived_constructor) {
        if (result.is_object())
            return result;
        RealmScope const caller(self, caller_realm);
        if (!result.is_undefined())
            return self.throw_type_error("Derived constructors may only return object or undefined");
        if (bound_this.is_empty())
            return self.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
        return bound_this;
    }
    return result;
}

// The stacks' sizes: values for a recursion as deep as the call-depth limit
// allows at a few dozen values a frame, environments a few to a frame. The
// blocks come zeroed from the OS (all-zero bits are the empty value), so
// their pages cost nothing until a frame reaches them.
namespace {
constexpr std::size_t value_stack_slots = std::size_t { 1 } << 21; // 16 MB of address space
constexpr std::size_t env_stack_slots = std::size_t { 1 } << 20; // 8 MB
}

Interpreter::Impl::VmStacks::~VmStacks()
{
    for (Frame* frame : frames)
        delete frame;
    platform::release_zeroed(values, value_stack_slots * sizeof(Value));
    platform::release_zeroed(static_cast<void*>(envs), env_stack_slots * sizeof(Environment*));
}

Frame* Interpreter::Impl::push_frame(CodeBlock const& code, Context const& cx)
{
    VmStacks& stacks = vm_stacks;
    if (stacks.values == nullptr) {
        stacks.values = static_cast<Value*>(platform::reserve_zeroed(value_stack_slots * sizeof(Value)));
        stacks.envs = static_cast<Environment**>(platform::reserve_zeroed(env_stack_slots * sizeof(Environment*)));
        if (stacks.values == nullptr || stacks.envs == nullptr) {
            std::fprintf(stderr, "internal: the machine's stacks could not be allocated\n");
            std::abort();
        }
        stacks.value_capacity = value_stack_slots;
        stacks.env_capacity = env_stack_slots;
    }
    // Where the new frame begins: above the top frame's extent (its
    // registers and operand area, or more if it pushed past the area) and
    // its environments.
    Value* value_base = stacks.values;
    Environment** env_base = stacks.envs;
    if (stacks.depth > 0) {
        Frame& below = *stacks.frames[stacks.depth - 1];
        value_base = below.stack.data() + below.stack.extent();
        env_base = below.envs.data() + below.envs.size();
    }
    Value const* const value_end = stacks.values + stacks.value_capacity;
    Environment* const* const env_end = stacks.envs + stacks.env_capacity;
    // Its registers, the operand area the compiler counted (and one more
    // for the value a resumed body is handed) and its first environment.
    std::size_t const operand_area = std::size_t { code.max_stack } + 1;
    std::size_t const slots = std::size_t { code.register_count } + operand_area;
    if (static_cast<std::size_t>(value_end - value_base) < slots || env_base >= env_end) {
        self.throw_range_error("Maximum call stack size exceeded");
        return nullptr;
    }
    if (stacks.depth == stacks.frames.size())
        stacks.frames.push_back(new Frame());
    Frame& frame = *stacks.frames[stacks.depth];
    ++stacks.depth;
    // Every field of the state, once: the slot holds whatever its last call
    // left, which nothing traced since it was popped.
    frame.code = &code;
    frame.pc = 0;
    frame.incoming = {};
    frame.field_key = Value();
    frame.variable = cx.variable;
    frame.function = cx.function;
    frame.program = cx.program;
    frame.private_environment = cx.private_environment;
    frame.strict = cx.strict;
    frame.this_value = Value();
    frame.new_target = nullptr;
    frame.function_env = nullptr;
    frame.result = Value::empty();
    frame.resume_kind = ResumeKind::Normal;
    frame.resume_value = Value();
    frame.resume_pending = false;
    frame.result_is_iter_result = false;
    // The registers start undefined; the operand area is read only where it
    // has been pushed to, and traced to its depth.
    frame.registers.reset(value_base, code.register_count);
    for (std::size_t i = 0; i < code.register_count; ++i)
        value_base[i] = Value::undefined();
    frame.stack.reset(value_base + code.register_count, static_cast<std::uint32_t>(operand_area), value_end);
    frame.envs.reset(env_base, env_end);
    frame.envs.push_back(cx.lexical);
    frame.refs.clear();
    frame.builders.clear();
    frame.inlined = false;
    return &frame;
}

void Interpreter::Impl::pop_frame(Frame& frame)
{
    VmStacks& stacks = vm_stacks;
    if (stacks.depth == 0 || stacks.frames[stacks.depth - 1] != &frame) {
        std::fprintf(stderr, "internal: a frame popped that is not the machine's top one\n");
        std::abort();
    }
    // The slot keeps what the call left: nothing traces a frame above the
    // depth, and the next push writes every field (and empties the vectors,
    // whose room it keeps).
    --stacks.depth;
}

SavedFrame* Interpreter::Impl::save_and_pop(Frame& frame, SavedFrame* into)
{
    SavedFrame* saved = into;
    if (saved == nullptr) {
        Heap::NoCollect const guard(heap());
        saved = heap().allocate<SavedFrame>();
    }
    std::size_t const before = saved->size_in_bytes();
    // The caller's arguments are not kept past the call (a suspended body
    // has run its prologue, which is all that reads them).
    saved->state = frame;
    saved->state.incoming = {};
    saved->registers.assign(frame.registers.data(), frame.registers.data() + frame.registers.size());
    saved->stack.assign(frame.stack.data(), frame.stack.data() + frame.stack.size());
    saved->envs.assign(frame.envs.data(), frame.envs.data() + frame.envs.size());
    saved->refs = frame.refs;
    saved->builders = frame.builders;
    pop_frame(frame);
    // What the copy grew by, told to the heap as every growing cell tells it.
    if (std::size_t const after = saved->size_in_bytes(); after > before)
        heap().grew(after - before);
    return saved;
}

Frame* Interpreter::Impl::restore_frame(SavedFrame& saved)
{
    CodeBlock const& code = *saved.state.code;
    Context const cx { saved.envs.empty() ? nullptr : saved.envs.front(), saved.state.variable, saved.state.program,
        saved.state.function, saved.state.strict, saved.state.private_environment };
    Frame* frame = push_frame(code, cx);
    if (frame == nullptr)
        return nullptr;
    static_cast<FrameState&>(*frame) = saved.state;
    for (std::size_t i = 0; i < saved.registers.size() && i < frame->registers.size(); ++i)
        frame->registers[i] = saved.registers[i];
    for (Value const& value : saved.stack)
        frame->stack.push_back(value);
    frame->envs.clear();
    for (Environment* env : saved.envs) {
        if (!frame->envs.has_room()) {
            pop_frame(*frame);
            self.throw_range_error("Maximum call stack size exceeded");
            return nullptr;
        }
        frame->envs.push_back(env);
    }
    frame->refs = saved.refs;
    frame->builders = saved.builders;
    return frame;
}

SavedFrame* Interpreter::Impl::fresh_saved_frame(CodeBlock const& code, Context const& cx)
{
    Heap::NoCollect const guard(heap());
    auto* saved = heap().allocate<SavedFrame>();
    saved->state.code = &code;
    saved->state.variable = cx.variable;
    saved->state.function = cx.function;
    saved->state.program = cx.program;
    saved->state.private_environment = cx.private_environment;
    saved->state.strict = cx.strict;
    saved->registers.assign(code.register_count, Value::undefined());
    saved->envs.push_back(cx.lexical);
    return saved;
}

Frame* Interpreter::Impl::enter_call(Value const& callee, Value const& this_argument, std::span<Value const> arguments, bool& threw)
{
    threw = false;
    if (!callee.is_object())
        return nullptr;
    Object* const object = callee.as_object();
    if (object->class_id() != Object::Class::Function || !static_cast<Function*>(object)->is_script())
        return nullptr;
    auto& function = *static_cast<ScriptFunction*>(object);
    FunctionNode const& node = function.node();
    // What the loop leaves to the generic path: a class constructor (its
    // TypeError), a generator or async body (their drivers), a body that
    // finds its names by name (a with, a direct eval), a default
    // constructor.
    if (node.is_class_constructor || node.is_generator || node.is_async || node.scope == nullptr || node.dynamic
        || node.is_default_constructor)
        return nullptr;
    // Interpreter::call's limits: the depth, and the interrupt's poll.
    if (self.m_call_depth >= self.m_call_depth_limit) {
        self.throw_range_error("Maximum call stack size exceeded");
        threw = true;
        return nullptr;
    }
    if (!tick()) {
        threw = true;
        return nullptr;
    }
    CodeBlock const* code = function.compiled();
    if (code == nullptr) {
        code = compiled_body(node);
        if (code == nullptr) {
            threw = true;
            return nullptr;
        }
        function.set_compiled(code);
    }
    // PrepareForOrdinaryCall (§10.2.1.1): the callee's realm is current, for
    // its script too, until the call ends.
    RealmRecord* const caller_realm = self.m_realm;
    RealmRecord* const caller_script_realm = self.m_script_realm;
    if (RealmRecord* const realm = function.realm())
        self.m_realm = realm;
    self.m_script_realm = self.m_realm;
    Context const cx { function.scope(), function.scope(), node.program, &function, node.is_strict, function.private_environment() };
    Frame* frame = push_frame(*code, cx);
    if (frame == nullptr) {
        self.m_realm = caller_realm;
        self.m_script_realm = caller_script_realm;
        threw = true;
        return nullptr;
    }
    frame->inlined = true;
    frame->caller_realm = caller_realm;
    frame->caller_script_realm = caller_script_realm;
    // The arguments stay where the caller left them, on its operand stack.
    frame->incoming = arguments;
    // OrdinaryCallBindThis (§10.2.1.2): sloppy code sees its realm's global
    // `this` for a nullish one and a wrapper for a primitive; an arrow has
    // none of its own.
    if (!node.is_arrow) {
        Value this_value = this_argument;
        if (!node.is_strict) {
            if (this_value.is_nullish()) {
                this_value = Value::object(self.global_this());
            } else if (!this_value.is_object()) {
                std::optional<Object*> const boxed = self.to_object(this_value);
                if (!boxed) {
                    pop_frame(*frame);
                    self.m_realm = caller_realm;
                    self.m_script_realm = caller_script_realm;
                    threw = true;
                    return nullptr;
                }
                this_value = Value::object(*boxed);
            }
        }
        frame->this_value = this_value;
    }
    ++self.m_call_depth;
    return frame;
}

Frame* Interpreter::Impl::leave_call(Frame& frame)
{
    --self.m_call_depth;
    self.m_realm = frame.caller_realm;
    self.m_script_realm = frame.caller_script_realm;
    pop_frame(frame);
    return vm_stacks.frames[vm_stacks.depth - 1];
}

bool Interpreter::Impl::put_member_named_slow(Frame& frame, Value const& base, JsString* name, Value const& value)
{
    // PutValue of a reference to base.name: a primitive base, or the
    // TypeError of a nullish one.
    Reference reference;
    reference.kind = Reference::Kind::Property;
    reference.base = base;
    reference.key = heap().key(name);
    reference.key_ready = true;
    return put_value(reference, value, frame_context(frame));
}

bool Interpreter::Impl::put_member_slow(Frame& frame, Value const& base, Value const& key, Value const& value)
{
    // As a reference to base[key] is put: the key converted at once unless
    // it is an object, which waits for the base's check.
    Reference reference;
    reference.kind = Reference::Kind::Property;
    reference.base = base;
    reference.key_value = key;
    if (!key.is_object()) {
        std::optional<PropertyKey> const converted = self.to_property_key(key);
        if (!converted)
            return false;
        reference.key = *converted;
        reference.key_ready = true;
    }
    return put_value(reference, value, frame_context(frame));
}

bool Interpreter::Impl::get_member_slow(Frame& frame)
{
    // GetValue of base[key] with an object key: the base checked first
    // (a nullish one's TypeError describes the key unconverted), then the
    // key made a property key, then [[Get]]; [base, key] to [value].
    Reference reference;
    reference.kind = Reference::Kind::Property;
    reference.base = frame.peek(1);
    reference.key_value = frame.peek(0);
    std::optional<Value> const value = get_value(reference, frame.strict);
    if (!value)
        return false;
    frame.stack.pop_back();
    frame.top() = *value;
    return true;
}

bool Interpreter::Impl::get_member_update(Frame& frame)
{
    // A compound write's read of base[key] (§13.15.2): the base checked and
    // the key made a property key once, here, before the right-hand side;
    // the key stays on the stack for the write, as made.
    Reference reference;
    reference.kind = Reference::Kind::Property;
    reference.base = frame.peek(1);
    reference.key_value = frame.peek(0);
    if (!frame.peek(0).is_object()) {
        std::optional<PropertyKey> const converted = self.to_property_key(frame.peek(0));
        if (!converted)
            return false;
        reference.key = *converted;
        reference.key_ready = true;
    }
    std::optional<Value> const value = get_value(reference, frame.strict);
    if (!value)
        return false;
    if (frame.peek(0).is_object())
        frame.peek(0) = key_to_value(heap(), reference.key);
    frame.push(*value);
    return true;
}

Context Interpreter::Impl::frame_context(Frame const& frame) const
{
    return Context { frame.envs.back(), frame.variable, frame.program, frame.function, frame.strict, frame.private_environment };
}

// A throw at the instruction just executed: the innermost handler whose
// range covers it takes over with the stacks cut back to its depths and
// the thrown value pushed. A termination (the interrupt) is uncatchable.
bool Interpreter::Impl::vm_unwind(Frame& frame)
{
    if (self.m_terminated)
        return false;
    std::uint32_t const at = frame.pc - 1;
    for (Handler const& handler : frame.code->handlers) {
        if (at < handler.start || at >= handler.end)
            continue;
        Value const thrown = self.take_exception();
        frame.stack.resize(handler.stack_depth);
        frame.refs.resize(handler.ref_depth);
        frame.envs.resize(1 + handler.env_depth);
        if (frame.builders.size() > handler.class_depth) {
            // A throw out of a class body: the classes it left half-made
            // are dropped, and the private names and strictness of the
            // outermost one's outside come back.
            ClassBuilder const& outermost = *frame.builders[handler.class_depth];
            frame.private_environment = outermost.outer_private;
            frame.strict = outermost.saved_strict;
            frame.builders.resize(handler.class_depth);
        }
        frame.stack.push_back(thrown);
        frame.pc = handler.target;
        return true;
    }
    return false;
}

// ---- the loop -------------------------------------------------------------

RunStatus Interpreter::Impl::vm_run(Frame& entry)
{
    if (!stack_ok())
        return RunStatus::Threw;
    // The frame is the top one of the stacks (push_frame or restore_frame
    // put it there), and everything it calls is pushed above it.
    ContextScope const context_scope(*this, frame_context(entry));
    // The profile (SASHFOLD_VM_PROFILE=1): the loop's own time, a test per
    // entry when it is off. Each instruction counted by its opcode only in a
    // build made for it (-DSASHFOLD_VM_COUNTS=ON), since a test on every
    // instruction costs every page two per cent.
    Interpreter::ActivityScope const activity(self, self.vm_activity());
    std::uint64_t* executed = nullptr;
#ifdef SASHFOLD_VM_COUNTS
    if (self.vm_profiling()) {
        std::vector<std::uint64_t>& counts = self.account_for_update().executed;
        if (counts.size() < opcode_count)
            counts.resize(opcode_count);
        executed = counts.data();
    }
#endif
    if (entry.resume_pending) {
        entry.push(entry.resume_value);
        entry.resume_pending = false;
        entry.resume_value = Value::undefined();
    }
    // The frame running: the one this run began with, or a call it made
    // itself (enter_call), pushed above it.
    Frame* current = &entry;
    for (;;) {
        Frame* next = nullptr;
        RunStatus const status = vm_run_frame(*current, next, executed);
        if (status == RunStatus::Switched) {
            current = next;
            continue;
        }
        if (status == RunStatus::Threw) {
            // No handler in the frame that threw: a call the loop made ends by
            // the throw, and its caller looks for one from its call.
            bool handled = false;
            while (current->inlined) {
                current = leave_call(*current);
                if (vm_unwind(*current)) {
                    handled = true;
                    break;
                }
            }
            if (handled)
                continue;
        }
        return status;
    }
}

// The run loop dispatches through label addresses where the compiler has
// them (gcc and clang, GNU extensions both: allowed in this function alone)
// and by a switch elsewhere, or when built with -DSASHFOLD_VM_SWITCH.
#if defined(__GNUC__) && !defined(SASHFOLD_VM_SWITCH)
#define SASHFOLD_VM_THREADED 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#else
#define SASHFOLD_VM_THREADED 0
#endif
RunStatus Interpreter::Impl::vm_run_frame(Frame& frame, Frame*& next, std::uint64_t* executed)
{
    static_cast<void>(executed);
    CodeBlock const& code = *frame.code;
    Heap& h = heap();
    WellKnownAtoms const& well_known = atoms();
    // What this block's sites have seen (js/Feedback.h): made at its first
    // run; an instruction with a site reads it only when the block has one.
    FeedbackVector* const feedback = feedback_for(code);

    // A property reference to `base[key]` with the key converted at once
    // unless it is an object, which waits for the base check (the order
    // evaluate_member_reference keeps).
    auto member_reference = [&](Value const& base, Value const& key, Reference& reference) -> bool {
        reference.kind = Reference::Kind::Property;
        reference.base = base;
        reference.key_value = key;
        if (key.is_object())
            return true;
        std::optional<PropertyKey> const converted = self.to_property_key(key);
        if (!converted)
            return false;
        reference.key = *converted;
        reference.key_ready = true;
        return true;
    };
    auto named_reference = [&](Value const& base, JsString* name) {
        Reference reference;
        reference.kind = Reference::Kind::Property;
        reference.base = base;
        reference.key = h.key(name);
        reference.key_ready = true;
        return reference;
    };
    auto iterator_record = [&](std::uint32_t reg) {
        IteratorRecord record;
        record.iterator = frame.registers[reg];
        record.next_method = frame.registers[reg + 1];
        record.done = false;
        return record;
    };
    auto iterator_done = [&](std::uint32_t reg) {
        Value const& done = frame.registers[reg + 2];
        return done.is_boolean() && done.as_boolean();
    };
    auto iter_result_field = [&](Value const& result, JsString* field) -> std::optional<Value> {
        if (!result.is_object())
            return self.throw_type_error("Iterator result " + self.describe(result) + " is not an object");
        return self.get(result, PropertyKey::atom(field));
    };
    auto direct_eval = [&](Args arguments) -> std::optional<Value> {
        // §13.3.6.1 step 6: a direct eval of a string runs in this scope;
        // anything else is returned as it is — unless the host reads code
        // out of it (HostGetCodeForEval: a TrustedScript).
        if (arguments.empty())
            return Value::undefined();
        Value source = arguments[0];
        if (!source.is_string()) {
            std::optional<JsString*> const host_code = self.on_code_for_eval ? self.on_code_for_eval(source) : std::nullopt;
            if (!host_code)
                return source;
            source = Value::string(*host_code);
        }
        Interpreter::Roots const roots(self);
        self.root(source);
        if (!tick())
            return std::nullopt;
        return perform_eval(source.as_string()->view(), frame.envs.back(), frame.strict, Value::empty(), true, frame.private_environment,
            frame.program);
    };
    // Resolved bindings. The environment `hops` out from the current one:
    // the parser counted the scopes that materialize, which are exactly the
    // environments on the chain, so the walk needs no names; a slot past
    // the end would be a compiler's fault, reported rather than read.
    auto scoped_environment = [&](std::uint32_t hops) -> Environment* {
        Environment* env = frame.envs.back();
        for (std::uint32_t i = 0; i < hops && env != nullptr; ++i)
            env = env->outer();
        return env;
    };
    auto scoped_binding = [&](std::uint32_t hops, std::uint32_t slot) -> Environment::Binding* {
        Environment* env = scoped_environment(hops);
        if (env == nullptr || slot >= env->binding_count()) {
            self.throw_type_error("internal: a resolved binding outside its environment");
            return nullptr;
        }
        return &env->binding_at(slot);
    };
    // One more environment for this frame: its region grows at the top of
    // the environment stack, and a full stack is the RangeError a deep
    // recursion gets.
    auto env_room = [&]() -> bool {
        if (frame.envs.has_room())
            return true;
        self.throw_range_error("Maximum call stack size exceeded");
        return false;
    };
    auto dead_zone = [&](JsString* name) {
        self.throw_reference_error("Cannot access '" + (name ? name->to_utf8() : std::string()) + "' before initialization");
    };
    auto register_name = [&](std::uint32_t reg) -> JsString* {
        return reg < code.register_names.size() ? code.register_names[reg] : nullptr;
    };
    // PutValue to a register: its dead zone first, then a const's TypeError.
    auto write_local = [&](std::uint32_t reg, bool immutable, Value const& value) -> bool {
        Value& held = frame.registers[reg];
        if (held.is_empty()) {
            dead_zone(register_name(reg));
            return false;
        }
        if (immutable) {
            self.throw_type_error("Assignment to constant variable.");
            return false;
        }
        held = value;
        return true;
    };
    // `this` of the running plain function: its own environment's when it
    // has one, else the frame's; the hole is a derived constructor's before
    // super().
    auto frame_this = [&]() -> std::optional<Value> {
        Value value = frame.this_value;
        if (frame.function_env != nullptr)
            value = frame.function_env->this_initialized() ? frame.function_env->this_value() : Value::empty();
        if (value.is_empty())
            return self.throw_reference_error("Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
        return value;
    };
    auto call_with = [&](Instruction const& ins, Value const& callee, Value const& this_value, Args arguments, bool eval) -> std::optional<Value> {
        if (eval && callee.is_object() && callee.as_object() == self.intrinsics().eval)
            return direct_eval(arguments);
        if (!Interpreter::is_callable(callee)) {
            Context const cx = frame_context(frame);
            return self.throw_type_error(expression_text(code.nodes[ins.b], cx) + " is not a function");
        }
        return self.call(callee, this_value, arguments);
    };

    // Dispatch. Every handler ends in VM_NEXT, back to the head of the loop
    // for the next instruction, or VM_FAIL, with the exception pending, to
    // the unwinder at its foot. Under gcc and clang the head jumps through a
    // table of the handlers' label addresses, with none of a switch's range
    // check; elsewhere the same handlers are the cases of a switch. The jump
    // is not copied into every handler (SpiderMonkey's threaded form): on
    // this machine's predictor that measured no faster (the kernels 0.89x
    // against 0.90x) and made this function's frame 880 bytes larger, which
    // a recursion through natives pays at every level; and a computed goto
    // may not leave a scope holding a destructor, which a few handlers have.
#if SASHFOLD_VM_THREADED
    static void* const dispatch[] = {
#define SASHFOLD_VM_LABEL(name, effect) &&op_##name,
        SASHFOLD_OPCODES(SASHFOLD_VM_LABEL)
#undef SASHFOLD_VM_LABEL
    };
#define VM_CASE(name) case Opcode::name: op_##name
#else
#define VM_CASE(name) case Opcode::name
#endif
#define VM_NEXT continue
#define VM_FAIL goto vm_fail
    Instruction const* ins;
    for (;;) {
        ins = &code.code[frame.pc++];
#ifdef SASHFOLD_VM_COUNTS
        if (executed != nullptr) [[unlikely]]
            ++executed[static_cast<std::size_t>(ins->op)];
#endif
#if SASHFOLD_VM_THREADED
        goto* dispatch[static_cast<std::size_t>(ins->op)];
#endif
        switch (ins->op) {
        // ---- stack
        VM_CASE(PushUndefined):
            frame.push(Value::undefined());
            VM_NEXT;
        VM_CASE(PushNull):
            frame.push(Value::null());
            VM_NEXT;
        VM_CASE(PushTrue):
            frame.push(Value::boolean(true));
            VM_NEXT;
        VM_CASE(PushFalse):
            frame.push(Value::boolean(false));
            VM_NEXT;
        VM_CASE(PushEmpty):
            frame.push(Value::empty());
            VM_NEXT;
        VM_CASE(PushConstant):
            frame.push(code.constants[ins->a]);
            VM_NEXT;
        VM_CASE(PushBigInt):
            frame.push(self.bigint(code.bigints[ins->a]));
            VM_NEXT;
        VM_CASE(PushInt):
            frame.push(ins->a <= 0x7FFFFFFFu ? Value::int32(static_cast<std::int32_t>(ins->a)) : Value::number(static_cast<double>(ins->a)));
            VM_NEXT;
        VM_CASE(Pop):
            frame.stack.pop_back();
            VM_NEXT;
        VM_CASE(Dup): {
            Value const copy = frame.top();
            frame.push(copy);
            VM_NEXT;
        }
        VM_CASE(Over): {
            Value const copy = frame.peek(1);
            frame.push(copy);
            VM_NEXT;
        }
        VM_CASE(Swap):
            std::swap(frame.peek(0), frame.peek(1));
            VM_NEXT;
        VM_CASE(LoadReg):
            frame.push(frame.registers[ins->a]);
            VM_NEXT;
        VM_CASE(StoreReg):
            frame.registers[ins->a] = frame.pop();
            VM_NEXT;
        VM_CASE(StoreRegKeep):
            frame.registers[ins->a] = frame.top();
            VM_NEXT;

        // ---- environments
        VM_CASE(PushBlockEnv): {
            if (!env_room()) {
                VM_FAIL;
            }
            Environment* env = new_environment(frame.envs.back());
            frame.envs.push_back(env);
            instantiate_block(*code.declarations[ins->a], env, frame.private_environment);
            VM_NEXT;
        }
        VM_CASE(PushNamesEnv): {
            if (!env_room()) {
                VM_FAIL;
            }
            Environment* env = new_environment(frame.envs.back());
            frame.envs.push_back(env);
            for (JsString* name : code.name_lists[ins->a])
                env->declare(name, Value::undefined(), ins->b != 0, false);
            VM_NEXT;
        }
        VM_CASE(PushWithEnv): {
            if (!env_room()) {
                VM_FAIL;
            }
            // §14.11.2: an object environment marked as a with's, so calls
            // through it get the object as `this`.
            std::optional<Object*> const object = self.to_object(frame.top());
            if (!object) {
                VM_FAIL;
            }
            Roots const roots(self);
            self.root(Value::object(*object));
            Environment* env = new_environment(frame.envs.back(), *object);
            env->set_with_environment(true);
            frame.stack.pop_back();
            frame.envs.push_back(env);
            VM_NEXT;
        }
        VM_CASE(PopEnv):
            frame.envs.pop_back();
            VM_NEXT;
        VM_CASE(CopyIterationEnv): {
            // CreatePerIterationEnvironment (§14.7.4.4); a resolved head's
            // environment is copied whole, slot for slot.
            Environment* previous = frame.envs.back();
            Environment* copy = new_environment(previous->outer());
            if (ins->flags & 1) {
                copy->assign_bindings(previous->bindings());
                frame.envs.back() = copy;
                VM_NEXT;
            }
            for (JsString* name : code.name_lists[ins->a]) {
                Environment::Binding const* binding = previous->find(name);
                copy->declare(name, binding ? binding->value : Value::undefined(), true, binding ? binding->initialized : true);
            }
            frame.envs.back() = copy;
            VM_NEXT;
        }
        VM_CASE(InitializeBinding):
            if (!initialize_binding(code.names[ins->a], frame.top(), frame.envs.back())) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            VM_NEXT;
        VM_CASE(LoadArgument):
            frame.push(ins->a < frame.incoming.size() ? frame.incoming[ins->a] : Value::undefined());
            VM_NEXT;
        VM_CASE(RestArguments): {
            std::span<Value const> const rest = ins->a < frame.incoming.size() ? frame.incoming.subspan(ins->a) : std::span<Value const>();
            frame.push(Value::object(self.new_array(rest)));
            VM_NEXT;
        }

        // ---- resolved bindings
        VM_CASE(GetLocal): {
            Value const value = frame.registers[ins->a];
            if (value.is_empty()) {
                dead_zone(register_name(ins->a));
                VM_FAIL;
            }
            frame.push(value);
            VM_NEXT;
        }
        VM_CASE(SetLocal):
            if (!write_local(ins->a, (ins->flags & 1) != 0, frame.top()))
                VM_FAIL;
            VM_NEXT;
        VM_CASE(GetScoped): {
            Environment::Binding const* binding = scoped_binding(ins->a, ins->b);
            if (binding == nullptr) {
                VM_FAIL;
            }
            if (!binding->initialized) {
                dead_zone(binding->name);
                VM_FAIL;
            }
            Value const value = binding->value;
            frame.push(value);
            VM_NEXT;
        }
        VM_CASE(SetScoped): {
            Environment::Binding* binding = scoped_binding(ins->a, ins->b);
            if (binding == nullptr || !set_mutable_binding(*binding, frame.top(), frame.strict))
                VM_FAIL;
            VM_NEXT;
        }
        VM_CASE(InitScoped): {
            Environment::Binding* binding = scoped_binding(ins->a, ins->b);
            if (binding == nullptr) {
                VM_FAIL;
            }
            binding->value = frame.top();
            binding->initialized = true;
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(PushEnv): {
            if (!env_room()) {
                VM_FAIL;
            }
            // A scope that materializes: its environment with every binding
            // laid out at once. The function's own also holds what the
            // chain is searched for by arrows and eval code: the function,
            // `this` (none yet in a derived constructor) and new.target.
            Environment* env = new_environment(frame.envs.back());
            env->assign_bindings(code.environments[ins->a].bindings);
            if (ins->flags & 1) {
                env->set_function(frame.function);
                if (frame.function != nullptr && !frame.function->node().is_arrow) {
                    if (frame.this_value.is_empty())
                        env->set_this_uninitialized();
                    else
                        env->set_this(frame.this_value);
                    env->set_new_target(frame.new_target);
                }
                frame.function_env = env;
            }
            if (ins->flags & 2)
                env->set_var_scope();
            frame.envs.push_back(env);
            VM_NEXT;
        }
        VM_CASE(MakeArguments): {
            bool const mapped = (ins->flags & 1) != 0;
            Object* arguments_object = make_arguments_object(*frame.function, mapped ? frame.function_env : nullptr, frame.incoming, mapped);
            frame.push(Value::object(arguments_object));
            VM_NEXT;
        }
        VM_CASE(LoadThis): {
            std::optional<Value> const value = frame_this();
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(LoadNewTarget):
            frame.push(frame.new_target ? Value::object(frame.new_target) : Value::undefined());
            VM_NEXT;
        VM_CASE(Suspend):
            // FunctionDeclarationInstantiation done at the call (§15.5.2);
            // the arguments are the caller's and are not kept past it.
            frame.incoming = {};
            frame.result = Value::empty();
            frame.resume_pending = true;
            return RunStatus::Yielded;
        VM_CASE(AnnexBCopy): {
            // B.3.2.1 step 2.b: a sloppy block-level function's current value
            // to the var binding the parser hoisted for it.
            JsString* name = code.names[ins->a];
            if (frame.strict || frame.envs.back() == frame.variable)
                VM_NEXT;
            Environment::Binding const* block_binding = frame.envs.back()->find(name);
            if (block_binding == nullptr)
                VM_NEXT;
            Value const value = block_binding->value;
            if (frame.variable->is_object_environment()) {
                if (!self.set(*frame.variable->object(), PropertyKey::atom(name), value, false))
                    VM_FAIL;
            } else if (Environment::Binding* var_binding = frame.variable->find(name)) {
                var_binding->value = value;
            }
            VM_NEXT;
        }
        VM_CASE(ResolveThis): {
            std::optional<Value> const value = resolve_this(frame.envs.back());
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(NewTarget): {
            Context cx = frame_context(frame);
            frame.push(evaluate_new_target(cx));
            VM_NEXT;
        }

        // ---- references
        VM_CASE(RefName):
            if (ins->site != no_site)
                frame.refs.push_back(global_reference(feedback->property(ins->site), code.names[ins->a], frame.envs.back()));
            else
                frame.refs.push_back(resolve(code.names[ins->a], frame.envs.back()));
            VM_NEXT;
        VM_CASE(RefLocal): {
            Reference reference;
            reference.kind = Reference::Kind::Local;
            reference.slot = ins->a;
            reference.immutable = (ins->flags & 1) != 0;
            frame.refs.push_back(reference);
            VM_NEXT;
        }
        VM_CASE(RefScoped): {
            Environment* env = scoped_environment(ins->a);
            if (env == nullptr || ins->b >= env->binding_count()) {
                self.throw_type_error("internal: a resolved binding outside its environment");
                VM_FAIL;
            }
            Reference reference;
            reference.kind = Reference::Kind::Scoped;
            reference.environment = env;
            reference.slot = ins->b;
            frame.refs.push_back(reference);
            VM_NEXT;
        }
        VM_CASE(RefMember): {
            Reference reference;
            if (!member_reference(frame.peek(1), frame.peek(0), reference)) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            frame.stack.pop_back();
            frame.refs.push_back(reference);
            VM_NEXT;
        }
        VM_CASE(RefMemberNamed):
            frame.refs.push_back(named_reference(frame.top(), code.names[ins->a]));
            frame.stack.pop_back();
            VM_NEXT;
        VM_CASE(RefSuper): {
            // Flag 1: in the method itself, whose frame has `this` and the
            // function with its home object.
            std::optional<Reference> reference;
            if (ins->flags & 1) {
                std::optional<Value> const this_value = frame_this();
                if (this_value)
                    reference = super_reference_of(*this_value, frame.function ? frame.function->home_object() : nullptr, &frame.top(), nullptr);
            } else {
                reference = super_reference(frame_context(frame), &frame.top(), nullptr);
            }
            if (!reference) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            frame.refs.push_back(*reference);
            VM_NEXT;
        }
        VM_CASE(RefSuperNamed): {
            std::optional<Reference> reference;
            if (ins->flags & 1) {
                std::optional<Value> const this_value = frame_this();
                if (this_value)
                    reference = super_reference_of(*this_value, frame.function ? frame.function->home_object() : nullptr, nullptr, code.names[ins->a]);
            } else {
                reference = super_reference(frame_context(frame), nullptr, code.names[ins->a]);
            }
            if (!reference) {
                VM_FAIL;
            }
            frame.refs.push_back(*reference);
            VM_NEXT;
        }
        VM_CASE(RefPrivate): {
            // MakePrivateReference (§13.3.3).
            JsString* name = code.names[ins->a];
            Symbol* private_name = frame.private_environment ? frame.private_environment->lookup(name) : nullptr;
            if (private_name == nullptr) {
                self.throw_syntax_error("Private field '" + name->to_utf8() + "' must be declared in an enclosing class");
                VM_FAIL;
            }
            Reference reference;
            reference.kind = Reference::Kind::Private;
            reference.base = frame.top();
            reference.name = name;
            reference.key = PropertyKey::symbol(private_name);
            reference.key_ready = true;
            frame.stack.pop_back();
            frame.refs.push_back(reference);
            VM_NEXT;
        }
        VM_CASE(RefGet): {
            if (Reference const& reference = frame.refs.back(); reference.kind == Reference::Kind::Local) {
                Value const value = frame.registers[reference.slot];
                if (value.is_empty()) {
                    dead_zone(register_name(reference.slot));
                    VM_FAIL;
                }
                frame.push(value);
                VM_NEXT;
            } else if (reference.cached_shape != nullptr && cached_reference_holds(reference)) {
                frame.push(reference.environment->object()->slot_value(reference.slot));
                VM_NEXT;
            }
            Context const cx = frame_context(frame);
            std::optional<Value> const value = get_value(frame.refs.back(), cx);
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(RefPut):
        VM_CASE(RefPutKeep): {
            Reference& reference = frame.refs.back();
            bool stored = false;
            if (reference.cached_shape != nullptr && cached_reference_holds(reference)) {
                // A global's own writable data property, still there as the
                // cache found it when the name was resolved: SetMutableBinding
                // finds it (§9.1.1.2.5) and OrdinarySet writes its value.
                reference.environment->object()->cache_store(reference.slot, frame.top());
                stored = true;
            } else {
                stored = reference.kind == Reference::Kind::Local
                    ? write_local(reference.slot, reference.immutable, frame.top())
                    : put_value(reference, frame.top(), frame_context(frame));
            }
            if (!stored) {
                VM_FAIL;
            }
            frame.refs.pop_back();
            if (ins->op == Opcode::RefPut)
                frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(RefThis):
            frame.push(this_for_call(frame.refs.back()));
            VM_NEXT;
        VM_CASE(RefDrop):
            frame.refs.pop_back();
            VM_NEXT;
        VM_CASE(RefDelete): {
            Context const cx = frame_context(frame);
            std::optional<Value> const value = delete_reference(frame.refs.back(), cx);
            frame.refs.pop_back();
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(GetName): {
            // A name that can only be the global environment's goes through
            // its site's cache; any other is resolved by name.
            if (ins->site != no_site) {
                std::optional<Value> const value = get_global(feedback->property(ins->site), code.names[ins->a], frame.envs.back(), frame.strict, false);
                if (!value) {
                    VM_FAIL;
                }
                frame.push(*value);
                VM_NEXT;
            }
            Reference reference = resolve(code.names[ins->a], frame.envs.back());
            std::optional<Value> const value = get_value(reference, frame.strict);
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(TypeofName): {
            // §13.5.3: an unresolvable name is "undefined", not an error
            // (the cache's path answers empty for one).
            if (ins->site != no_site) {
                std::optional<Value> const value = get_global(feedback->property(ins->site), code.names[ins->a], frame.envs.back(), frame.strict, true);
                if (!value) {
                    VM_FAIL;
                }
                frame.push(Value::string(value->is_empty() ? well_known.undefined : self.type_of(*value)));
                VM_NEXT;
            }
            Reference reference = resolve(code.names[ins->a], frame.envs.back());
            if (reference.kind == Reference::Kind::Unresolvable) {
                frame.push(Value::string(well_known.undefined));
                VM_NEXT;
            }
            std::optional<Value> const value = get_value(reference, frame.strict);
            if (!value) {
                VM_FAIL;
            }
            frame.push(Value::string(self.type_of(*value)));
            VM_NEXT;
        }
        VM_CASE(GetMemberNamed): {
            // GetValue of base.name (§6.2.5.5): [[Get]] with the base as the
            // receiver, a primitive's through its wrapper's prototype, a
            // nullish one's TypeError naming the key. An object's read goes
            // through the site's cache: an own data property of a shared
            // shape is a compare and a load here, the rest out of the loop.
            Value const base = frame.top();
            if (ins->site != no_site && base.is_object()) {
                PropertySite& site = feedback->property(ins->site);
                Object const& object = *base.as_object();
                Shape const* const shape = object.shape();
                if (site.state == PropertySite::Monomorphic && site.first.shape == shape && site.first.kind == CacheKind::OwnData && !shape->is_dictionary()) {
                    ++ic_hits;
                    frame.top() = object.slot_value(site.first.slot);
                    VM_NEXT;
                }
                std::optional<Value> const cached = get_named(&site, base, code.names[ins->a]);
                if (!cached) {
                    VM_FAIL;
                }
                frame.top() = *cached;
                VM_NEXT;
            }
            std::optional<Value> const value = self.get(base, h.key(code.names[ins->a]));
            if (!value) {
                VM_FAIL;
            }
            frame.top() = *value;
            VM_NEXT;
        }
        // ---- member writes, with no reference made
        VM_CASE(PutMemberNamed):
        VM_CASE(PutMemberNamedKeep): {
            // PutValue (§6.2.5.6) of base.name: an object base is set at
            // once; any other (a primitive, or the TypeError of a nullish
            // one) the way a reference to it is put.
            Value const& base = frame.peek(1);
            Value const& value = frame.peek(0);
            if (base.is_object() && ins->site != no_site) {
                PropertySite& site = feedback->property(ins->site);
                Object& object = *base.as_object();
                Shape const* const shape = object.shape();
                if (site.state == PropertySite::Monomorphic && site.first.shape == shape && site.first.kind == CacheKind::OwnData && !shape->is_dictionary()) {
                    ++ic_hits;
                    object.cache_store(site.first.slot, value);
                } else if (!put_named(&site, base, code.names[ins->a], value, frame.strict)) {
                    VM_FAIL;
                }
            } else if (base.is_object()) {
                if (!self.set(base, h.key(code.names[ins->a]), value, frame.strict)) {
                    VM_FAIL;
                }
            } else if (!put_member_named_slow(frame, base, code.names[ins->a], value)) {
                VM_FAIL;
            }
            if (ins->op == Opcode::PutMemberNamedKeep)
                frame.peek(1) = value;
            else
                frame.stack.pop_back();
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(PutMember):
        VM_CASE(PutMemberKeep): {
            // PutValue of base[key], the key made a property key now, after
            // the value (§13.3.3 note): a primitive key at once (nothing to
            // observe), an object one where the reference would.
            Value const& base = frame.peek(2);
            Value const& key = frame.peek(1);
            Value const& value = frame.peek(0);
            if (ins->site != no_site)
                note_element(feedback->element(ins->site), base, key);
            if (base.is_object() && !key.is_object()) {
                std::optional<PropertyKey> const converted = self.to_property_key(key);
                if (!converted || !self.set(base, *converted, value, frame.strict)) {
                    VM_FAIL;
                }
            } else if (!put_member_slow(frame, base, key, value)) {
                VM_FAIL;
            }
            if (ins->op == Opcode::PutMemberKeep) {
                frame.peek(2) = value;
                frame.stack.pop_back();
                frame.stack.pop_back();
            } else {
                frame.stack.resize(frame.stack.size() - 3);
            }
            VM_NEXT;
        }
        VM_CASE(GetMemberUpdate):
            if (!get_member_update(frame))
                VM_FAIL;
            VM_NEXT;
        VM_CASE(GetMember): {
            // An array read at an int32 index inside the dense storage:
            // the element itself, which is what [[Get]] would answer. A
            // hole, an index past the storage and every other base take
            // the reference path.
            if (Value const& key = frame.peek(0); key.is_int32() && key.as_int32() >= 0) {
                Value const& base = frame.peek(1);
                if (base.is_object() && base.as_object()->class_id() == Object::Class::Array) {
                    auto const& elements = static_cast<ArrayObject const*>(base.as_object())->dense();
                    auto const index = static_cast<std::uint32_t>(key.as_int32());
                    if (index < elements.size() && !elements[index].is_empty()) {
                        if (ins->site != no_site) {
                            if (ElementSite& seen = feedback->element(ins->site); (seen.kinds & ElementDense) == 0)
                                seen.kinds |= ElementDense;
                        }
                        Value const element = elements[index];
                        frame.stack.pop_back();
                        frame.top() = element;
                        VM_NEXT;
                    }
                }
            }
            if (ins->site != no_site)
                note_element(feedback->element(ins->site), frame.peek(1), frame.peek(0));
            // GetValue of base[key]: a primitive key made a property key at
            // once (nothing to observe), then [[Get]]; an object key waits
            // for the base's check, out of the loop.
            Value const key = frame.peek(0);
            if (key.is_object()) [[unlikely]] {
                if (!get_member_slow(frame))
                    VM_FAIL;
                VM_NEXT;
            }
            std::optional<PropertyKey> const converted = self.to_property_key(key);
            if (!converted) {
                VM_FAIL;
            }
            Value const base = frame.peek(1);
            std::optional<Value> const value = self.get(base, *converted);
            if (!value) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            frame.top() = *value;
            VM_NEXT;
        }

        // ---- operators
        VM_CASE(Binary): {
            auto const op = static_cast<BinaryOp>(ins->a);
            Value const& left = frame.peek(1);
            Value const& right = frame.peek(0);
            // Two numbers, which most operands are: answered here without
            // the conversions and the rooting the general path needs —
            // in int32 arithmetic when both are int32s and the answer is
            // exact there, else in doubles.
            if (left.is_int32() && right.is_int32()) {
                // Recorded once: a store at every run would cost more
                // than the operation it records.
                if (ins->site != no_site) {
                    if (std::uint8_t& seen = feedback->operands(ins->site); (seen & OperandInt32) == 0)
                        seen |= OperandInt32;
                }
                if (std::optional<Value> const fast = int32_binary(op, left.as_int32(), right.as_int32())) {
                    frame.stack.pop_back();
                    frame.top() = *fast;
                    VM_NEXT;
                }
            } else if (ins->site != no_site) {
                auto const kinds = static_cast<std::uint8_t>(operand_kind(left) | operand_kind(right));
                if (std::uint8_t& seen = feedback->operands(ins->site); (seen & kinds) != kinds)
                    seen |= kinds;
            }
            if (left.is_number() && right.is_number()) {
                if (std::optional<Value> const fast = number_binary(op, left.as_number(), right.as_number())) {
                    frame.stack.pop_back();
                    frame.top() = *fast;
                    VM_NEXT;
                }
            }
            std::optional<Value> const result = apply_binary(op, left, right);
            if (!result) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            frame.top() = *result;
            VM_NEXT;
        }
        VM_CASE(Unary): {
            Value& operand = frame.top();
            switch (static_cast<UnaryOp>(ins->a)) {
            case UnaryOp::Not:
                operand = Value::boolean(!to_boolean(operand));
                break;
            case UnaryOp::Minus: {
                std::optional<Value> const numeric = self.to_numeric(operand);
                if (!numeric) {
                    VM_FAIL;
                }
                operand = numeric->is_bigint() ? self.bigint(numeric->as_bigint()->value().negated())
                                               : Value::number(-numeric->as_number());
                break;
            }
            case UnaryOp::Plus: {
                std::optional<double> const number = self.to_number(operand);
                if (!number) {
                    VM_FAIL;
                }
                operand = Value::number(*number);
                VM_NEXT;
            }
            case UnaryOp::BitwiseNot: {
                std::optional<Value> const numeric = self.to_numeric(operand);
                if (!numeric) {
                    VM_FAIL;
                }
                operand = numeric->is_bigint()
                    ? self.bigint(numeric->as_bigint()->value().bitwise_not())
                    : Value::int32(~Interpreter::double_to_int32(numeric->as_number()));
                VM_NEXT;
            }
            case UnaryOp::Typeof:
                operand = Value::string(self.type_of(operand));
                VM_NEXT;
            default:
                VM_NEXT;
            }
            VM_NEXT;
        }
        VM_CASE(ToNumeric): {
            if (frame.top().is_number())
                VM_NEXT; // a number is its own ToNumeric
            std::optional<Value> const numeric = self.to_numeric(frame.top());
            if (!numeric) {
                VM_FAIL;
            }
            frame.top() = *numeric;
            VM_NEXT;
        }
        VM_CASE(Inc):
        VM_CASE(Dec): {
            // The operand is numeric already (ToNumeric went before).
            Value const& operand = frame.top();
            if (operand.is_bigint()) {
                BigInteger const one = BigInteger::from_int64(1);
                BigInteger const& old = operand.as_bigint()->value();
                frame.top() = self.bigint(ins->op == Opcode::Inc ? old + one : old - one);
            } else if (operand.is_int32() && operand.as_int32() != (ins->op == Opcode::Inc ? INT32_MAX : INT32_MIN)) {
                frame.top() = Value::int32(operand.as_int32() + (ins->op == Opcode::Inc ? 1 : -1));
            } else {
                frame.top() = Value::number(operand.as_number() + (ins->op == Opcode::Inc ? 1 : -1));
            }
            VM_NEXT;
        }
        VM_CASE(ToPropertyKey): {
            std::optional<PropertyKey> const key = self.to_property_key(frame.top());
            if (!key) {
                VM_FAIL;
            }
            frame.top() = key_to_value(h, *key);
            VM_NEXT;
        }
        VM_CASE(ToString): {
            std::optional<JsString*> const text = self.to_string(frame.top());
            if (!text) {
                VM_FAIL;
            }
            frame.top() = Value::string(*text);
            VM_NEXT;
        }
        VM_CASE(StringConcat): {
            if (frame.peek(1).as_string()->length() + frame.peek(0).as_string()->length() > h.max_string_length()) {
                self.throw_range_error("Invalid string length");
                VM_FAIL;
            }
            // Both halves are on the stack, rooted, through the allocation.
            JsString* joined = h.concat(frame.peek(1).as_string(), frame.peek(0).as_string());
            frame.stack.pop_back();
            frame.top() = Value::string(joined);
            VM_NEXT;
        }
        VM_CASE(PrivateIn): {
            Context const cx = frame_context(frame);
            std::optional<Value> const value = private_in(code.names[ins->a], frame.top(), cx);
            if (!value) {
                VM_FAIL;
            }
            frame.top() = *value;
            VM_NEXT;
        }
        VM_CASE(RequireObjectCoercible): {
            Value const& value = frame.top();
            if (value.is_nullish()) {
                self.throw_type_error("Cannot destructure '" + self.describe(value) + "' as it is " + (value.is_null() ? "null" : "undefined") + ".");
                VM_FAIL;
            }
            VM_NEXT;
        }
        VM_CASE(ThrowTypeErrorConst):
            self.throw_type_error(code.constants[ins->a].as_string()->to_utf8());
            VM_FAIL;

        // ---- control
        VM_CASE(Jump):
            frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfTrue):
            if (to_boolean(frame.pop()))
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfFalse):
            if (!to_boolean(frame.pop()))
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfTrueKeep):
            if (to_boolean(frame.top()))
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfFalseKeep):
            if (!to_boolean(frame.top()))
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfNullish):
            if (frame.pop().is_nullish())
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfNotNullishKeep):
            if (!frame.top().is_nullish())
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfNotUndefined):
            if (!frame.pop().is_undefined())
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfEmpty):
            if (frame.pop().is_empty())
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(Switch): {
            Value const& token = frame.registers[ins->a];
            std::vector<std::uint32_t> const& table = code.jump_tables[ins->b];
            std::size_t const index = token.is_number() ? static_cast<std::size_t>(token.as_number()) : table.size();
            if (index >= table.size()) {
                self.throw_type_error("internal: a finally block was entered with no token");
                VM_FAIL;
            }
            frame.pc = table[index];
            VM_NEXT;
        }
        VM_CASE(JumpIfResumeNormal):
            if (frame.resume_kind == ResumeKind::Normal)
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(JumpIfResumeReturn):
            if (frame.resume_kind == ResumeKind::Return)
                frame.pc = ins->a;
            VM_NEXT;
        VM_CASE(Return): {
            if (frame.inlined) {
                // A call this loop made: back to its caller, which takes the
                // call's operands off its stack and the result on.
                Value const result = frame.pop();
                Frame* caller = leave_call(frame);
                std::size_t const argc = caller->code->code[caller->pc - 1].a;
                caller->stack.resize(caller->stack.size() - argc - 2);
                caller->push(result);
                next = caller;
                return RunStatus::Switched;
            }
            frame.result = frame.pop();
            return RunStatus::Completed;
        }
        VM_CASE(Throw):
            self.throw_value(frame.pop());
            VM_FAIL;
        VM_CASE(Step):
            if (!tick())
                VM_FAIL;
            VM_NEXT;

        // ---- calls
        VM_CASE(Call):
        VM_CASE(CallEval): {
            std::size_t const argc = ins->a;
            std::size_t const size = frame.stack.size();
            Args const arguments(frame.stack.data() + size - argc, argc);
            if (ins->site != no_site)
                note_call(feedback->call(ins->site), frame.stack[size - argc - 2]);
            // A plain script function runs here, on a frame pushed above this
            // one: no C++ call, no second run loop. Its Return takes the
            // callee, `this` and the arguments off this stack.
            if (Value const& target = frame.stack[size - argc - 2]; ins->op == Opcode::Call && target.is_object()
                && target.as_object()->class_id() == Object::Class::Function && static_cast<Function*>(target.as_object())->is_script()) {
                bool threw = false;
                if (Frame* callee = enter_call(target, frame.stack[size - argc - 1], arguments, threw)) {
                    next = callee;
                    return RunStatus::Switched;
                }
                if (threw) {
                    VM_FAIL;
                }
            }
            std::optional<Value> const result = call_with(*ins, frame.stack[size - argc - 2], frame.stack[size - argc - 1], arguments,
                ins->op == Opcode::CallEval);
            if (!result) {
                VM_FAIL;
            }
            frame.stack.resize(size - argc - 2);
            frame.push(*result);
            VM_NEXT;
        }
        VM_CASE(CallArray):
        VM_CASE(CallEvalArray): {
            std::size_t const size = frame.stack.size();
            std::vector<Value> const arguments = array_arguments(frame.stack[size - 1]);
            std::optional<Value> const result = call_with(*ins, frame.stack[size - 3], frame.stack[size - 2], arguments,
                ins->op == Opcode::CallEvalArray);
            if (!result) {
                VM_FAIL;
            }
            frame.stack.resize(size - 3);
            frame.push(*result);
            VM_NEXT;
        }
        VM_CASE(New):
        VM_CASE(NewArray): {
            std::size_t const size = frame.stack.size();
            std::vector<Value> spread_arguments;
            Args arguments;
            std::size_t consumed = 0;
            if (ins->op == Opcode::NewArray) {
                spread_arguments = array_arguments(frame.stack[size - 1]);
                arguments = spread_arguments;
                consumed = 2;
            } else {
                arguments = Args(frame.stack.data() + size - ins->a, ins->a);
                consumed = ins->a + 1;
            }
            Value const& constructor = frame.stack[size - consumed];
            if (ins->site != no_site)
                note_call(feedback->call(ins->site), constructor);
            if (!Interpreter::is_constructor(constructor)) {
                Context const cx = frame_context(frame);
                self.throw_type_error(expression_text(code.nodes[ins->b], cx) + " is not a constructor");
                VM_FAIL;
            }
            std::optional<Value> const result = self.construct(constructor, arguments);
            if (!result) {
                VM_FAIL;
            }
            frame.stack.resize(size - consumed);
            frame.push(*result);
            VM_NEXT;
        }
        VM_CASE(SuperCall):
        VM_CASE(SuperCallArray): {
            std::size_t const size = frame.stack.size();
            std::vector<Value> spread_arguments;
            Args arguments;
            std::size_t consumed = 0;
            if (ins->op == Opcode::SuperCallArray) {
                spread_arguments = array_arguments(frame.stack[size - 1]);
                arguments = spread_arguments;
                consumed = 1;
            } else {
                arguments = Args(frame.stack.data() + size - ins->a, ins->a);
                consumed = ins->a;
            }
            std::optional<Value> result;
            if (code.slots && frame.function != nullptr && !frame.function->node().is_arrow && frame.function_env == nullptr) {
                // A derived constructor with no environment of its own
                // keeps `this` on its frame: super() binds it there
                // (§13.3.7.1 steps 6–7 with the frame as the this binding).
                result = super_construct(*frame.function, frame.new_target, arguments);
                if (result && !frame.this_value.is_empty()) {
                    self.throw_reference_error("Super constructor may only be called once");
                    result.reset();
                }
                if (result) {
                    frame.this_value = *result;
                    if (!initialize_instance_elements(*result->as_object(), *frame.function))
                        result.reset();
                }
            } else {
                Context cx = frame_context(frame);
                result = super_call(cx, arguments);
            }
            if (!result) {
                VM_FAIL;
            }
            frame.stack.resize(size - consumed);
            frame.push(*result);
            VM_NEXT;
        }

        // ---- literals
        VM_CASE(NewArrayLiteral): {
            auto* array = static_cast<ArrayObject*>(self.new_array());
            if (ins->a != 0)
                array->reserve_elements(ins->a);
            frame.push(Value::object(array));
            VM_NEXT;
        }
        VM_CASE(ArrayPush): {
            auto* array = static_cast<ArrayObject*>(frame.peek(1).as_object());
            array->push(frame.top());
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(ArrayHole): {
            auto* array = static_cast<ArrayObject*>(frame.top().as_object());
            array->set_length(array->length() + 1);
            VM_NEXT;
        }
        VM_CASE(ArraySpread): {
            // §13.2.4.1: the iterable's values, each an element of its own.
            std::optional<std::vector<Value>> const values = self.iterable_to_list(frame.top());
            if (!values) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            auto* array = static_cast<ArrayObject*>(frame.top().as_object());
            for (Value const& value : *values)
                array->push(value);
            VM_NEXT;
        }
        VM_CASE(NewObject): {
            Object* object = self.new_object();
            if (ins->a != 0)
                object->reserve_properties(ins->a);
            frame.push(Value::object(object));
            VM_NEXT;
        }
        VM_CASE(SetPrototype): {
            Value const value = frame.pop();
            Object* object = frame.top().as_object();
            if (value.is_object())
                object->set_prototype(value.as_object());
            else if (value.is_null())
                object->set_prototype(nullptr);
            VM_NEXT;
        }
        VM_CASE(CopyDataProperties): {
            Object* target = frame.peek(1).as_object();
            if (!copy_data_properties(*target, frame.top(), {})) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(CopyDataPropertiesExcluding): {
            auto* taken = static_cast<ArrayObject*>(frame.registers[ins->a].as_object());
            std::vector<PropertyKey> excluded;
            for (std::uint32_t i = 0; i < taken->length(); ++i)
                excluded.push_back(key_from_value(h, taken->element(i)));
            Object* target = frame.peek(1).as_object();
            if (!copy_data_properties(*target, frame.top(), excluded)) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(DefinePropertyNamed): {
            Object* object = frame.peek(1).as_object();
            if (!self.create_data_property(*object, h.key(code.names[ins->a]), frame.top())) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(DefinePropertyDyn): {
            Object* object = frame.peek(2).as_object();
            PropertyKey const key = key_from_value(h, frame.peek(1));
            if (!self.create_data_property(*object, key, frame.top())) {
                VM_FAIL;
            }
            frame.stack.pop_back();
            frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(DefineMethod):
        VM_CASE(DefineMethodDyn): {
            // A method shorthand (§15.4.5): named after its key, with the
            // object as its home for `super.x`.
            bool const dynamic = ins->op == Opcode::DefineMethodDyn;
            Object* object = dynamic ? frame.peek(1).as_object() : frame.top().as_object();
            PropertyKey const key = dynamic ? key_from_value(h, frame.top()) : h.key(code.names[ins->b]);
            Context cx = frame_context(frame);
            std::optional<Value> const value = make_closure(*code.functions[ins->a], cx, &key);
            if (!value) {
                VM_FAIL;
            }
            Roots const roots(self);
            self.root(*value);
            static_cast<ScriptFunction*>(value->as_object())->set_home_object(object);
            if (!self.create_data_property(*object, key, *value)) {
                VM_FAIL;
            }
            if (dynamic)
                frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(DefineAccessor):
        VM_CASE(DefineAccessorDyn): {
            // A getter or setter joins an existing accessor's other half.
            bool const dynamic = ins->op == Opcode::DefineAccessorDyn;
            bool const is_setter = (ins->flags & 1) != 0;
            Object* object = dynamic ? frame.peek(1).as_object() : frame.top().as_object();
            PropertyKey const key = dynamic ? key_from_value(h, frame.top()) : h.key(code.names[ins->b]);
            Heap::NoCollect const no_collect(h);
            ScriptFunction* accessor = self.new_script_function(*code.functions[ins->a], frame.envs.back(), frame.private_environment);
            accessor->set_home_object(object);
            set_function_name(*accessor, key, is_setter ? "set" : "get");
            Object* getter = nullptr;
            Object* setter = nullptr;
            if (PropertyRef const existing = object->find_own(key); existing && existing->accessor) {
                getter = existing->getter;
                setter = existing->setter;
            }
            (is_setter ? setter : getter) = accessor;
            object->put_accessor(key, getter, setter, Enumerable | Configurable);
            if (dynamic)
                frame.stack.pop_back();
            VM_NEXT;
        }
        VM_CASE(NewRegExp): {
            std::optional<Value> const value = evaluate_regexp(*code.regexps[ins->a]);
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(TemplateObject): {
            std::optional<Object*> const site = template_object(*code.templates[ins->a]);
            if (!site) {
                VM_FAIL;
            }
            frame.push(Value::object(*site));
            VM_NEXT;
        }
        VM_CASE(MakeClosure):
        VM_CASE(MakeClosureNamedDyn): {
            bool const dynamic = ins->op == Opcode::MakeClosureNamedDyn;
            PropertyKey key;
            PropertyKey const* name_key = nullptr;
            if (dynamic) {
                key = key_from_value(h, frame.top());
                name_key = &key;
            } else if (ins->b != None) {
                key = h.key(code.names[ins->b]);
                name_key = &key;
            }
            Context cx = frame_context(frame);
            std::optional<Value> const value = make_closure(*code.functions[ins->a], cx, name_key, (ins->flags & 1) == 0);
            if (!value) {
                VM_FAIL;
            }
            if (dynamic)
                frame.stack.pop_back();
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(LoadFieldKey):
            frame.push(frame.field_key);
            VM_NEXT;
        VM_CASE(ClassScope):
        VM_CASE(ClassScopeNamedDyn): {
            if (!env_room()) {
                VM_FAIL;
            }
            // The class's scope goes on the frame: its environment is the
            // lexical one until ClassFinish, its body strict.
            bool const dynamic = ins->op == Opcode::ClassScopeNamedDyn;
            PropertyKey key;
            PropertyKey const* name_key = nullptr;
            if (dynamic) {
                key = key_from_value(h, frame.top());
                name_key = &key;
            } else if (ins->b != None) {
                key = h.key(code.names[ins->b]);
                name_key = &key;
            }
            ClassBuilder* builder
                = class_scope(*code.classes[ins->a], frame.envs.back(), frame.private_environment, frame.strict, name_key, (ins->flags & 1) == 0);
            if (dynamic)
                frame.stack.pop_back();
            frame.builders.push_back(builder);
            frame.envs.push_back(builder->class_env);
            frame.strict = true;
            VM_NEXT;
        }
        VM_CASE(ClassBegin):
        VM_CASE(ClassBeginHeritage): {
            ClassBuilder& builder = *frame.builders.back();
            Value heritage;
            if (ins->op == Opcode::ClassBeginHeritage)
                heritage = frame.pop();
            if (!class_begin(builder, ins->op == Opcode::ClassBeginHeritage ? &heritage : nullptr)) {
                VM_FAIL;
            }
            frame.private_environment = builder.private_env; // the class's names, for its keys and bodies
            VM_NEXT;
        }
        VM_CASE(ClassElement):
        VM_CASE(ClassElementKeyed): {
            ClassBuilder& builder = *frame.builders.back();
            Value key_value;
            if (ins->op == Opcode::ClassElementKeyed)
                key_value = frame.pop();
            if (!class_element(builder, ins->a, ins->op == Opcode::ClassElementKeyed ? &key_value : nullptr))
                VM_FAIL;
            VM_NEXT;
        }
        VM_CASE(ClassFinish): {
            ClassBuilder& builder = *frame.builders.back();
            std::optional<Value> const value = class_finish(builder);
            frame.envs.pop_back();
            frame.private_environment = builder.outer_private;
            frame.strict = builder.saved_strict;
            frame.builders.pop_back();
            if (!value) {
                VM_FAIL;
            }
            frame.push(*value);
            VM_NEXT;
        }
        VM_CASE(AppendToReg): {
            auto* array = static_cast<ArrayObject*>(frame.registers[ins->a].as_object());
            array->push(frame.top());
            frame.stack.pop_back();
            VM_NEXT;
        }

        // ---- iteration
        VM_CASE(GetIterator):
        VM_CASE(GetAsyncIterator): {
            std::optional<IteratorRecord> const record
                = ins->op == Opcode::GetIterator ? self.get_iterator(frame.top()) : self.get_async_iterator(frame.top());
            if (!record) {
                VM_FAIL;
            }
            frame.top() = record->iterator;
            frame.push(record->next_method);
            VM_NEXT;
        }
        VM_CASE(IteratorNextCall): {
            // The async protocol's step: next() called, its answer pushed
            // for the Await that follows; the object check comes after.
            std::optional<Value> const result = self.call(frame.registers[ins->a + 1], frame.registers[ins->a], {});
            if (!result) {
                VM_FAIL;
            }
            frame.push(*result);
            VM_NEXT;
        }
        VM_CASE(IteratorReturnCall):
        VM_CASE(IteratorReturnCallQuiet): {
            // AsyncIteratorClose's first half: return() called when there
            // is one, its answer pushed for an Await; Empty when there is
            // none. The quiet form runs under a pending throw, which wins
            // over anything return() does.
            Value const iterator = frame.registers[ins->a];
            bool const quiet = ins->op == Opcode::IteratorReturnCallQuiet;
            Value pending;
            if (quiet)
                pending = frame.top();
            std::optional<Value> const method = self.get_method(iterator, self.key("return"));
            std::optional<Value> result;
            if (method && !method->is_undefined())
                result = self.call(*method, iterator, {});
            if ((!method || (!method->is_undefined() && !result))) {
                if (!quiet || self.m_terminated) {
                    VM_FAIL;
                }
                self.take_exception();
                frame.push(Value::empty());
                VM_NEXT;
            }
            frame.push(method->is_undefined() ? Value::empty() : *result);
            VM_NEXT;
        }
        VM_CASE(RequireIterResult): {
            if (!frame.top().is_object()) {
                self.throw_type_error("Iterator result " + self.describe(frame.top()) + " is not an object");
                VM_FAIL;
            }
            VM_NEXT;
        }
        VM_CASE(IteratorNext): {
            std::optional<Value> const result = self.call(frame.registers[ins->a + 1], frame.registers[ins->a], {});
            if (!result) {
                VM_FAIL;
            }
            if (!result->is_object()) {
                self.throw_type_error("Iterator result " + self.describe(*result) + " is not an object");
                VM_FAIL;
            }
            frame.push(*result);
            VM_NEXT;
        }
        VM_CASE(IteratorResultDone): {
            std::optional<Value> const done = iter_result_field(frame.top(), well_known.done);
            if (!done) {
                VM_FAIL;
            }
            frame.top() = Value::boolean(to_boolean(*done));
            VM_NEXT;
        }
        VM_CASE(IteratorResultValue): {
            std::optional<Value> const value = iter_result_field(frame.top(), well_known.value);
            if (!value) {
                VM_FAIL;
            }
            frame.top() = *value;
            VM_NEXT;
        }
        VM_CASE(IteratorStep): {
            // IteratorStepValue for a pattern: undefined once the iterator
            // is done; the iterator's own throw marks it done unclosed.
            if (iterator_done(ins->a)) {
                frame.push(Value::undefined());
                VM_NEXT;
            }
            IteratorRecord record = iterator_record(ins->a);
            Value out;
            std::optional<bool> const stepped = self.iterator_step(record, out);
            if (!stepped) {
                frame.registers[ins->a + 2] = Value::boolean(true);
                VM_FAIL;
            }
            if (!*stepped) {
                frame.registers[ins->a + 2] = Value::boolean(true);
                frame.push(Value::undefined());
                VM_NEXT;
            }
            frame.push(out);
            VM_NEXT;
        }
        VM_CASE(IteratorRestArray): {
            ArrayObject* rest = self.new_array();
            Roots const roots(self);
            self.root(Value::object(rest));
            while (!iterator_done(ins->a)) {
                IteratorRecord record = iterator_record(ins->a);
                Value out;
                std::optional<bool> const stepped = self.iterator_step(record, out);
                if (!stepped) {
                    frame.registers[ins->a + 2] = Value::boolean(true);
                    VM_FAIL;
                }
                if (!*stepped) {
                    frame.registers[ins->a + 2] = Value::boolean(true);
                    break;
                }
                rest->push(out);
            }
            frame.push(Value::object(rest));
            VM_NEXT;
        }
        VM_CASE(IteratorClose): {
            // IteratorClose on a normal exit: return() runs, and its own
            // failure is the outcome.
            if (iterator_done(ins->a))
                VM_NEXT;
            IteratorRecord const record = iterator_record(ins->a);
            if (!self.iterator_close(record, false))
                VM_FAIL;
            VM_NEXT;
        }
        VM_CASE(IteratorCloseThrowing): {
            // IteratorClose with a throw pending — the thrown value is on the
            // stack for the Throw that follows; it stays the outcome whatever
            // return() does.
            if (iterator_done(ins->a))
                VM_NEXT;
            self.throw_value(frame.top());
            IteratorRecord const record = iterator_record(ins->a);
            self.iterator_close(record, true);
            self.throw_value(frame.top());
            VM_NEXT;
        }
        VM_CASE(ForInStart): {
            // §14.7.5.6 enumerate: nothing to walk for null or undefined.
            Value const& subject = frame.top();
            Object* object = nullptr;
            if (!subject.is_nullish()) {
                std::optional<Object*> const boxed = self.to_object(subject);
                if (!boxed) {
                    VM_FAIL;
                }
                object = *boxed;
            }
            Roots const roots(self);
            if (object)
                self.root(Value::object(object));
            auto* enumerator = h.allocate<ForInIteratorObject>(object);
            frame.top() = Value::object(enumerator);
            if (!enumerator_load(enumerator->enumerator()))
                VM_FAIL;
            VM_NEXT;
        }
        VM_CASE(ForInNext): {
            auto* enumerator = static_cast<ForInIteratorObject*>(frame.registers[ins->a].as_object());
            JsString* key = enumerator_next(enumerator->enumerator());
            if (key == nullptr && self.has_exception()) {
                VM_FAIL;
            }
            frame.push(key ? Value::string(key) : Value::empty());
            VM_NEXT;
        }

        // ---- suspension
        VM_CASE(Yield):
            frame.result = frame.pop();
            frame.result_is_iter_result = (ins->flags & 1) != 0;
            frame.resume_pending = true;
            return RunStatus::Yielded;
        VM_CASE(Await):
            frame.result = frame.pop();
            frame.resume_pending = true;
            return RunStatus::Awaiting;

        // ---- modules
        VM_CASE(ImportCall): {
            // The specifier sits below the options; both stay on the
            // stack — and so traced — until the promise replaces them.
            Value const specifier = frame.peek(1);
            Value const options = frame.peek(0);
            std::optional<Value> const promise = self.perform_import_call(frame.program, specifier, options);
            if (!promise) {
                VM_FAIL;
            }
            frame.pop();
            frame.top() = *promise;
            VM_NEXT;
        }
        VM_CASE(ImportMeta): {
            std::optional<Value> const meta = self.import_meta_for(frame.program);
            if (!meta) {
                VM_FAIL;
            }
            frame.push(*meta);
            VM_NEXT;
        }
        VM_CASE(Nop):
            VM_NEXT;
        }
        continue;
    vm_fail:
        if (!vm_unwind(frame))
            return RunStatus::Threw;
    }
#undef VM_CASE
#undef VM_NEXT
#undef VM_FAIL
}
#if SASHFOLD_VM_THREADED
#pragma GCC diagnostic pop
#endif

// ---- generators -------------------------------------------------------------

// EvaluateGeneratorBody (§15.5.2): the arguments are bound (the caller
// did that), the generator object is made with the function's own
// `prototype` — or the intrinsic when that is not an object — and
// nothing of the body runs until the first next().
std::optional<Value> Interpreter::Impl::start_generator(ScriptFunction& function, Context const& cx, Frame* prepared)
{
    if (prepared != nullptr) {
        // The prologue runs first (EvaluateGeneratorBody step 1), to the
        // Suspend that ends it, on the frame the call pushed; the frame is
        // then copied out and kept by the object, made with the intrinsic
        // prototype at once and given the function's own `prototype` after
        // (step 2 may run a getter).
        if (vm_run(*prepared) != RunStatus::Yielded) {
            pop_frame(*prepared);
            return std::nullopt;
        }
        Roots const roots(self);
        GeneratorObject* generator = nullptr;
        {
            Heap::NoCollect const guard(heap());
            SavedFrame* saved = save_and_pop(*prepared, nullptr);
            generator = heap().allocate<GeneratorObject>(self.intrinsics().generator_prototype, saved);
        }
        self.root(Value::object(generator));
        std::optional<Object*> const prototype = self.get_prototype_from_constructor(&function, &Intrinsics::generator_prototype);
        if (!prototype)
            return std::nullopt;
        generator->set_prototype(*prototype);
        return Value::object(generator);
    }
    CodeBlock const* code = compiled_body(function.node());
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    std::optional<Object*> const prototype = self.get_prototype_from_constructor(&function, &Intrinsics::generator_prototype);
    if (!prototype)
        return std::nullopt;
    self.root(Value::object(*prototype));
    Heap::NoCollect const guard(heap());
    SavedFrame* saved = fresh_saved_frame(*code, cx);
    GeneratorObject* generator = heap().allocate<GeneratorObject>(*prototype, saved);
    return Value::object(generator);
}

// GeneratorResume and GeneratorResumeAbrupt (§27.5.3.3, §27.5.3.4): the
// body runs to its next yield, its end or a throw; what it produces is
// wrapped as an iterator result unless a yield* handed one through.
std::optional<Value> Interpreter::Impl::generator_resume(GeneratorObject& generator, ResumeKind kind, Value const& value)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    self.root(value);
    switch (generator.state()) {
    case GeneratorObject::State::Executing:
        return self.throw_type_error("Generator is already running");
    case GeneratorObject::State::Completed:
        if (kind == ResumeKind::Throw)
            return self.throw_value(value);
        return Value::object(self.create_iter_result(kind == ResumeKind::Return ? value : Value::undefined(), true));
    case GeneratorObject::State::SuspendedStart:
        if (kind == ResumeKind::Return) {
            generator.set_state(GeneratorObject::State::Completed);
            generator.release_frame();
            return Value::object(self.create_iter_result(value, true));
        }
        if (kind == ResumeKind::Throw) {
            generator.set_state(GeneratorObject::State::Completed);
            generator.release_frame();
            return self.throw_value(value);
        }
        break;
    case GeneratorObject::State::SuspendedYield:
        break;
    }
    // The body runs in its function's realm, and so is the result it
    // yields made (GeneratorResume enters the generator's own context).
    SavedFrame& saved = *generator.frame();
    RealmScope const realm_scope(self, saved.state.function ? saved.state.function->realm() : nullptr, RealmScope::Code::Script);
    saved.state.resume_kind = kind;
    saved.state.resume_value = value;
    // Back on the stacks to run; a full stack is the RangeError of a deep
    // recursion, and the generator stays as it was.
    Frame* frame = restore_frame(saved);
    if (frame == nullptr)
        return std::nullopt;
    generator.set_state(GeneratorObject::State::Executing);
    RunStatus const status = vm_run(*frame);
    switch (status) {
    case RunStatus::Yielded: {
        generator.set_state(GeneratorObject::State::SuspendedYield);
        Value const result = frame->result;
        bool const iter_result = frame->result_is_iter_result;
        frame->result = Value::empty();
        self.root(result);
        save_and_pop(*frame, &saved);
        if (iter_result)
            return result;
        return Value::object(self.create_iter_result(result, false));
    }
    case RunStatus::Completed: {
        generator.set_state(GeneratorObject::State::Completed);
        Value const result = frame->result;
        self.root(result);
        pop_frame(*frame);
        generator.release_frame();
        return Value::object(self.create_iter_result(result, true));
    }
    case RunStatus::Threw:
        generator.set_state(GeneratorObject::State::Completed);
        pop_frame(*frame);
        generator.release_frame();
        return std::nullopt;
    case RunStatus::Awaiting:
    case RunStatus::Switched: // never leaves vm_run
        break;
    }
    pop_frame(*frame);
    generator.set_state(GeneratorObject::State::Completed);
    generator.release_frame();
    return self.throw_syntax_error("await inside a generator body");
}

// ---- async functions ----------------------------------------------------------

// [[Call]] of an async function (§15.8.4 EvaluateAsyncFunctionBody): the
// promise capability comes first, so a throw while the parameters are
// bound rejects the promise rather than propagating.
std::optional<Value> Interpreter::Impl::call_async_function(ScriptFunction& function, Value const& this_argument, std::span<Value const> arguments)
{
    Roots const roots(self);
    std::optional<PromiseCapability> const capability = new_promise_capability(self, Value::object(self.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    self.root(capability->promise);
    self.root(capability->resolve);
    self.root(capability->reject);
    std::optional<Value> const result = run_script_function(function, this_argument, arguments, nullptr, nullptr, &*capability);
    if (result)
        return *result;
    if (self.m_terminated)
        return std::nullopt;
    Value const thrown = self.take_exception();
    self.root(thrown);
    Value const reject_arguments[1] = { thrown };
    if (!self.call(capability->reject, Value::undefined(), reject_arguments))
        return std::nullopt;
    return capability->promise;
}

// AsyncFunctionStart (§27.7.5.1) and AsyncBlockStart (§27.7.5.2): the body
// runs to its first await, its end or a throw.
std::optional<Value> Interpreter::Impl::start_async(FunctionNode const& node, Context const& cx, PromiseCapability const& capability, Frame* prepared)
{
    CodeBlock const* code = prepared ? prepared->code : compiled_body(node);
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    AsyncContextObject* context = nullptr;
    {
        Heap::NoCollect const guard(heap());
        // A body the call put on the stacks runs there first; one that never
        // ran (a module's) is saved from the start and restored.
        SavedFrame* saved = prepared ? nullptr : fresh_saved_frame(*code, cx);
        context = heap().allocate<AsyncContextObject>(nullptr, saved, capability.promise, capability.resolve, capability.reject);
    }
    self.root(Value::object(context));
    // A prologue of the body's own runs in this first step, before the
    // first await.
    async_step(*context, prepared);
    return capability.promise;
}

// One run of an async body (§27.7.5.1 step 4's closure, and §27.7.5.3
// Await): a completion settles the promise; an await hooks the frame's
// resumption onto the awaited value's promise with reactions that carry
// no capability of their own, and returns to the caller.
void Interpreter::Impl::async_step(AsyncContextObject& context, Frame* running)
{
    Roots const roots(self);
    self.root(Value::object(&context));
    // The settlement of the async body's promise: a throw rejects it.
    auto reject_with_pending = [&] {
        if (self.m_terminated)
            return;
        Value const thrown = self.take_exception();
        self.root(thrown);
        Value const reject_arguments[1] = { thrown };
        self.call(context.reject(), Value::undefined(), reject_arguments);
    };
    Frame* frame = running;
    if (frame == nullptr) {
        SavedFrame* saved = context.frame();
        if (saved == nullptr)
            return;
        frame = restore_frame(*saved);
        if (frame == nullptr) {
            context.release_frame();
            reject_with_pending();
            return;
        }
    }
    RealmScope const realm_scope(self, frame->function ? frame->function->realm() : nullptr, RealmScope::Code::Script);
    while (true) {
        RunStatus const status = vm_run(*frame);
        // The call's arguments were for its prologue, which has run: they
        // are the caller's, and the caller has returned by the next step.
        frame->incoming = {};
        if (status == RunStatus::Completed || status == RunStatus::Yielded) {
            Value const result = frame->result;
            self.root(result);
            pop_frame(*frame);
            context.release_frame();
            if (status == RunStatus::Yielded) {
                self.throw_syntax_error("yield inside an async function body");
                reject_with_pending();
                return;
            }
            Value const resolve_arguments[1] = { result };
            self.call(context.resolve(), Value::undefined(), resolve_arguments);
            return;
        }
        if (status == RunStatus::Threw) {
            pop_frame(*frame);
            context.release_frame();
            reject_with_pending();
            return;
        }
        // Awaiting.
        Value const awaited = frame->result;
        frame->result = Value::empty();
        self.root(awaited);
        std::optional<Value> const promise = promise_resolve(self, Value::object(self.intrinsics().promise_constructor), awaited);
        if (!promise) {
            // PromiseResolve threw (a `constructor` getter): that is the
            // await's own throw.
            if (self.m_terminated) {
                pop_frame(*frame);
                context.release_frame();
                return;
            }
            frame->resume_kind = ResumeKind::Throw;
            frame->resume_value = self.take_exception();
            continue;
        }
        self.root(*promise);
        auto resume = [](ResumeKind kind) {
            return [kind](Interpreter& in, ClosureFunction& self_function, Value const&, Args arguments) -> std::optional<Value> {
                auto* async_context = static_cast<AsyncContextObject*>(self_function.slot(0).as_object());
                if (SavedFrame* resumed = async_context->frame()) {
                    resumed->state.resume_kind = kind;
                    resumed->state.resume_value = argument(arguments, 0);
                    in.impl().async_step(*async_context);
                }
                return Value::undefined();
            };
        };
        ClosureFunction* on_fulfilled = self.new_closure("", 1, { Value::object(&context) }, resume(ResumeKind::Normal));
        self.root(Value::object(on_fulfilled));
        ClosureFunction* on_rejected = self.new_closure("", 1, { Value::object(&context) }, resume(ResumeKind::Throw));
        self.root(Value::object(on_rejected));
        perform_then(self, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
        // Off the stacks until a reaction resumes it.
        Heap::NoCollect const guard(heap());
        context.set_frame(save_and_pop(*frame, context.frame()));
        return;
    }
}

// ---- async generators (§27.6) ------------------------------------------------

std::optional<Value> Interpreter::Impl::start_async_generator(ScriptFunction& function, Context const& cx, Frame* prepared)
{
    // §27.6.3.2 AsyncGeneratorStart: the frame waits for the first request,
    // once a prologue of the body's own has run to its Suspend on the frame
    // the call pushed.
    if (prepared != nullptr) {
        if (vm_run(*prepared) != RunStatus::Yielded) {
            pop_frame(*prepared);
            return std::nullopt;
        }
        Roots const roots(self);
        AsyncGeneratorObject* generator = nullptr;
        {
            Heap::NoCollect const guard(heap());
            SavedFrame* saved = save_and_pop(*prepared, nullptr);
            generator = heap().allocate<AsyncGeneratorObject>(self.intrinsics().async_generator_prototype, saved);
        }
        self.root(Value::object(generator));
        std::optional<Object*> const prototype = self.get_prototype_from_constructor(&function, &Intrinsics::async_generator_prototype);
        if (!prototype)
            return std::nullopt;
        generator->set_prototype(*prototype);
        return Value::object(generator);
    }
    CodeBlock const* code = compiled_body(function.node());
    if (code == nullptr)
        return std::nullopt;
    Roots const roots(self);
    std::optional<Object*> const prototype = self.get_prototype_from_constructor(&function, &Intrinsics::async_generator_prototype);
    if (!prototype)
        return std::nullopt;
    self.root(Value::object(*prototype));
    Heap::NoCollect const guard(heap());
    SavedFrame* saved = fresh_saved_frame(*code, cx);
    auto* generator = heap().allocate<AsyncGeneratorObject>(*prototype, saved);
    return Value::object(generator);
}

// AsyncGeneratorCompleteStep (§27.6.3.3): the front request settled, with
// an iterator result or the throw.
void Interpreter::Impl::async_generator_complete_step(AsyncGeneratorObject& generator, ResumeKind kind, Value const& value, bool done)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    self.root(value);
    AsyncGeneratorObject::Request const request = generator.queue().front();
    generator.queue().pop_front();
    self.root(request.capability.promise);
    self.root(request.capability.resolve);
    self.root(request.capability.reject);
    if (kind == ResumeKind::Throw) {
        Value const arguments[1] = { value };
        self.call(request.capability.reject, Value::undefined(), arguments);
        return;
    }
    Object* result = self.create_iter_result(value, done);
    Value const arguments[1] = { Value::object(result) };
    self.call(request.capability.resolve, Value::undefined(), arguments);
}

// AsyncGeneratorAwaitReturn (§27.6.3.7): a return request's value is
// awaited, then the generator completes with it — or with its rejection.
void Interpreter::Impl::async_generator_await_return(AsyncGeneratorObject& generator)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    Value const value = generator.queue().front().value;
    self.root(value);
    std::optional<Value> const promise = promise_resolve(self, Value::object(self.intrinsics().promise_constructor), value);
    if (!promise) {
        if (self.m_terminated)
            return;
        generator.set_state(AsyncGeneratorObject::State::Completed);
        Value const thrown = self.take_exception();
        self.root(thrown);
        async_generator_complete_step(generator, ResumeKind::Throw, thrown, true);
        async_generator_drain_queue(generator);
        return;
    }
    self.root(*promise);
    auto settle = [](ResumeKind kind) {
        return [kind](Interpreter& in, ClosureFunction& self_function, Value const&, Args arguments) -> std::optional<Value> {
            auto* target = static_cast<AsyncGeneratorObject*>(self_function.slot(0).as_object());
            target->set_state(AsyncGeneratorObject::State::Completed);
            in.impl().async_generator_complete_step(*target, kind, argument(arguments, 0), true);
            in.impl().async_generator_drain_queue(*target);
            return Value::undefined();
        };
    };
    ClosureFunction* on_fulfilled = self.new_closure("", 1, { Value::object(&generator) }, settle(ResumeKind::Normal));
    self.root(Value::object(on_fulfilled));
    ClosureFunction* on_rejected = self.new_closure("", 1, { Value::object(&generator) }, settle(ResumeKind::Throw));
    self.root(Value::object(on_rejected));
    perform_then(self, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
}

// AsyncGeneratorDrainQueue (§27.6.3.8): once the body is done, every
// waiting request is answered — done, with its throw, or, for a return,
// after its value has been awaited (which stops the drain until then).
void Interpreter::Impl::async_generator_drain_queue(AsyncGeneratorObject& generator)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    while (!generator.queue().empty()) {
        AsyncGeneratorObject::Request const& request = generator.queue().front();
        if (request.kind == ResumeKind::Return) {
            generator.set_state(AsyncGeneratorObject::State::AwaitingReturn);
            async_generator_await_return(generator);
            return;
        }
        ResumeKind const kind = request.kind;
        Value const value = request.value;
        self.root(value);
        async_generator_complete_step(generator, kind, kind == ResumeKind::Throw ? value : Value::undefined(), true);
    }
}

// The body run to its next yield, its end, a throw or an await
// (AsyncGeneratorStart's closure, §27.6.3.2; AsyncGeneratorYield's
// completing and immediate resumption, §27.6.3.8; Await, §27.7.5.3).
void Interpreter::Impl::async_generator_step(AsyncGeneratorObject& generator)
{
    Roots const roots(self);
    self.root(Value::object(&generator));
    SavedFrame* saved = generator.frame();
    if (saved == nullptr)
        return;
    // A completion of the body by a throw: the front request is answered
    // with it, and the rest drained.
    auto complete_with_pending = [&] {
        if (self.m_terminated)
            return;
        Value const thrown = self.take_exception();
        self.root(thrown);
        async_generator_complete_step(generator, ResumeKind::Throw, thrown, true);
        async_generator_drain_queue(generator);
    };
    Frame* frame = restore_frame(*saved);
    if (frame == nullptr) {
        generator.set_state(AsyncGeneratorObject::State::Completed);
        generator.release_frame();
        complete_with_pending();
        return;
    }
    RealmScope const realm_scope(self, frame->function ? frame->function->realm() : nullptr, RealmScope::Code::Script);
    while (true) {
        RunStatus const status = vm_run(*frame);
        if (status == RunStatus::Yielded) {
            Value const value = frame->result;
            frame->result = Value::empty();
            self.root(value);
            async_generator_complete_step(generator, ResumeKind::Normal, value, false);
            if (generator.queue().empty()) {
                generator.set_state(AsyncGeneratorObject::State::SuspendedYield);
                save_and_pop(*frame, saved);
                return;
            }
            // A request that arrived while the body ran resumes it at once
            // (the resumption's return value is awaited by the body itself).
            AsyncGeneratorObject::Request const& next = generator.queue().front();
            frame->resume_kind = next.kind;
            frame->resume_value = next.value;
            continue;
        }
        if (status == RunStatus::Completed) {
            generator.set_state(AsyncGeneratorObject::State::Completed);
            Value const result = frame->result;
            self.root(result);
            pop_frame(*frame);
            generator.release_frame();
            async_generator_complete_step(generator, ResumeKind::Normal, result, true);
            async_generator_drain_queue(generator);
            return;
        }
        if (status == RunStatus::Threw) {
            generator.set_state(AsyncGeneratorObject::State::Completed);
            pop_frame(*frame);
            generator.release_frame();
            complete_with_pending();
            return;
        }
        // Awaiting, as an async function does.
        Value const awaited = frame->result;
        frame->result = Value::empty();
        self.root(awaited);
        std::optional<Value> const promise = promise_resolve(self, Value::object(self.intrinsics().promise_constructor), awaited);
        if (!promise) {
            if (self.m_terminated) {
                generator.set_state(AsyncGeneratorObject::State::Completed);
                pop_frame(*frame);
                generator.release_frame();
                return;
            }
            frame->resume_kind = ResumeKind::Throw;
            frame->resume_value = self.take_exception();
            continue;
        }
        self.root(*promise);
        auto resume = [](ResumeKind kind) {
            return [kind](Interpreter& in, ClosureFunction& self_function, Value const&, Args arguments) -> std::optional<Value> {
                auto* target = static_cast<AsyncGeneratorObject*>(self_function.slot(0).as_object());
                if (SavedFrame* resumed = target->frame()) {
                    resumed->state.resume_kind = kind;
                    resumed->state.resume_value = argument(arguments, 0);
                    in.impl().async_generator_step(*target);
                }
                return Value::undefined();
            };
        };
        ClosureFunction* on_fulfilled = self.new_closure("", 1, { Value::object(&generator) }, resume(ResumeKind::Normal));
        self.root(Value::object(on_fulfilled));
        ClosureFunction* on_rejected = self.new_closure("", 1, { Value::object(&generator) }, resume(ResumeKind::Throw));
        self.root(Value::object(on_rejected));
        perform_then(self, *static_cast<PromiseObject*>(promise->as_object()), Value::object(on_fulfilled), Value::object(on_rejected), std::nullopt);
        // Off the stacks until a reaction resumes it.
        save_and_pop(*frame, saved);
        return;
    }
}

// AsyncGeneratorResume (§27.6.3.4).
void Interpreter::Impl::async_generator_resume(AsyncGeneratorObject& generator, ResumeKind kind, Value const& value)
{
    SavedFrame* saved = generator.frame();
    if (saved == nullptr)
        return;
    generator.set_state(AsyncGeneratorObject::State::Executing);
    saved->state.resume_kind = kind;
    saved->state.resume_value = value;
    async_generator_step(generator);
}

// %AsyncGeneratorPrototype%.next, return and throw past their validation
// (§27.6.1.2–.4): a generator that is done answers at once; otherwise the
// request is queued and, by the generator's state, run now, awaited now
// (a return before the body ever ran), or left for its turn.
std::optional<Value> Interpreter::Impl::async_generator_enqueue(AsyncGeneratorObject& generator, ResumeKind kind, Value const& value,
    PromiseCapability const& capability)
{
    using State = AsyncGeneratorObject::State;
    Roots const roots(self);
    self.root(Value::object(&generator));
    self.root(value);
    self.root(capability.promise);
    self.root(capability.resolve);
    self.root(capability.reject);
    State state = generator.state();
    if (kind == ResumeKind::Normal && state == State::Completed) {
        Object* result = self.create_iter_result(Value::undefined(), true);
        Value const arguments[1] = { Value::object(result) };
        if (!self.call(capability.resolve, Value::undefined(), arguments))
            return std::nullopt;
        return capability.promise;
    }
    if (kind == ResumeKind::Throw && state == State::SuspendedStart) {
        generator.set_state(State::Completed);
        generator.release_frame();
        state = State::Completed;
    }
    if (kind == ResumeKind::Throw && state == State::Completed) {
        Value const arguments[1] = { value };
        if (!self.call(capability.reject, Value::undefined(), arguments))
            return std::nullopt;
        return capability.promise;
    }
    generator.queue().push_back(AsyncGeneratorObject::Request { kind, value, capability });
    if (kind == ResumeKind::Return && (state == State::SuspendedStart || state == State::Completed)) {
        generator.set_state(State::AwaitingReturn);
        generator.release_frame();
        async_generator_await_return(generator);
    } else if (state == State::SuspendedStart || state == State::SuspendedYield) {
        async_generator_resume(generator, kind, value);
    }
    return capability.promise;
}

}
