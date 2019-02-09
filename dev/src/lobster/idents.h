// Copyright 2014 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LOBSTER_IDENTS
#define LOBSTER_IDENTS

#include "lobster/natreg.h"

#define FLATBUFFERS_DEBUG_VERIFICATION_FAILURE
#include "lobster/bytecode_generated.h"

namespace lobster {

struct NativeFun;
struct SymbolTable;

struct Node;
struct List;

struct SubFunction;

struct SpecIdent;

struct Ident : Named {
    size_t scopelevel;

    bool single_assignment = true;  // not declared const but def only, exp may or may not be const
    bool constant = false;          // declared const
    bool static_constant = false;   // not declared const but def only, exp is const.
    bool anonymous_arg = false;
    bool logvar = false;

    SpecIdent *cursid = nullptr;

    Ident(string_view _name, int _idx, size_t _sl)
        : Named(_name, _idx), scopelevel(_sl) {}

    void Assign(Lex &lex) {
        single_assignment = false;
        if (constant)
            lex.Error("variable " + name + " is constant");
    }

    flatbuffers::Offset<bytecode::Ident> Serialize(flatbuffers::FlatBufferBuilder &fbb,
                                                   bool is_top_level) {
        return bytecode::CreateIdent(fbb, fbb.CreateString(name), constant, is_top_level);
    }
};

struct SpecIdent {
    Ident *id;
    TypeRef type;
    Lifetime lt = LT_UNDEF;
    bool consume_on_last_use = false;
    int logvaridx = -1;
    int idx, sidx = -1;     // Into specidents, and into vm ordering.
    SubFunction *sf_def = nullptr;  // Where it is defined, including anonymous functions.

    SpecIdent(Ident *_id, TypeRef _type, int idx)
        : id(_id), type(_type), idx(idx){}
    int Idx() { assert(sidx >= 0); return sidx; }
    SpecIdent *&Current() { return id->cursid; }
};

// Only still needed because we have no idea which struct it refers to at parsing time.
struct SharedField : Named {
    SharedField(string_view _name, int _idx) : Named(_name, _idx) {}
    SharedField() : SharedField("", 0) {}
};

struct Field {
    TypeRef type;
    SharedField *id = nullptr;
    int genericref = -1;
    Node *defaultval = nullptr;

    Field() = default;
    Field(SharedField *_id, TypeRef _type, int _genericref, Node *_defaultval)
        : type(_type), id(_id),
        genericref(_genericref), defaultval(_defaultval) {}
    Field(const Field &o);
    ~Field();
};

struct FieldVector : GenericArgs {
    vector<Field> v;

    FieldVector(int nargs) : v(nargs) {}

    size_t size() const { return v.size(); }
    TypeRef GetType(size_t i) const { return v[i].type; }
    ArgFlags GetFlags(size_t) const { return AF_NONE; }
    string_view GetName(size_t i) const { return v[i].id->name; }
};

struct GenericParameter {
    string_view name;
};

struct Struct : Named {
    FieldVector fields { 0 };
    vector<GenericParameter> generics;
    Struct *next = nullptr, *first = this;
    Struct *superclass = nullptr;
    Struct *firstsubclass = nullptr, *nextsubclass = nullptr;  // Used in codegen.
    bool readonly = false;
    bool predeclaration = false;
    Type thistype { V_STRUCT, this };  // convenient place to store the type corresponding to this.
    TypeRef sametype = type_undefined;  // If all fields are int/float, this allows vector ops.
    type_elem_t typeinfo = (type_elem_t)-1;  // Runtime type.

    Struct(string_view _name, int _idx) : Named(_name, _idx) {}
    Struct()                            : Named("", 0) {}

    int Has(SharedField *fld) {
        for (auto &uf : fields.v) if (uf.id == fld) return int(&uf - &fields.v[0]);
        return -1;
    }

    Struct *CloneInto(Struct *st) {
        *st = *this;
        st->thistype = Type(V_STRUCT, st);
        st->next = next;
        st->first = first;
        next = st;
        return st;
    }

    bool IsSpecialization(Struct *other) {
        if (!generics.empty()) {
            for (auto struc = first->next; struc; struc = struc->next)
                if (struc == other)
                    return true;
            return false;
        } else {
            return this == other;
        }
    }

    int NumSuperTypes() {
        int n = 0;
        for (auto t = superclass; t; t = t->superclass) n++;
        return n;
    }

