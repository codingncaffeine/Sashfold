// Module records and the module namespace exotic object: §16.2.1.5–§16.2.1.10
// and §10.4.6, ported with the specification's names and step order. A
// module without a top-level await runs its body on the tree-walker under
// a context whose lexical and variable environments are both the module
// environment; one with a top-level await runs the same statements as an
// async function body on the bytecode tier, and its importers wait for
// it through the asynchronous half of InnerModuleEvaluation.

#include "js/Module.h"

#include "js/Evaluator.h"
#include "js/Runtime.h"
#include "js/Strings.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace sashfold::js {

namespace {

bool is_star(JsString const* name)
{
    return name != nullptr && name->view() == u"*";
}

// A name spelled as a canonical array index is an index key (§7.1.21),
// so the namespace's keys compare equal to what script writes.
std::optional<std::uint32_t> array_index_of(std::u16string_view text)
{
    if (text.empty() || text.size() > 10 || (text.size() > 1 && text[0] == u'0'))
        return std::nullopt;
    std::uint64_t value = 0;
    for (char16_t const c : text) {
        if (c < u'0' || c > u'9')
            return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - u'0');
    }
    if (value > 0xFFFFFFFEu)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

PropertyKey key_for_export(JsString* name)
{
    if (std::optional<std::uint32_t> const index = array_index_of(name->view()))
        return PropertyKey::index(*index);
    return PropertyKey::atom(name);
}

std::u16string index_text(std::uint32_t index)
{
    std::string const digits = std::to_string(index);
    return std::u16string(digits.begin(), digits.end());
}

} // namespace

// ---- ModuleRecord -----------------------------------------------------------

ModuleRecord::ModuleRecord(std::string key, std::unique_ptr<Program> program)
    : m_key(std::move(key))
    , m_program(std::move(program))
{
}

void ModuleRecord::add_loaded_module(JsString* specifier, ModuleRecord* module)
{
    m_loaded_modules.emplace_back(specifier, module);
}

ModuleRecord* ModuleRecord::imported_module(JsString* specifier) const
{
    for (auto const& [loaded_specifier, module] : m_loaded_modules) {
        if (loaded_specifier == specifier)
            return module;
    }
    return nullptr;
}

