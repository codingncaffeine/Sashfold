#pragma once

// Module records (§16.2.1.5): a Source Text Module Record over a parsed
// Program, with the fields the specification names so that its
// algorithms — GetExportedNames, ResolveExport, InnerModuleLinking,
// InitializeEnvironment, InnerModuleEvaluation — port line for line. The
// record is a cell: the module map on the Interpreter keeps every record
// alive for the realm's life, and a record keeps its environment, its
// namespace object and its meta object, so nothing script can reach
// outlives what it points into.
//
// Loading is the host's: a resolver turns a specifier into a key and a
// fetcher turns a key into source text (Interpreter::set_module_hooks);
// the Interpreter walks a module's requests with them before linking.

#include "js/Ast.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Object.h"
#include "js/Value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::js {

class ModuleRecord;

// The answer of ResolveExport (§16.2.1.6.3): the module and binding a
// name resolves to — a null binding name standing for the module's
// namespace object — or that the name is not exported, or that two star
// exports both provide it.
struct ExportResolution {
    enum class Kind : std::uint8_t { NotFound, Ambiguous, Binding };
    Kind kind = Kind::NotFound;
    ModuleRecord* module = nullptr;
    JsString* binding_name = nullptr; // Binding: null = the namespace of `module`

    bool found() const { return kind == Kind::Binding; }
};

class ModuleRecord : public Cell {
public:
    enum class Status : std::uint8_t { Unlinked, Linking, Linked, Evaluating, EvaluatingAsync, Evaluated };

    ModuleRecord(std::string key, std::unique_ptr<Program> program);

    // The host's name for the module — a URL, a path — and `import.meta.url`.
    std::string const& key() const { return m_key; }
    Program const& program() const { return *m_program; }
    Status status() const { return m_status; }
    Environment* environment() const { return m_environment; }
    // [[EvaluationError]]: empty until an evaluation failed.
    Value const& evaluation_error() const { return m_evaluation_error; }
    bool has_top_level_await() const { return m_program->has_top_level_await; }
    // [[TopLevelCapability]]'s promise once Evaluate has run, else empty.
    Value evaluation_promise() const { return m_top_level_capability ? m_top_level_capability->promise : Value::empty(); }
    Object* import_meta() const { return m_import_meta; }
    void set_import_meta(Object* meta) { m_import_meta = meta; }

    // [[LoadedModules]]: the record each requested specifier resolved to,
    // filled by the loader before linking. GetImportedModule (§16.2.1.9)
    // is the lookup; null means the loader never got there.
    void add_loaded_module(JsString* specifier, ModuleRecord* module);
    ModuleRecord* imported_module(JsString* specifier) const;
    bool loaded_all() const { return m_loaded_modules.size() >= m_program->requested_modules.size(); }

    // GetExportedNames (§16.2.1.6.2) and ResolveExport (§16.2.1.6.3).
    std::vector<JsString*> exported_names(std::vector<ModuleRecord*>& export_star_set);
    ExportResolution resolve_export(JsString* export_name, std::vector<std::pair<ModuleRecord*, JsString*>>& resolve_set);
    // GetModuleNamespace (§16.2.1.10): made once, on first request; the
    // second answers it without an interpreter, or null before then.
    Object* get_namespace(Interpreter&);
    Object* existing_namespace() const { return m_namespace; }

    // Link (§16.2.1.5.2) and Evaluate (§16.2.1.5.3). Link is false with a
    // SyntaxError pending; Evaluate answers the top-level promise, or
    // nullopt when the capability itself could not be made.
    bool link(Interpreter&);
    std::optional<Value> evaluate(Interpreter&);

    void trace(Tracer&) override;
    std::size_t size_in_bytes() const override { return sizeof(*this); }

private:
    friend class Interpreter;
    // The inner algorithms, with the specification's names.
    std::optional<std::size_t> inner_linking(Interpreter&, std::vector<ModuleRecord*>& stack, std::size_t index);
    bool initialize_environment(Interpreter&);
    std::optional<std::size_t> inner_evaluation(Interpreter&, std::vector<ModuleRecord*>& stack, std::size_t index);
    bool execute_module(Interpreter&);

    std::string m_key;
    std::unique_ptr<Program> m_program;
    Environment* m_environment = nullptr; // [[Environment]]; null until InitializeEnvironment
    Object* m_namespace = nullptr; // [[Namespace]]
    Object* m_import_meta = nullptr; // [[ImportMeta]]
    Status m_status = Status::Unlinked;
    Value m_evaluation_error = Value::empty();
    std::optional<PromiseCapability> m_top_level_capability; // [[TopLevelCapability]]
    // [[AsyncEvaluation]] and its order, [[AsyncParentModules]],
    // [[PendingAsyncDependencies]], [[CycleRoot]]: the async half of
    // InnerModuleEvaluation keeps them; a module with a top-level await
    // is refused by name until that half is written.
    bool m_async_evaluation = false;
    std::uint64_t m_async_evaluation_order = 0;
    std::vector<ModuleRecord*> m_async_parent_modules;
    std::size_t m_pending_async_dependencies = 0;
    ModuleRecord* m_cycle_root = nullptr;
    std::optional<std::size_t> m_dfs_index; // [[DFSIndex]], [[DFSAncestorIndex]]: EMPTY = nullopt
    std::optional<std::size_t> m_dfs_ancestor_index;
    std::vector<std::pair<JsString*, ModuleRecord*>> m_loaded_modules;
};

// A module namespace exotic object (§10.4.6): one property per export of
// the module, each a live read of the binding it resolves to, no
// prototype, never extensible, @@toStringTag "Module".
class ModuleNamespaceObject : public Object {
public:
    ModuleNamespaceObject(ModuleRecord* module, std::vector<JsString*> exports);

    ModuleRecord* module() const { return m_module; }
    std::vector<JsString*> const& exports() const { return m_exports; } // sorted by code units

    // The exotic [[GetOwnProperty]] reads the export's value, and an
    // uninitialised binding is a ReferenceError there (§10.4.6.4 step 4);
    // the virtual below cannot throw, so Interpreter::get_own_property
    // routes a namespace here and the reflective built-ins go through it.
    std::optional<std::optional<PropertyDescriptor>> get_own_property(Interpreter&, PropertyKey const&);
    std::optional<PropertyDescriptor> get_own_property(PropertyKey const&) const override;
    bool define_own_property(PropertyKey const&, PropertyDescriptor const&) override;
    bool has_property(PropertyKey const&) const override;
    std::optional<Value> get(Interpreter&, PropertyKey const&, Value const& receiver) override;
    std::optional<bool> set(Interpreter&, PropertyKey const&, Value const&, Value const& receiver) override;
    bool delete_property(PropertyKey const&) override;
    std::vector<PropertyKey> own_keys() const override;

    void trace(Tracer&) override;

private:
    bool is_export(PropertyKey const& key) const;
    // The binding an export resolves to, read: the value, or nullopt
    // with a ReferenceError pending for a binding in its dead zone.
    std::optional<Value> read_export(Interpreter&, JsString* name);

    ModuleRecord* m_module;
    std::vector<JsString*> m_exports;
};

}