    flatbuffers::Offset<bytecode::Struct> Serialize(flatbuffers::FlatBufferBuilder &fbb) {
        vector<flatbuffers::Offset<bytecode::Field>> fieldoffsets;
        for (auto f : fields.v)
            fieldoffsets.push_back(bytecode::CreateField(fbb, fbb.CreateString(f.id->name)));
        return bytecode::CreateStruct(fbb, fbb.CreateString(name), idx,
                                      fbb.CreateVector(fieldoffsets));
    }
};

struct Arg : Typed {
    SpecIdent *sid = nullptr;

    Arg() = default;
    Arg(const Arg &o) : Typed(o), sid(o.sid) {}
    Arg(SpecIdent *_sid, TypeRef _type, ArgFlags _flags) : Typed(_type, _flags), sid(_sid) {}
};

struct ArgVector : GenericArgs {
    vector<Arg> v;

    ArgVector(int nargs) : v(nargs) {}

    size_t size() const { return v.size(); }
    TypeRef GetType(size_t i) const { return v[i].type; }
    ArgFlags GetFlags(size_t i) const { return v[i].flags; }
    string_view GetName(size_t i) const { return v[i].sid->id->name; }

    bool Add(const Arg &in) {
        for (auto &arg : v)
            if (arg.sid->id == in.sid->id)
                return false;
        v.push_back(in);
        return true;
    }
};

struct Function;

struct SubFunction {
    int idx;
    ArgVector args { 0 };
    ArgVector locals { 0 };
    ArgVector freevars { 0 };       // any used from outside this scope
    TypeRef fixedreturntype = nullptr;
    TypeRef returntype = type_undefined;
    size_t num_returns = 0;
    size_t reqret = 0;  // Do the caller(s) want values to be returned?
    const Lifetime ltret = LT_KEEP;
    vector<pair<const SubFunction *, TypeRef>> reuse_return_events;
    bool isrecursivelycalled = false;
    bool iscoroutine = false;
    ArgVector coyieldsave { 0 };
    TypeRef coresumetype;
    type_elem_t cotypeinfo = (type_elem_t)-1;
    List *body = nullptr;
    SubFunction *next = nullptr;
    Function *parent = nullptr;
    int subbytecodestart = 0;
    bool typechecked = false;
    bool freevarchecked = false;
    bool mustspecialize = false;
    bool logvarcallgraph = false;
    bool isdynamicfunctionvalue = false;
    int numcallers = 0;
    Type thistype { V_FUNCTION, this };  // convenient place to store the type corresponding to this

    SubFunction(int _idx) : idx(_idx) {}

    void SetParent(Function &f, SubFunction *&link) {
        parent = &f;
        next = link;
        link = this;
    }

    ~SubFunction();
};

struct Function : Named {
    int bytecodestart = 0;
    // functions with the same name and args, but different types (dynamic dispatch |
    // specialization)
    SubFunction *subf = nullptr;
    // functions with the same name but different number of args (overloaded)
    Function *sibf = nullptr;
    // if false, subfunctions can be generated by type specialization as opposed to programmer
    // implemented dynamic dispatch
    bool multimethod = false;
    TypeRef multimethodretval;
    // does not have a programmer specified name
    bool anonymous = false;
    // its merely a function type, has no body, but does have a set return type.
    bool istype = false;
    // Store the original types the function was declared with, before specialization.
    ArgVector orig_args { 0 };
    size_t scopelevel;

    Function(string_view _name, int _idx, size_t _sl)
        : Named(_name, _idx), scopelevel(_sl) {}
    ~Function() {}

    size_t nargs() const { return subf->args.v.size(); }

    int NumSubf() {
        int sum = 0;
        for (auto sf = subf; sf; sf = sf->next) sum++;
        return sum;
    }

    bool RemoveSubFunction(SubFunction *sf) {
        for (auto sfp = &subf; *sfp; sfp = &(*sfp)->next) if (*sfp == sf) {
            *sfp = sf->next;
            sf->next = nullptr;
            return true;
        }
        return false;
    }

    flatbuffers::Offset<bytecode::Function> Serialize(flatbuffers::FlatBufferBuilder &fbb) {
        return bytecode::CreateFunction(fbb, fbb.CreateString(name), bytecodestart);
    }
};

struct SymbolTable {
    unordered_map<string_view, Ident *> idents;  // Key points to value!
    vector<Ident *> identtable;
    vector<Ident *> identstack;
    vector<SpecIdent *> specidents;

