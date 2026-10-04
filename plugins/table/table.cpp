// table.cpp
// a Lua-style table for embr: an associative container keyed by any value, not just strings like embr's map literals
// it's a plugin built on TypedPtr/userdata, rather than widening Value::map_type's key type, which would touch
// MapExpr/BUILD_MAP in both backends and Value's equality/printing
//
// usage
//   import "table"
//   t = table()
//   table_set(t, 1, "one")
//   table_set(t, "name", "embr")
//   print(table_get(t, 1))          # "one"
//   print(table_has(t, "missing"))  # 0
//   for pair in table_items(t)
//       print(pair[0])
//       print(pair[1])
//   end
//
// good to know
//   - not synchronized: use an instance from one thread only. to share across workers see shared_table() in
//     plugins/parallel, which uses the same key rules (table.h) plus a lock
//   - the key rules (table.h) are stricter than Value::operator==, which treats mismatched types as equal if they
//     print the same (5 == "5"). that's fine for `if x == 5` but wrong for a hash table. these follow Lua:
//       numbers:       int and float with the same value are the same key (5 and 5.0). NaN raises
//       strings:       compared by content, never equal to a number key
//       pointers/fns:  compared by identity (pointer address, or the Callable object's address), like Lua
//       arrays/maps:   not supported (raises). they are plain values with no identity, so there is no sound
//                      meaning of "the same key"
//   - iteration order (table_keys/table_values/table_items) is unspecified and can change between runs, like
//     std::unordered_map. keys can mix numbers, strings and identities, so there is no one sort order

#include "table.h"

using namespace embr;
using namespace embrtable;

static Param pPtr(std::string n) { return Param::req(std::move(n), TS::Ptr); }
static Param pAny(std::string n) { return Param::req(std::move(n), TS::Any); }

struct Table {
    TableData data;
};

static Table* requireTable(const Value& v) {
    return v.userdata<Table>("table.handle");
}

EMBR_PLUGIN {

    // table() -> ptr<table.handle>
    interp->bind("table", [interp](const std::vector<Value>&) -> Value {
        Value h = Value::makeUserdata<Table>("table.handle");
        // at interpreter teardown, empty this table (see Interpreter::addClearable): breaks a table that holds itself
        interp->addClearable(h.asPointer().owner, [w = std::weak_ptr<Table>(std::static_pointer_cast<Table>(h.asPointer().owner))] {
            if (auto t = w.lock()) t->data.clear();
        });
        // let gc_collect() see the keys and values this table holds (a closure stored here would
        // otherwise look unreachable and have its captured scope cleared out from under it)
        Table* t = static_cast<Table*>(h.asPointer().ptr);
        h.withTracer([t](const TraceVisitor& visit) {
            for (const auto& kv : t->data) { visit(kv.second.first); visit(kv.second.second); }
        });
        return h;
    });

    // table_set(t, key, value) -> 0
    interp->bindSig("table_set", {pPtr("t"), pAny("key"), pAny("value")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_set", args[1]);
        t->data[k] = {args[1], args[2]};
        return Value(0.0);
    });

    // table_get(t, key) -> any
    // raises "key not found" if key isn't in t (matches how indexing a
    // core map with [] behaves), use table_has() to check first.
    interp->bindSig("table_get", {pPtr("t"), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_get", args[1]);
        auto it = t->data.find(k);
        if (it == t->data.end())
            throwTableError("table_get", "key not found: " + valueRepr(args[1]));
        return it->second.second;
    });

    // table_has(t, key) -> num
    interp->bindSig("table_has", {pPtr("t"), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_has", args[1]);
        return Value(t->data.count(k) ? 1.0 : 0.0);
    });

    // table_delete(t, key) -> num
    // returns 1 if a key was actually removed, 0 if it wasn't present
    // (a no-op, not an error, matches fs_remove()/meta_undef()'s convention).
    interp->bindSig("table_delete", {pPtr("t"), pAny("key")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_delete", args[1]);
        return Value(t->data.erase(k) > 0 ? 1.0 : 0.0);
    });

    // table_len(t) -> num
    interp->bindSig("table_len", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        return Value((double)t->data.size());
    });

    // table_clear(t) -> 0
    interp->bindSig("table_clear", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        t->data.clear();
        return Value(0.0);
    });

    // table_keys(t) -> arr
    interp->bindSig("table_keys", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) out.push_back(kv.first);
        return Value(std::move(out));
    });

    // table_values(t) -> arr
    interp->bindSig("table_values", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) out.push_back(kv.second);
        return Value(std::move(out));
    });

    // table_items(t) -> arr of [key, value] pairs
    // the way to iterate a table's contents (embr's `for k, v in ...`
    // syntax only understands core map/array values, not a plugin userdata
    // handle), e.g. `for pair in table_items(t) ... pair[0]/pair[1] ... end`
    interp->bindSig("table_items", {pPtr("t")},
    [](const std::vector<Value>& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) {
            Value::array_type pair;
            pair.push_back(kv.first);
            pair.push_back(kv.second);
            out.emplace_back(std::move(pair));
        }
        return Value(std::move(out));
    });
}