// GetExportedNames (§16.2.1.6.2): the local and indirect names, then
// every star-exported module's names but "default", a circular star walk
// contributing nothing the second time round.
std::vector<JsString*> ModuleRecord::exported_names(std::vector<ModuleRecord*>& export_star_set)
{
    std::vector<JsString*> names;
    if (std::find(export_star_set.begin(), export_star_set.end(), this) != export_star_set.end())
        return names;
    export_star_set.push_back(this);
    for (ExportEntryRecord const& entry : m_program->local_export_entries)
        names.push_back(entry.export_name);
    for (ExportEntryRecord const& entry : m_program->indirect_export_entries)
        names.push_back(entry.export_name);
    for (ExportEntryRecord const& entry : m_program->star_export_entries) {
        ModuleRecord* requested = imported_module(entry.module_request);
        if (requested == nullptr)
            continue;
        for (JsString* name : requested->exported_names(export_star_set)) {
            if (name->view() == u"default")
                continue;
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
    }
    return names;
}

// ResolveExport (§16.2.1.6.3).
ExportResolution ModuleRecord::resolve_export(JsString* export_name, std::vector<std::pair<ModuleRecord*, JsString*>>& resolve_set)
{
    ExportResolution resolution;
    for (auto const& [module, name] : resolve_set) {
        if (module == this && name == export_name)
            return resolution; // a circular import request: not found
    }
    resolve_set.emplace_back(this, export_name);
    for (ExportEntryRecord const& entry : m_program->local_export_entries) {
        if (entry.export_name == export_name) {
            resolution.kind = ExportResolution::Kind::Binding;
            resolution.module = this;
            resolution.binding_name = entry.local_name;
            return resolution;
        }
    }
    for (ExportEntryRecord const& entry : m_program->indirect_export_entries) {
        if (entry.export_name != export_name)
            continue;
        ModuleRecord* imported = imported_module(entry.module_request);
        if (imported == nullptr)
            return resolution;
        if (is_star(entry.import_name)) {
            // `export * as ns from 'm'`: the namespace of m.
            resolution.kind = ExportResolution::Kind::Binding;
            resolution.module = imported;
            resolution.binding_name = nullptr;
            return resolution;
        }
        return imported->resolve_export(entry.import_name, resolve_set);
    }
    if (export_name->view() == u"default")
        return resolution; // a default export is never provided by `export *`
    ExportResolution star_resolution;
    for (ExportEntryRecord const& entry : m_program->star_export_entries) {
        ModuleRecord* imported = imported_module(entry.module_request);
        if (imported == nullptr)
            continue;
        ExportResolution const candidate = imported->resolve_export(export_name, resolve_set);
        if (candidate.kind == ExportResolution::Kind::Ambiguous)
            return candidate;
        if (candidate.kind != ExportResolution::Kind::Binding)
            continue;
        if (star_resolution.kind == ExportResolution::Kind::NotFound) {
            star_resolution = candidate;
            continue;
        }
        // Two star exports both provide the name: the same binding is
        // one export, anything else is ambiguous.
        if (candidate.module != star_resolution.module || candidate.binding_name != star_resolution.binding_name) {
            ExportResolution ambiguous;
            ambiguous.kind = ExportResolution::Kind::Ambiguous;
            return ambiguous;
        }
    }
    return star_resolution;
}

// GetModuleNamespace (§16.2.1.10) with ModuleNamespaceCreate (§10.4.6.12):
// the exported names that resolve unambiguously, sorted by code units.
Object* ModuleRecord::get_namespace(Interpreter& in)
{
    if (m_namespace != nullptr)
        return m_namespace;
    std::vector<ModuleRecord*> export_star_set;
    std::vector<JsString*> unambiguous;
    for (JsString* name : exported_names(export_star_set)) {
        std::vector<std::pair<ModuleRecord*, JsString*>> resolve_set;
        if (resolve_export(name, resolve_set).found())
            unambiguous.push_back(name);
    }
    std::sort(unambiguous.begin(), unambiguous.end(), [](JsString const* a, JsString const* b) { return a->view() < b->view(); });
    Heap::NoCollect const guard(in.heap());
    auto* object = in.heap().allocate<ModuleNamespaceObject>(this, std::move(unambiguous));
    object->put(PropertyKey::symbol(in.atoms().symbol_to_string_tag), Value::string(in.atom("Module")), frozen_attributes);
    object->prevent_extensions();
    m_namespace = object;
    return m_namespace;
}

// Link (§16.2.1.5.2): a failure anywhere unwinds every module still on
// the stack to unlinked.
bool ModuleRecord::link(Interpreter& in)
{
    std::vector<ModuleRecord*> stack;
    std::optional<std::size_t> const result = inner_linking(in, stack, 0);
    if (!result) {
        for (ModuleRecord* module : stack) {
            module->m_status = Status::Unlinked;
            module->m_dfs_index.reset();
            module->m_dfs_ancestor_index.reset();
        }
        return false;
    }
    return true;
}

// InnerModuleLinking (§16.2.1.5.2.1): Tarjan's strongly connected
// components over the requested modules, each initialised once its
// dependencies are, a whole cycle marked linked together.
std::optional<std::size_t> ModuleRecord::inner_linking(Interpreter& in, std::vector<ModuleRecord*>& stack, std::size_t index)
{
    if (m_status == Status::Linking || m_status == Status::Linked || m_status == Status::EvaluatingAsync || m_status == Status::Evaluated)
        return index;
    m_status = Status::Linking;
    m_dfs_index = index;
    m_dfs_ancestor_index = index;
    ++index;
    stack.push_back(this);
    for (ModuleRequest const& request : m_program->requested_modules) {
        ModuleRecord* required = imported_module(request.specifier);
        if (required == nullptr) {
            in.throw_type_error("Module '" + request.specifier->to_utf8() + "' was not loaded");
            return std::nullopt;
        }
        std::optional<std::size_t> const next = required->inner_linking(in, stack, index);
        if (!next)
            return std::nullopt;
        index = *next;
        if (required->m_status == Status::Linking)
            m_dfs_ancestor_index = std::min(*m_dfs_ancestor_index, *required->m_dfs_ancestor_index);
    }
    if (!initialize_environment(in))
        return std::nullopt;
    if (m_dfs_ancestor_index == m_dfs_index) {
        while (true) {
            ModuleRecord* module = stack.back();
            stack.pop_back();
            module->m_status = Status::Linked;
            if (module == this)
                break;
        }
    }
    return index;
}

// InitializeEnvironment (§16.2.1.6.4): every indirect export must
// resolve; then the module environment, its import bindings (a namespace
// import is an immutable binding holding the namespace, anything else an
// indirect binding into the exporting module), its vars, its lexicals in
// their dead zone, and its functions, instantiated now.
bool ModuleRecord::initialize_environment(Interpreter& in)
{
    auto unresolvable = [&](JsString* name, ExportResolution const& resolution) {
        std::string const text = name->to_utf8();
        if (resolution.kind == ExportResolution::Kind::Ambiguous)
            in.throw_syntax_error("The requested module contains conflicting star exports for name '" + text + "'");
        else
            in.throw_syntax_error("The requested module does not provide an export named '" + text + "'");
        return false;
    };
    for (ExportEntryRecord const& entry : m_program->indirect_export_entries) {
        std::vector<std::pair<ModuleRecord*, JsString*>> resolve_set;
        ExportResolution const resolution = resolve_export(entry.export_name, resolve_set);
        if (!resolution.found())
            return unresolvable(entry.export_name, resolution);
    }
    Interpreter::Impl& impl = in.impl();
    Heap::NoCollect const guard(in.heap());
    Environment* env = impl.new_environment(impl.global_lexical);
    // A module environment binds `this` as undefined (§9.1.1.5.4).
    env->set_this(Value::undefined());
    m_environment = env;
    for (ImportEntryRecord const& entry : m_program->import_entries) {
        ModuleRecord* imported = imported_module(entry.module_request);
        if (imported == nullptr) {
            in.throw_type_error("Module '" + entry.module_request->to_utf8() + "' was not loaded");
            return false;
        }
        if (is_star(entry.import_name)) {
            env->declare(entry.local_name, Value::object(imported->get_namespace(in)), false, true);
            continue;
        }
        std::vector<std::pair<ModuleRecord*, JsString*>> resolve_set;
        ExportResolution const resolution = imported->resolve_export(entry.import_name, resolve_set);
        if (!resolution.found())
            return unresolvable(entry.import_name, resolution);
        if (resolution.binding_name == nullptr)
            env->declare(entry.local_name, Value::object(resolution.module->get_namespace(in)), false, true);
        else
            env->declare_import(entry.local_name, resolution.module, resolution.binding_name);
    }
    for (JsString* name : m_program->declarations.vars)
        env->declare(name, Value::undefined(), true, true);
    for (auto const& [name, is_const] : m_program->declarations.lexicals)
        env->declare(name, Value::undefined(), !is_const, false);
    for (FunctionDeclaration const* declaration : m_program->declarations.functions) {
        FunctionNode const& node = *declaration->function;
        // An anonymous `export default function` binds `*default*` and is
        // named "default" (§16.2.3.7 InstantiateFunctionObject).
        JsString* name = node.name != nullptr ? node.name : in.atom("*default*");
        ScriptFunction* closure = in.new_script_function(node, env, nullptr);
        if (node.name == nullptr)
            closure->put(PropertyKey::atom(in.atoms().name), Value::string(in.atom("default")), Configurable);
        Environment::Binding& binding = env->declare(name, Value::object(closure), true, true);
        binding.value = Value::object(closure);
        binding.initialized = true;
    }
    return true;
}

// Evaluate (§16.2.1.5.3): the top-level promise, made once per cycle
// root; a failure marks every module still on the stack evaluated with
// the error and rejects the promise.
std::optional<Value> ModuleRecord::evaluate(Interpreter& in)
{
    ModuleRecord* module = this;
    if (module->m_status == Status::EvaluatingAsync || module->m_status == Status::Evaluated) {
        if (module->m_cycle_root != nullptr)
            module = module->m_cycle_root;
    } else if (module->m_status != Status::Linked) {
        return in.throw_type_error("Module '" + m_key + "' is not linked");
    }
    if (module->m_top_level_capability)
        return module->m_top_level_capability->promise;
    Interpreter::Roots const roots(in);
    std::optional<PromiseCapability> const capability = new_promise_capability(in, Value::object(in.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    module->m_top_level_capability = *capability;
    std::vector<ModuleRecord*> stack;
    std::optional<std::size_t> const result = module->inner_evaluation(in, stack, 0);
    if (!result) {
        if (in.terminated())
            return std::nullopt;
        Value const error = in.take_exception();
        in.root(error);
        for (ModuleRecord* m : stack) {
            m->m_status = Status::Evaluated;
            m->m_evaluation_error = error;
        }
        Value const reject_arguments[1] = { error };
        if (!in.call(capability->reject, Value::undefined(), reject_arguments))
            return std::nullopt;
        return capability->promise;
    }
    // A module evaluated by now — a graph with nothing asynchronous in it,
    // or one whose asynchronous evaluation finished under an earlier root
    // — settles the promise here; one still evaluating asynchronously
    // settles it from AsyncModuleExecutionFulfilled or -Rejected, which
    // read the capability when they run.
    if (module->m_status == Status::Evaluated) {
        Value const resolve_arguments[1] = { Value::undefined() };
        if (!in.call(capability->resolve, Value::undefined(), resolve_arguments))
            return std::nullopt;
    }
    return capability->promise;
}

// InnerModuleEvaluation (§16.2.1.5.3.1): the same walk as linking, each
// module's body run once its dependencies have run, a cycle finished
// together; an earlier failure is rethrown for every later request.
std::optional<std::size_t> ModuleRecord::inner_evaluation(Interpreter& in, std::vector<ModuleRecord*>& stack, std::size_t index)
{
    if (m_status == Status::EvaluatingAsync || m_status == Status::Evaluated) {
        if (m_evaluation_error.is_empty())
            return index;
        return in.throw_value(m_evaluation_error);
    }
    if (m_status == Status::Evaluating)
        return index;
    if (m_status != Status::Linked)
        return in.throw_type_error("Module '" + m_key + "' is not linked");
    m_status = Status::Evaluating;
    m_dfs_index = index;
    m_dfs_ancestor_index = index;
    m_pending_async_dependencies = 0;
    ++index;
    stack.push_back(this);
    for (ModuleRequest const& request : m_program->requested_modules) {
        ModuleRecord* required = imported_module(request.specifier);
        if (required == nullptr)
            return in.throw_type_error("Module '" + request.specifier->to_utf8() + "' was not loaded");
        std::optional<std::size_t> const next = required->inner_evaluation(in, stack, index);
        if (!next)
            return std::nullopt;
        index = *next;
        if (required->m_status == Status::Evaluating) {
            m_dfs_ancestor_index = std::min(*m_dfs_ancestor_index, *required->m_dfs_ancestor_index);
        } else {
            if (required->m_cycle_root != nullptr)
                required = required->m_cycle_root;
            if (!required->m_evaluation_error.is_empty())
                return in.throw_value(required->m_evaluation_error);
        }
        if (required->m_async_evaluation == AsyncEvaluation::Pending) {
            ++m_pending_async_dependencies;
            required->m_async_parent_modules.push_back(this);
        }
    }
    if (m_pending_async_dependencies > 0 || has_top_level_await()) {
        // The module evaluates asynchronously: ordered now against every
        // other pending module, started at once when nothing it needs is
        // still pending, else started by the last dependency to finish.
        m_async_evaluation = AsyncEvaluation::Pending;
        m_async_evaluation_order = in.impl().module_async_evaluation_count++;
        if (m_pending_async_dependencies == 0 && !execute_async_module(in))
            return std::nullopt;
    } else if (!execute_module(in)) {
        return std::nullopt;
    }
    if (m_dfs_ancestor_index == m_dfs_index) {
        while (true) {
            ModuleRecord* module = stack.back();
            stack.pop_back();
            module->m_status = module->m_async_evaluation == AsyncEvaluation::Unset ? Status::Evaluated : Status::EvaluatingAsync;
            module->m_cycle_root = this;
            if (module == this)
                break;
        }
    }
    return index;
}

// ExecuteModule (§16.2.1.6.5): the body under a context whose environments
// are both the module environment, strict, with no function and so no
// `this` of its own. Without a top-level await it runs to its end on the
// tree-walker; with one it is AsyncBlockStart (§27.7.5.2) over the same
// statements — the module is the one async function it contains, and it
// runs to its first await here and settles the capability when it ends.
// The environment already holds every declaration of the body
// (InitializeEnvironment did what a call's prologue does), so the function
// is the statement list and nothing else.
FunctionNode const& ModuleRecord::synthetic_body(bool is_async)
{
    FunctionNode const*& slot = is_async ? m_async_body : m_sync_body;
    if (slot == nullptr) {
        FunctionNode* body = m_program->make_function();
        body->is_async = is_async;
        body->is_strict = true;
        body->is_constructable = false;
        body->body = m_program->body;
        body->declarations = m_program->declarations;
        slot = body;
    }
    return *slot;
}

bool ModuleRecord::execute_module(Interpreter& in, PromiseCapability const* capability)
{
    Interpreter::Impl& impl = in.impl();
    Context const context { m_environment, m_environment, m_program.get(), nullptr, true, nullptr };
    if (!has_top_level_await()) {
        Interpreter::Impl::ContextScope scope(impl, context);
        if (in.bytecode_for_all())
            return impl.run_compiled_node(synthetic_body(false), scope.context()).has_value();
        Completion const completion = impl.execute_list(m_program->body, scope.context());
        return completion.type != Completion::Type::Throw;
    }
    if (capability == nullptr) {
        in.throw_type_error("internal: a module with a top-level await executed without a capability");
        return false;
    }
    return impl.start_async(synthetic_body(true), context, *capability).has_value();
}

// ExecuteAsyncModule (§16.2.1.5.3.2): a fresh capability for the body's
// own promise, the module's two continuations as its reactions, then the
// body started. The closures hold the record by pointer: the module map
// keeps every record for the realm's life, so nothing here dangles.
bool ModuleRecord::execute_async_module(Interpreter& in)
{
    Interpreter::Roots const roots(in);
    std::optional<PromiseCapability> const capability = new_promise_capability(in, Value::object(in.intrinsics().promise_constructor));
    if (!capability)
        return false;
    in.root(capability->promise);
    in.root(capability->resolve);
    in.root(capability->reject);
    ModuleRecord* const module = this;
    ClosureFunction* on_fulfilled = in.new_closure("", 0, {},
        [module](Interpreter& interpreter, ClosureFunction&, Value const&, std::span<Value const>) -> std::optional<Value> {
            if (!async_module_execution_fulfilled(interpreter, *module))
                return std::nullopt;
            return Value::undefined();
        });
    in.root(Value::object(on_fulfilled));
    ClosureFunction* on_rejected = in.new_closure("", 1, {},
        [module](Interpreter& interpreter, ClosureFunction&, Value const&, std::span<Value const> arguments) -> std::optional<Value> {
            Value const error = arguments.empty() ? Value::undefined() : arguments[0];
            if (!async_module_execution_rejected(interpreter, *module, error))
                return std::nullopt;
            return Value::undefined();
        });
    in.root(Value::object(on_rejected));
    perform_then(in, *static_cast<PromiseObject*>(capability->promise.as_object()), Value::object(on_fulfilled),
        Value::object(on_rejected), std::nullopt);
    return execute_module(in, &*capability);
}

// GatherAvailableAncestors (§16.2.1.5.3.3): every importer that was
// waiting on this module and now waits on nothing, and — through an
// importer with no await of its own, which will run synchronously — the
// importers waiting on that one, and so on up. An importer whose cycle
// already failed is left where it is.
void ModuleRecord::gather_available_ancestors(std::vector<ModuleRecord*>& exec_list)
{
    for (ModuleRecord* m : m_async_parent_modules) {
        if (std::find(exec_list.begin(), exec_list.end(), m) != exec_list.end())
            continue;
        ModuleRecord const* root = m->m_cycle_root != nullptr ? m->m_cycle_root : m;
        if (!root->m_evaluation_error.is_empty())
            continue;
        if (m->m_status != Status::EvaluatingAsync || m->m_pending_async_dependencies == 0)
            continue;
        --m->m_pending_async_dependencies;
        if (m->m_pending_async_dependencies == 0) {
            exec_list.push_back(m);
            if (!m->has_top_level_await())
                m->gather_available_ancestors(exec_list);
        }
    }
}

// AsyncModuleExecutionFulfilled (§16.2.1.5.3.4): the module is done and
// its own promise, when it is a root somebody asked to evaluate, settles;
// then every importer that waited only on it runs, in the order they were
// found to be asynchronous — a body with an await of its own started as
// an async one, any other run to its end here and finished likewise.
bool ModuleRecord::async_module_execution_fulfilled(Interpreter& in, ModuleRecord& module)
{
    if (module.m_status == Status::Evaluated)
        return true; // a rejection reached it first (§16.2.1.5.3.5 step 9)
    module.m_async_evaluation = AsyncEvaluation::Done;
    module.m_status = Status::Evaluated;
    if (module.m_top_level_capability) {
        Value const arguments[1] = { Value::undefined() };
        if (!in.call(module.m_top_level_capability->resolve, Value::undefined(), arguments))
            return false;
    }
    std::vector<ModuleRecord*> exec_list;
    module.gather_available_ancestors(exec_list);
    std::sort(exec_list.begin(), exec_list.end(),
        [](ModuleRecord const* a, ModuleRecord const* b) { return a->m_async_evaluation_order < b->m_async_evaluation_order; });
    for (ModuleRecord* m : exec_list) {
        if (m->m_status == Status::Evaluated)
            continue; // failed meanwhile, through an ancestor handled above
        bool started = false;
        if (m->has_top_level_await())
            started = m->execute_async_module(in);
        else
            started = m->execute_module(in);
        if (!started) {
            if (in.terminated())
                return false;
            Interpreter::Roots const roots(in);
            Value const error = in.take_exception();
            in.root(error);
            if (!async_module_execution_rejected(in, *m, error))
                return false;
            continue;
        }
        if (m->has_top_level_await())
            continue; // its own continuation will finish it
        m->m_async_evaluation = AsyncEvaluation::Done;
        m->m_status = Status::Evaluated;
        if (m->m_top_level_capability) {
            Value const arguments[1] = { Value::undefined() };
            if (!in.call(m->m_top_level_capability->resolve, Value::undefined(), arguments))
                return false;
        }
    }
    return true;
}

// AsyncModuleExecutionRejected (§16.2.1.5.3.5): the module fails with
// the error, its own promise first and then every importer waiting on
// it — leaf to root, as fulfilment settles them — and every later
// request for any of them answers the same error.
bool ModuleRecord::async_module_execution_rejected(Interpreter& in, ModuleRecord& module, Value const& error)
{
    if (module.m_status == Status::Evaluated)
        return true; // already failed through another dependency
    module.m_evaluation_error = error;
    module.m_status = Status::Evaluated;
    module.m_async_evaluation = AsyncEvaluation::Done;
    if (module.m_top_level_capability) {
        Value const arguments[1] = { error };
        if (!in.call(module.m_top_level_capability->reject, Value::undefined(), arguments))
            return false;
    }
    std::vector<ModuleRecord*> const parents = module.m_async_parent_modules;
    for (ModuleRecord* parent : parents) {
        if (!async_module_execution_rejected(in, *parent, error))
            return false;
    }
    return true;
}

void ModuleRecord::trace(Tracer& tracer)
{
    tracer.visit(m_environment);
    tracer.visit(m_namespace);
    tracer.visit(m_import_meta);
    tracer.visit(m_evaluation_error);
    if (m_top_level_capability) {
        tracer.visit(m_top_level_capability->promise);
        tracer.visit(m_top_level_capability->resolve);
        tracer.visit(m_top_level_capability->reject);
    }
    for (ModuleRecord* module : m_async_parent_modules)
        tracer.visit(module);
    tracer.visit(m_cycle_root);
    for (auto const& [specifier, module] : m_loaded_modules) {
        tracer.visit(specifier);
        tracer.visit(module);
    }
}

// ---- ModuleNamespaceObject -------------------------------------------------

ModuleNamespaceObject::ModuleNamespaceObject(ModuleRecord* module, std::vector<JsString*> exports)
    : Object(nullptr, Class::ModuleNamespace)
    , m_module(module)
    , m_exports(std::move(exports))
{
}

bool ModuleNamespaceObject::is_export(PropertyKey const& key) const
{
    if (key.is_atom()) {
        JsString const* name = key.as_atom();
        return std::find(m_exports.begin(), m_exports.end(), name) != m_exports.end();
    }
    if (key.is_index()) {
        std::u16string const text = index_text(key.as_index());
        return std::any_of(m_exports.begin(), m_exports.end(), [&](JsString const* name) { return name->view() == text; });
    }
    return false;
}

// The export's binding, read live (§10.4.6.8 steps 4–12): a namespace
// re-export answers the other module's namespace, a binding in its dead
// zone — or in a module whose environment does not exist yet — is a
// ReferenceError.
std::optional<Value> ModuleNamespaceObject::read_export(Interpreter& in, JsString* name)
{
    std::vector<std::pair<ModuleRecord*, JsString*>> resolve_set;
    ExportResolution const resolution = m_module->resolve_export(name, resolve_set);
    if (!resolution.found())
        return in.throw_reference_error(name->to_utf8() + " is not defined");
    if (resolution.binding_name == nullptr)
        return Value::object(resolution.module->get_namespace(in));
    ModuleRecord* target = resolution.module;
    JsString* binding_name = resolution.binding_name;
    // A local export of a binding that is itself imported (a namespace
    // import re-exported) is a plain binding; follow an indirect one all
    // the same, in case a host made one.
    for (int hops = 0; hops < 64; ++hops) {
        Environment* env = target->environment();
        if (env == nullptr)
            return in.throw_reference_error("Cannot access '" + name->to_utf8() + "' before initialization");
        Environment::Binding const* binding = env->find(binding_name);
        if (binding == nullptr)
            return in.throw_reference_error(name->to_utf8() + " is not defined");
        if (binding->import_module != nullptr) {
            target = binding->import_module;
            binding_name = binding->import_name;
            continue;
        }
        if (!binding->initialized)
            return in.throw_reference_error("Cannot access '" + name->to_utf8() + "' before initialization");
        return binding->value;
    }
    return in.throw_reference_error(name->to_utf8() + " is not defined");
}

std::optional<std::optional<PropertyDescriptor>> ModuleNamespaceObject::get_own_property(Interpreter& in, PropertyKey const& key)
{
    if (key.is_symbol())
        return Object::get_own_property(key);
    if (!is_export(key))
        return std::optional<PropertyDescriptor> {};
    std::optional<Value> const value = read_export(in, in.heap().key_to_string(key));
    if (!value)
        return std::nullopt;
    return PropertyDescriptor::data(*value, static_cast<std::uint8_t>(Writable | Enumerable));
}

// The non-throwing [[GetOwnProperty]]: what the throwing one answers when
// the binding is readable, and a descriptor of undefined for one in its
// dead zone — the reflective built-ins take the throwing path through
// Interpreter::get_own_property instead.
std::optional<PropertyDescriptor> ModuleNamespaceObject::get_own_property(PropertyKey const& key) const
{
    if (key.is_symbol())
        return Object::get_own_property(key);
    if (!is_export(key))
        return std::nullopt;
    Value value = Value::undefined();
    JsString* name = nullptr;
    if (key.is_atom()) {
        name = key.as_atom();
    } else {
        std::u16string const text = index_text(key.as_index());
        auto const match = std::find_if(m_exports.begin(), m_exports.end(), [&](JsString const* candidate) { return candidate->view() == text; });
        name = match == m_exports.end() ? nullptr : *match;
    }
    if (name != nullptr) {
        std::vector<std::pair<ModuleRecord*, JsString*>> resolve_set;
        ExportResolution const resolution = m_module->resolve_export(name, resolve_set);
        if (resolution.found() && resolution.binding_name != nullptr && resolution.module->environment() != nullptr) {
            Environment::Binding const* binding = resolution.module->environment()->find(resolution.binding_name);
            if (binding != nullptr && binding->initialized && binding->import_module == nullptr)
                value = binding->value;
        } else if (resolution.found() && resolution.module->existing_namespace() != nullptr) {
            value = Value::object(resolution.module->existing_namespace());
        }
    }
    return PropertyDescriptor::data(value, static_cast<std::uint8_t>(Writable | Enumerable));
}

// [[DefineOwnProperty]] (§10.4.6.3): a symbol goes the ordinary way; an
// export accepts only a descriptor it already satisfies.
bool ModuleNamespaceObject::define_own_property(PropertyKey const& key, PropertyDescriptor const& desc)
{
    if (key.is_symbol())
        return Object::define_own_property(key, desc);
    std::optional<PropertyDescriptor> const current = get_own_property(key);
    if (!current)
        return false;
    if (desc.configurable.value_or(false))
        return false;
    if (desc.enumerable.has_value() && !*desc.enumerable)
        return false;
    if (desc.is_accessor())
        return false;
    if (desc.writable.has_value() && !*desc.writable)
        return false;
    if (desc.value.has_value())
        return Interpreter::same_value(*desc.value, *current->value);
    return true;
}

bool ModuleNamespaceObject::has_property(PropertyKey const& key) const
{
    if (key.is_symbol())
        return Object::has_property(key);
    return is_export(key);
}

std::optional<Value> ModuleNamespaceObject::get(Interpreter& in, PropertyKey const& key, Value const& receiver)
{
    if (key.is_symbol())
        return Object::get(in, key, receiver);
    if (!is_export(key))
        return Value::undefined();
    return read_export(in, in.heap().key_to_string(key));
}

std::optional<bool> ModuleNamespaceObject::set(Interpreter&, PropertyKey const&, Value const&, Value const&)
{
    return false;
}

bool ModuleNamespaceObject::delete_property(PropertyKey const& key)
{
    if (key.is_symbol())
        return Object::delete_property(key);
    return !is_export(key);
}

// [[OwnPropertyKeys]] (§10.4.6.11): the exports in their sorted order,
// then the symbols.
std::vector<PropertyKey> ModuleNamespaceObject::own_keys() const
{
    std::vector<PropertyKey> keys;
    keys.reserve(m_exports.size() + 1);
    for (JsString* name : m_exports)
        keys.push_back(key_for_export(name));
    for (PropertyKey const& key : Object::own_keys()) {
        if (key.is_symbol())
            keys.push_back(key);
    }
    return keys;
}

void ModuleNamespaceObject::trace(Tracer& tracer)
{
    Object::trace(tracer);
    tracer.visit(m_module);
    for (JsString* name : m_exports)
        tracer.visit(name);
}

}