    unordered_map<string_view, Struct *> structs;  // Key points to value!
    vector<Struct *> structtable;

    unordered_map<string_view, SharedField *> fields;  // Key points to value!
    vector<SharedField *> fieldtable;

    unordered_map<string_view, Function *> functions;  // Key points to value!
    vector<Function *> functiontable;
    vector<SubFunction *> subfunctiontable;
    SubFunction *toplevel = nullptr;

    vector<string> filenames;

    vector<size_t> scopelevels;

    struct WithStackElem { TypeRef type; Ident *id = nullptr; SubFunction *sf = nullptr; };
    vector<WithStackElem> withstack;
    vector<size_t> withstacklevels;

    enum { NUM_VECTOR_TYPE_WRAPPINGS = 3 };
    vector<TypeRef> default_int_vector_types[NUM_VECTOR_TYPE_WRAPPINGS],
                    default_float_vector_types[NUM_VECTOR_TYPE_WRAPPINGS];

    // Used during parsing.
    vector<SubFunction *> defsubfunctionstack;

    vector<Type *> typelist;  // Used for constructing new vector types, variables, etc.
    vector<vector<Type::TupleElem> *> tuplelist;

    string current_namespace;
    // FIXME: because we cleverly use string_view's into source code everywhere, we now have
    // no way to refer to constructed strings, and need to store them seperately :(
    // TODO: instead use larger buffers and constuct directly into those, so no temp string?
    vector<const char *> stored_names;

    ~SymbolTable() {
        for (auto id : identtable)       delete id;
        for (auto sid : specidents)      delete sid;
        for (auto st : structtable)      delete st;
        for (auto f  : functiontable)    delete f;
        for (auto sf : subfunctiontable) delete sf;
        for (auto f  : fieldtable)       delete f;
        for (auto t  : typelist)         delete t;
        for (auto t  : tuplelist)        delete t;
        for (auto n  : stored_names)     delete[] n;
    }

    string NameSpaced(string_view name) {
        assert(!current_namespace.empty());
        return cat(current_namespace, "_", name);
    }

    string_view StoreName(const string &s) {
        auto buf = new char[s.size()];
        memcpy(buf, s.data(), s.size());  // Look ma, no terminator :)
        stored_names.push_back(buf);
        return string_view(buf, s.size());
    }

    string_view MaybeNameSpace(string_view name, bool other_conditions) {
        return other_conditions && !current_namespace.empty() && scopelevels.size() == 1
            ? StoreName(NameSpaced(name))
            : name;
    }

    Ident *Lookup(string_view name) {
        auto it = idents.find(name);
        if (it != idents.end()) return it->second;
        if (!current_namespace.empty()) {
            auto it = idents.find(NameSpaced(name));
            if (it != idents.end()) return it->second;
        }
        return nullptr;
    }

    Ident *LookupAny(string_view name) {
        for (auto id : identtable) if (id->name == name) return id;
        return nullptr;
    }

    Ident *NewId(string_view name, SubFunction *sf) {
        auto ident = new Ident(name, (int)identtable.size(), scopelevels.size());
        ident->cursid = NewSid(ident, sf);
        identtable.push_back(ident);
        return ident;
    }

    Ident *LookupDef(string_view name, Lex &lex, bool anonymous_arg, bool islocal,
                     bool withtype) {
        auto sf = defsubfunctionstack.back();
        auto existing_ident = Lookup(name);
        if (anonymous_arg && existing_ident && existing_ident->cursid->sf_def == sf)
            return existing_ident;
        Ident *ident = nullptr;
        if (LookupWithStruct(name, lex, ident))
            lex.Error("cannot define variable with same name as field in this scope: " + name);
        ident = NewId(name, sf);
        ident->anonymous_arg = anonymous_arg;
        (islocal ? sf->locals : sf->args).v.push_back(
            Arg(ident->cursid, type_any, AF_GENERIC | (withtype ? AF_WITHTYPE : AF_NONE)));
        if (existing_ident) {
            lex.Error("identifier redefinition / shadowing: " + ident->name);
        }
        idents[ident->name /* must be in value */] = ident;
        identstack.push_back(ident);
        return ident;
    }

    Ident *LookupUse(string_view name, Lex &lex) {
        auto id = Lookup(name);
        if (!id)
            lex.Error("unknown identifier: " + name);
        return id;
    }

    void AddWithStruct(TypeRef t, Ident *id, Lex &lex, SubFunction *sf) {
        if (t->t != V_STRUCT) lex.Error(":: can only be used with struct/value types");
        for (auto &wp : withstack)
            if (wp.type->struc == t->struc)
                lex.Error("type used twice in the same scope with ::");
        // FIXME: should also check if variables have already been defined in this scope that clash
        // with the struct, or do so in LookupUse
        assert(t->struc);
        withstack.push_back({ t, id, sf });
    }

    SharedField *LookupWithStruct(string_view name, Lex &lex, Ident *&id) {
        auto fld = FieldUse(name);
        if (!fld) return nullptr;
        assert(!id);
        for (auto &wse : withstack) {
            if (wse.type->struc->Has(fld) >= 0) {
                if (id) lex.Error("access to ambiguous field: " + fld->name);
                id = wse.id;
            }
        }
        return id ? fld : nullptr;
    }

    WithStackElem GetWithStackBack() {
        return withstack.size()
            ? withstack.back()
            : WithStackElem();
    }

    void MakeLogVar(Ident *id) {
        id->logvar = true;
        defsubfunctionstack.back()->logvarcallgraph = true;
    }

    SubFunction *ScopeStart() {
        scopelevels.push_back(identstack.size());
        withstacklevels.push_back(withstack.size());
        auto sf = CreateSubFunction();
        defsubfunctionstack.push_back(sf);
        return sf;
    }

    void ScopeCleanup() {
        defsubfunctionstack.pop_back();
        while (identstack.size() > scopelevels.back()) {
            auto ident = identstack.back();
            auto it = idents.find(ident->name);
            if (it != idents.end()) {  // can already have been removed by private var cleanup
                idents.erase(it);
            }
            identstack.pop_back();
        }
        scopelevels.pop_back();
        while (withstack.size() > withstacklevels.back()) withstack.pop_back();
        withstacklevels.pop_back();
    }

    void UnregisterStruct(const Struct *st, Lex &lex) {
        if (st->predeclaration) lex.Error("pre-declared struct never defined: " + st->name);
        auto it = structs.find(st->name);
        if (it != structs.end()) structs.erase(it);
    }

    void UnregisterFun(Function *f) {
        auto it = functions.find(f->name);
        if (it != functions.end())  // it can already have been removed by another variation
            functions.erase(it);
    }

    void EndOfInclude() {
        current_namespace.clear();
        auto it = idents.begin();
        while (it != idents.end()) {
            if (it->second->isprivate) {
                idents.erase(it++);
            } else
                it++;
        }
    }

    Struct &StructDecl(string_view name, Lex &lex) {
        auto stit = structs.find(name);
        if (stit != structs.end()) {
            if (!stit->second->predeclaration) lex.Error("double declaration of type: " + name);
            stit->second->predeclaration = false;
            return *stit->second;
        } else {
            auto st = new Struct(name, (int)structtable.size());
            structs[st->name /* must be in value */] = st;
            structtable.push_back(st);
            return *st;
        }
    }

    Struct &StructUse(string_view name, Lex &lex) {
        auto stit = structs.find(name);
        if (stit != structs.end()) return *stit->second;
        if (!current_namespace.empty()) {
            stit = structs.find(NameSpaced(name));
            if (stit != structs.end()) return *stit->second;
        }
        lex.Error("unknown type: " + name);
        return *stit->second;
    }

    bool IsSuperTypeOrSame(const Struct *sup, const Struct *sub) {
        for (auto t = sub; t; t = t->superclass)
            if (t == sup)
                return true;
        return false;
    }

    const Struct *CommonSuperType(const Struct *a, const Struct *b) {
        if (a != b) for (;;) {
            a = a->superclass;
            if (!a) return nullptr;
            if (IsSuperTypeOrSame(a, b)) break;
        }
        return a;
    }

    SharedField &FieldDecl(string_view name) {
        auto fld = FieldUse(name);
        if (fld) return *fld;
        fld = new SharedField(name, (int)fieldtable.size());
        fields[fld->name /* must be in value */] = fld;
        fieldtable.push_back(fld);
        return *fld;
    }

    SharedField *FieldUse(string_view name) {
        auto it = fields.find(name);
        return it != fields.end() ? it->second : nullptr;
    }

    SubFunction *CreateSubFunction() {
        auto sf = new SubFunction((int)subfunctiontable.size());
        subfunctiontable.push_back(sf);
        return sf;
    }

    Function &CreateFunction(string_view name, string_view context) {
        auto fname = name.length() ? string(name) : cat("function", functiontable.size(), context);
        auto f = new Function(fname, (int)functiontable.size(), scopelevels.size());
        functiontable.push_back(f);
        return *f;
    }

    Function &FunctionDecl(string_view name, size_t nargs, Lex &lex) {
        auto fit = functions.find(name);
        if (fit != functions.end()) {
            if (fit->second->scopelevel != scopelevels.size())
                lex.Error("cannot define a variation of function " + name +
                          " at a different scope level");
            for (auto f = fit->second; f; f = f->sibf)
                if (f->nargs() == nargs)
                    return *f;
        }
        auto &f = CreateFunction(name, "");
        if (fit != functions.end()) {
            f.sibf = fit->second->sibf;
            fit->second->sibf = &f;
        } else {
            functions[f.name /* must be in value */] = &f;
        }
        return f;
    }

    Function *FindFunction(string_view name) {
        auto it = functions.find(name);
        if (it != functions.end()) return it->second;
        if (!current_namespace.empty()) {
            auto it = functions.find(NameSpaced(name));
            if (it != functions.end()) return it->second;
        }
        return nullptr;
    }

    SpecIdent *NewSid(Ident *id, SubFunction *sf, TypeRef type = nullptr) {
        auto sid = new SpecIdent(id, type, (int)specidents.size());
        sid->sf_def = sf;
        specidents.push_back(sid);
        return sid;
    }

    void CloneSids(ArgVector &av, SubFunction *sf) {
        for (auto &a : av.v) {
            a.sid = NewSid(a.sid->id, sf);
        }
    }

    void CloneIds(SubFunction &sf, const SubFunction &o) {
        sf.args = o.args;     CloneSids(sf.args, &sf);
        sf.locals = o.locals; CloneSids(sf.locals, &sf);
        // Don't clone freevars, these will be accumulated in the new copy anew.
    }

    Type *NewType() {
        // These get allocated for very few nodes, given that most types are shared or stored in
        // their own struct.
        auto t = new Type();
        typelist.push_back(t);
        return t;
    }

    TypeRef Wrap(TypeRef elem, ValueType with) {
        auto wt = WrapKnown(elem, with);
        return !wt.Null() ? wt : elem->Wrap(NewType(), with);
    }

    bool RegisterTypeVector(vector<TypeRef> *sv, const char **names) {
        if (sv[0].size()) return true;  // Already initialized.
        for (size_t i = 0; i < NUM_VECTOR_TYPE_WRAPPINGS; i++) {
            sv[i].push_back(nullptr);
            sv[i].push_back(nullptr);
        }
        for (auto name = names; *name; name++) {
            // Can't use stucts.find, since all are out of scope.
            for (auto struc : structtable) if (struc->name == *name) {
                for (size_t i = 0; i < NUM_VECTOR_TYPE_WRAPPINGS; i++) {
                    auto vt = TypeRef(&struc->thistype);
                    for (size_t j = 0; j < i; j++) vt = Wrap(vt, V_VECTOR);
                    sv[i].push_back(vt);
                }
                goto found;
            }
            return false;
            found:;
        }
        return true;
    }

    bool RegisterDefaultVectorTypes() {
        // TODO: This isn't great hardcoded in the compiler, would be better if it was declared in
        // lobster code.
        static const char *default_int_vector_type_names[]   =
            { "xy_i", "xyz_i", "xyzw_i", nullptr };
        static const char *default_float_vector_type_names[] =
            { "xy_f", "xyz_f", "xyzw_f", nullptr };
        return RegisterTypeVector(default_int_vector_types, default_int_vector_type_names) &&
               RegisterTypeVector(default_float_vector_types, default_float_vector_type_names);
    }

    TypeRef VectorType(TypeRef vt, size_t level, int arity) const {
        return vt->sub->t == V_INT
            ? default_int_vector_types[level][arity]
            : default_float_vector_types[level][arity];
    }

    bool IsGeneric(TypeRef type) {
        if (type->t == V_ANY) return true;
        auto u = type->UnWrapped();
        return u->t == V_STRUCT && !u->struc->generics.empty();
    }

    // This one is used to sort types for multi-dispatch.
    bool IsLessGeneralThan(const Type &a, const Type &b) const {
        if (a.t != b.t) return a.t > b.t;
        switch (a.t) {
            case V_VECTOR:
            case V_NIL:
                return IsLessGeneralThan(*a.sub, *b.sub);
            case V_FUNCTION:
                return a.sf->idx < b.sf->idx;
            case V_STRUCT: {
                if (a.struc == b.struc) return false;
                auto ans = a.struc->NumSuperTypes();
                auto bns = b.struc->NumSuperTypes();
                return ans != bns
                    ? ans > bns
                    : a.struc->idx < b.struc->idx;
            }
            default:
                return false;
        }
    }

    void Serialize(vector<int> &code,
                   vector<uchar> &code_attr,
                   vector<type_elem_t> &typetable,
                   vector<type_elem_t> &vint_typeoffsets,
                   vector<type_elem_t> &vfloat_typeoffsets,
                   vector<bytecode::LineInfo> &linenumbers,
                   vector<bytecode::SpecIdent> &sids,
                   vector<string_view> &stringtable,
                   vector<int> &speclogvars,
                   string &bytecode) {
        flatbuffers::FlatBufferBuilder fbb;
        vector<flatbuffers::Offset<flatbuffers::String>> fns;
        for (auto &f : filenames) fns.push_back(fbb.CreateString(f));
        vector<flatbuffers::Offset<bytecode::Function>> functionoffsets;
        for (auto f : functiontable) functionoffsets.push_back(f->Serialize(fbb));
        vector<flatbuffers::Offset<bytecode::Struct>> structoffsets;
        for (auto s : structtable) structoffsets.push_back(s->Serialize(fbb));
        vector<flatbuffers::Offset<bytecode::Ident>> identoffsets;
        for (auto i : identtable) identoffsets.push_back(i->Serialize(fbb, i->cursid->sf_def == toplevel));
        auto bcf = bytecode::CreateBytecodeFile(fbb,
            LOBSTER_BYTECODE_FORMAT_VERSION,
            fbb.CreateVector(code),
            fbb.CreateVector(code_attr),
            fbb.CreateVector((vector<int> &)typetable),
            fbb.CreateVector<flatbuffers::Offset<flatbuffers::String>>(stringtable.size(),
                [&](size_t i) {
                    return fbb.CreateString(stringtable[i].data(), stringtable[i].size());
                }
            ),
            fbb.CreateVectorOfStructs(linenumbers),
            fbb.CreateVector(fns),
            fbb.CreateVector(functionoffsets),
            fbb.CreateVector(structoffsets),
            fbb.CreateVector(identoffsets),
            fbb.CreateVectorOfStructs(sids),
            fbb.CreateVector((vector<int> &)vint_typeoffsets),
            fbb.CreateVector((vector<int> &)vfloat_typeoffsets),
            fbb.CreateVector(speclogvars));
        bytecode::FinishBytecodeFileBuffer(fbb, bcf);
        bytecode.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }
};

inline string TypeName(TypeRef type, int flen = 0, const SymbolTable *st = nullptr) {
    switch (type->t) {
    case V_STRUCT:
        return type->struc->name;
    case V_VECTOR:
        return flen && type->Element()->Numeric()
            ? (flen < 0
                ? (type->Element()->t == V_INT ? "vec_i" : "vec_f")  // FIXME: better names?
                : TypeName(st->VectorType(type, 0, flen)))
            : (type->Element()->t == V_VAR
                ? "[]"
                : "[" + TypeName(type->Element(), flen, st) + "]");
    case V_FUNCTION:
        return type->sf // || type->sf->anonymous
            ? type->sf->parent->name
            : "function";
    case V_NIL:
        return type->Element()->t == V_VAR
            ? "nil"
            : TypeName(type->Element(), flen, st) + "?";
    case V_COROUTINE:
        return type->sf
            ? "coroutine(" + type->sf->parent->name + ")"
            : "coroutine";
    case V_TUPLE: {
        string s = "(";
        for (auto [i, te] : enumerate(*type->tup)) {
            if (i) s += ", ";
            s += TypeName(te.type);
        }
        s += ")";
        return s;
    }
    default:
        return string(BaseTypeName(type->t));
    }
}

}  // namespace lobster

#endif  // LOBSTER_IDENTS
