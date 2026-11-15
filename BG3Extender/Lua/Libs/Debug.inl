#include <Extender/ScriptExtender.h>
#include <Lua/Debugger/LuaDebugger.h>
#include <tlhelp32.h>
#include <atomic>

/// <lua_module>Debug</lua_module>
BEGIN_NS(lua::debug)

void DebugDumpLifetimes(lua_State* L)
{
    auto const& pool = State::FromLua(L)->GetLifetimePool().GetAllocator();

    unsigned l1free{ 0 }, l1partial{ 0 }, l1full{ 0 };
    unsigned l2free{ 0 }, l2partial{ 0 }, l2full{ 0 };
    unsigned l3free{ 0 }, l3partial{ 0 }, l3full{ 0 };
    unsigned totalObjs{ 0 };

    for (auto i = 0; i < std::size(pool.l1_); i++) {
        if (pool.l1_[i] == 0) l1full++;
        else if (pool.l1_[i] == 0xffffffffffffffffull) l1free++;
        else l1partial++;
    }

    for (auto i = 0; i < std::size(pool.l2_); i++) {
        if (pool.l2_[i] == 0) l2full++;
        else if (pool.l2_[i] == 0xffffffffffffffffull) l2free++;
        else l2partial++;
    }

    for (auto i = 0; i < std::size(pool.l3_); i++) {
        if (pool.l3_[i] == 0) l3full++;
        else if (pool.l3_[i] == 0xffffffffffffffffull) l3free++;
        else l3partial++;
        totalObjs += (unsigned)_mm_popcnt_u64(pool.l3_[i]);
    }

    std::cout << " === LIFETIME STATS === " << std::endl;
    std::cout << "L1: " << l1free << " free pages, " << l1partial << " partially saturated pages, " << l1full << " full pages" << std::endl;
    std::cout << "L2: " << l2free << " free pages, " << l2partial << " partially saturated pages, " << l2full << " full pages" << std::endl;
    std::cout << "L3: " << l3free << " free pages, " << l3partial << " partially saturated pages, " << l3full << " full pages" << std::endl;
    std::cout << "Objects: " << 262144 << " in pool, " << totalObjs << " free" << std::endl;
}

void DumpStack(lua_State* L)
{
    auto top = lua_gettop(L);
    for (int idx = 1; idx <= top; idx++) {
        switch (lua_type(L, idx)) {
        case LUA_TNIL:
            INFO("<%d> nil", idx);
            break;

        case LUA_TBOOLEAN:
            INFO("<%d> %s", idx, lua_toboolean(L, idx) ? "true" : "false");
            break;

        case LUA_TNUMBER:
            INFO("<%d> %f", idx, (float)lua_tonumber(L, idx));
            break;

        case LUA_TSTRING:
            INFO("<%d> String '%s'", idx, lua_tostring(L, idx));
            break;

        default:
            INFO("<%d> %s", idx, GetDebugName(L, idx));
            break;
        }
    }
}

ValueType GetBaseType(ValueType type)
{
    return (*gExtender->GetServer().Osiris().GetGlobals().Types)->ResolveAlias((uint16_t)type);
}

char const * OsiToLuaTypeName(ValueType type)
{
    auto alias = GetBaseType(type);

    switch (alias) {
    case ValueType::Integer:
    case ValueType::Integer64:
        return "integer";

    case ValueType::Real:
        return "number";

    case ValueType::String:
        return "string";

    case ValueType::GuidString:
        return "string GUID";

    default:
        return "any";
    }
}

STDString DoGenerateIdeHelpers(bool builtinOnly)
{
    STDString helpers;
    helpers.reserve(0x20000);

    STDString functionComment, functionDefn;

    auto functions = gExtender->GetServer().Osiris().GetGlobals().Functions;

    (*functions)->Iterate([&helpers, &functionComment, &functionDefn, builtinOnly](OsiString const & key, OsiFunctionDef const * func) {
        if (builtinOnly
            && func->Type != FunctionType::Event
            && func->Type != FunctionType::Call
            && func->Type != FunctionType::Query
            && func->Type != FunctionType::SysCall
            && func->Type != FunctionType::SysQuery) {
            return;
        }

        functionComment.clear();
        functionDefn.clear();
        auto const & outParams = func->Signature->OutParamList;
        auto numOutParams = outParams.numOutParams();

        auto types = func->Signature->Params->Params.Head;
        for (auto i = 0; i < func->Signature->Params->Params.Size; i++) {
            types = types->Next;

            if (!outParams.isOutParam(i)) {
                functionComment += "--- @param arg";
                functionComment += std::to_string(i + 1);
                functionComment += " ";
                functionComment += OsiToLuaTypeName((ValueType)types->Item.Type);
                functionComment += "\r\n";
            }
        }

        if (numOutParams > 0) {
            auto types = func->Signature->Params->Params.Head;
            for (auto i = 0; i < func->Signature->Params->Params.Size; i++) {
                types = types->Next;

                if (outParams.isOutParam(i)) {
                    functionComment += "--- @return ";
                    functionComment += OsiToLuaTypeName((ValueType)types->Item.Type);
                    functionComment += "\r\n";
                }
            }
        } else {
            if (func->Type == FunctionType::SysQuery
                || func->Type == FunctionType::Query
                || func->Type == FunctionType::UserQuery) {
                functionComment += "--- @return boolean Did the query succeed?\r\n";
            }
        }

        functionDefn += func->Signature->Name;
        functionDefn += " = function (";

        auto pendingInParams = func->Signature->Params->Params.Size - numOutParams;
        for (auto i = 0; i < func->Signature->Params->Params.Size; i++) {
            if (!outParams.isOutParam(i)) {
                functionDefn += "arg";
                functionDefn += std::to_string(i + 1);
                if (--pendingInParams > 0) {
                    functionDefn += ", ";
                }
            }
        }

        functionDefn += ") end\r\n\r\n";

        helpers += functionComment;
        helpers += "Osi.";
        helpers += functionDefn;

        // Export global name if function is a builtin
        if (func->Type == FunctionType::Event
            || func->Type == FunctionType::Call
            || func->Type == FunctionType::Query
            || func->Type == FunctionType::SysCall
            || func->Type == FunctionType::SysQuery) {
            helpers += functionComment;
            helpers += functionDefn;
        }
    });

    return helpers;
}

void GenerateIdeHelpers(lua_State* L, std::optional<bool> builtinOnly)
{
#if defined(OSI_EOCAPP)
    if (gExtender->GetConfig().DeveloperMode) {
#endif
        auto lua = State::FromLua(L);
        if (lua->IsRestricted(RestrictionFlag::Osiris) || !gExtender->GetServer().IsInContext()) {
            luaL_error(L, "GenerateIdeHelpers() can only be called when Osiris is available");
        }

        auto helpers = DoGenerateIdeHelpers(builtinOnly && *builtinOnly);

        auto path = GetStaticSymbols().ToPath("", PathRootType::Data);
        path += "Mods/";
        path += GetStaticSymbols().GetModManagerServer()->BaseModule.Info.Directory;
        path += "/Story/RawFiles/Lua/OsiIdeHelpers.lua";

        std::ofstream f(path.c_str(), std::ios::out | std::ios::binary);
        if (!f.good()) {
            OsiError("Could not open file to save IDE helpers: '" << path << "'");
            return;
        }

        f.write(helpers.c_str(), helpers.size());
#if defined(OSI_EOCAPP)
    } else {
        OsiError("GenerateIdeHelpers() only supported in developer mode");
    }
#endif
}

bool IsDeveloperMode()
{
    return gExtender->GetConfig().DeveloperMode;
}

// --- Raw access for reverse engineering -------------------------------------------------------
// Available in developer mode only. Reading and writing go through Read/WriteProcessMemory on
// the game's own process, so an address that is not mapped (or not writable) makes the call
// fail instead of crashing the game. Addresses are plain integers.

void RequireDeveloperMode(lua_State* L, char const* function)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        luaL_error(L, "Ext.Debug.%s() is only available in developer mode", function);
    }
}

// Ext.Debug.ReadMemory(address, size) -> string of `size` bytes, or nil if any of them cannot be read
UserReturn ReadMemory(lua_State* L, int64_t address, int32_t size)
{
    RequireDeveloperMode(L, "ReadMemory");
    if (size <= 0 || size > 0x100000) {
        return luaL_error(L, "Size must be between 1 and 1048576 bytes");
    }

    STDString buffer;
    buffer.resize((std::size_t)size);
    SIZE_T read{ 0 };
    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, buffer.data(), (SIZE_T)size, &read) && read == (SIZE_T)size) {
        lua_pushlstring(L, buffer.data(), buffer.size());
    } else {
        lua_pushnil(L);
    }

    return 1;
}

// Ext.Debug.WriteMemory(address, bytes) -> whether all bytes were written.
// Nothing checks what is being overwritten: a wrong address corrupts the game.
bool WriteMemory(lua_State* L, int64_t address, StringView bytes)
{
    RequireDeveloperMode(L, "WriteMemory");
    if (bytes.empty() || bytes.size() > 0x100000) {
        luaL_error(L, "Between 1 and 1048576 bytes can be written at once");
    }

    SIZE_T written{ 0 };
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, bytes.data(), bytes.size(), &written)
        && written == bytes.size();
}

// Ext.Debug.GetModuleInfo() -> { Base = address, Size = bytes } of the game executable
UserReturn GetModuleInfo(lua_State* L)
{
    RequireDeveloperMode(L, "GetModuleInfo");
    auto base = (uint8_t const*)GetModuleHandleW(NULL);
    auto dos = (IMAGE_DOS_HEADER const*)base;
    auto nt = (IMAGE_NT_HEADERS const*)(base + dos->e_lfanew);

    lua_newtable(L);
    settable(L, "Base", (int64_t)base);
    settable(L, "Size", (int64_t)nt->OptionalHeader.SizeOfImage);
    return 1;
}

// Ext.Debug.GetComponentTypes() -> every component type the game registered on this side:
//   { Name, Index, InlineSize, TotalSize, OneFrame, Replicated, Mapped }
// Mapped tells whether the extender has a layout for it (entity.<Name> works).
UserReturn GetComponentTypes(lua_State* L)
{
    RequireDeveloperMode(L, "GetComponentTypes");
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto world = ecs.GetEntityWorld();

    lua_newtable(L);
    if (world == nullptr) return 1;

    int index = 1;
    ecs.ForEachComponentType([&](STDString const& name, ecs::ComponentTypeIndex type) {
        lua_newtable(L);
        settable(L, "Name", name);
        settable(L, "Index", (int32_t)type);
        auto entry = world->ComponentRegistry_.Get(type);
        if (entry != nullptr) {
            settable(L, "InlineSize", (int32_t)entry->InlineSize);
            settable(L, "TotalSize", (int32_t)entry->TotalSize);
            settable(L, "OneFrame", entry->OneFrame);
            settable(L, "Replicated", entry->Replicated);
        }
        settable(L, "Mapped", ecs.GetComponentType(type).has_value());
        lua_rawseti(L, -2, index++);
    });

    return 1;
}

// Ext.Debug.GetSystemTypes() -> { { Name, Index, Mapped } } for every system the game registered on this side
UserReturn GetSystemTypes(lua_State* L)
{
    RequireDeveloperMode(L, "GetSystemTypes");
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();

    lua_newtable(L);
    int index = 1;
    ecs.ForEachSystemType([&](STDString const& name, ecs::SystemTypeIndex type, bool mapped) {
        lua_newtable(L);
        settable(L, "Name", name);
        settable(L, "Index", (int32_t)type);
        settable(L, "Mapped", mapped);
        lua_rawseti(L, -2, index++);
    });

    return 1;
}

// Ext.Debug.GetComponentAddress(entity, name) -> { Address, InlineSize, TotalSize } or nil.
// Works for components the extender has no layout for. When InlineSize is smaller than
// TotalSize the entity stores a pointer: the component itself is at the address read from there.
UserReturn GetComponentAddress(lua_State* L, EntityHandle entity, STDString name)
{
    RequireDeveloperMode(L, "GetComponentAddress");
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto world = ecs.GetEntityWorld();
    auto type = ecs.GetComponentIndex(name);
    auto entry = (world != nullptr && type) ? world->ComponentRegistry_.Get(*type) : nullptr;
    if (entry == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    auto component = world->GetRawComponent(entity, *type, entry->InlineSize);
    if (component == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    settable(L, "Address", (int64_t)component);
    settable(L, "InlineSize", (int32_t)entry->InlineSize);
    settable(L, "TotalSize", (int32_t)entry->TotalSize);
    return 1;
}

// --- Addresses of fields, module/RVA conversion, and a typed read --------------------------------
// A component's field lives at a fixed offset inside the component, and the component lives at a
// place in the process; a place in the process is also module+0xRVA, the form the decompiler
// database speaks. These turn one into the other, all read-only, developer mode only. Nothing here
// writes to the game or sets a breakpoint: it reads memory and the extender's own type information.

// The scalar value types ReadValue understands, by the type information's own name, with their
// width. A field of any other kind (object, array, string, enum) still has an address and an
// offset; only its bytes are not turned into a Lua number here.
struct ScalarType
{
    char const* Name;
    uint32_t Size;
    enum { Signed, Unsigned, Float, Bool } Repr;
};

static constexpr ScalarType ScalarTypes[] = {
    { "int8", 1, ScalarType::Signed }, { "uint8", 1, ScalarType::Unsigned },
    { "int16", 2, ScalarType::Signed }, { "uint16", 2, ScalarType::Unsigned },
    { "int32", 4, ScalarType::Signed }, { "uint32", 4, ScalarType::Unsigned },
    { "int64", 8, ScalarType::Signed }, { "uint64", 8, ScalarType::Unsigned },
    { "float", 4, ScalarType::Float }, { "double", 8, ScalarType::Float },
    { "bool", 1, ScalarType::Bool },
};

static ScalarType const* FindScalarType(StringView name)
{
    for (auto const& t : ScalarTypes) {
        if (name == t.Name) return &t;
    }
    return nullptr;
}

// Whether [address, address + size) is entirely committed and readable, asked of the OS rather
// than guessed: a bad address then returns nil instead of reaching into an unmapped page.
static bool IsReadableRange(uint8_t const* address, std::size_t size)
{
    if (size == 0) return false;
    auto const* p = address;
    auto const* end = address + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        auto protect = mbi.Protect & 0xff;
        bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE
            || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ
            || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        p = (uint8_t const*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// Ext.Debug.GetFieldAddress(entity, component, field) -> absolute address of one field, from the
// extender's own type information: { Address, Offset, ComponentAddress, Type, Size, Scalar } or nil.
// Only components the extender has a layout for (its property map) resolve a field. The component
// pointer is dereferenced when the type is a proxy, so Address points at the live field either way.
UserReturn GetFieldAddress(lua_State* L, EntityHandle entity, STDString component, STDString field)
{
    RequireDeveloperMode(L, "GetFieldAddress");
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto world = ecs.GetEntityWorld();
    auto index = (world != nullptr) ? ecs.GetComponentIndex(component) : std::optional<ecs::ComponentTypeIndex>{};
    auto extType = index ? ecs.GetComponentType(*index) : std::optional<ExtComponentType>{};
    if (!index || !extType) {
        lua_pushnil(L);
        return 1;
    }

    auto const& meta = ecs.GetComponentMeta(*extType);
    auto pm = meta.Properties;
    if (pm == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    auto prop = pm->Properties.try_get(FixedStringUnhashed(field.c_str()));
    if (prop == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    auto base = (uint8_t*)world->GetRawComponent(entity, *index, meta.InlineSize);
    if (base == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    // A proxy component stores a pointer to the real object; the field lives there, not inline.
    if (meta.IsProxy) {
        base = (uint8_t*)ecs::DereferenceProxyComponent(base);
        if (base == nullptr) {
            lua_pushnil(L);
            return 1;
        }
    }

    lua_newtable(L);
    settable(L, "Address", (int64_t)(base + prop->Offset));
    settable(L, "Offset", (int64_t)prop->Offset);
    settable(L, "ComponentAddress", (int64_t)base);

    // The field's type name and, when it is a scalar, its width - so a reader knows what to ask
    // ReadValue for. The name comes from the type information's member list where it has one.
    if (pm->TypeInfo != nullptr) {
        auto member = pm->TypeInfo->Members.try_get(FixedString(field.c_str()));
        if (member != nullptr && *member) {
            auto const& ty = member->Get();
            settable(L, "Type", ty.TypeName.GetString());
            if (auto scalar = FindScalarType(ty.TypeName.GetStringView())) {
                settable(L, "Size", (int32_t)scalar->Size);
                settable(L, "Scalar", true);
            }
        }
    }

    return 1;
}

// Ext.Debug.ModuleFromAddress(address) -> { Module, RVA, Base, Size } or nil: which loaded module
// holds the address, and the offset inside it. This is the `module+0xRVA` the decompiler speaks.
UserReturn ModuleFromAddress(lua_State* L, int64_t address)
{
    RequireDeveloperMode(L, "ModuleFromAddress");
    HMODULE module{ nullptr };
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)address, &module) || module == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    auto base = (uint8_t const*)module;
    auto dos = (IMAGE_DOS_HEADER const*)base;
    auto nt = (IMAGE_NT_HEADERS const*)(base + dos->e_lfanew);
    auto size = nt->OptionalHeader.SizeOfImage;

    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(module, path, (DWORD)std::size(path));
    STDString name;
    if (len > 0) {
        std::wstring_view view(path, len);
        auto slash = view.find_last_of(L"\\/");
        name = ToStdUTF8(slash == std::wstring_view::npos ? view : view.substr(slash + 1)).c_str();
    }

    lua_newtable(L);
    settable(L, "Module", name);
    settable(L, "RVA", (int64_t)((uint8_t const*)address - base));
    settable(L, "Base", (int64_t)base);
    settable(L, "Size", (int32_t)size);
    return 1;
}

// Ext.Debug.AddressFromModule(module, rva) -> absolute address of module+rva, or nil if the module
// is not loaded or the offset is past its image. `module` is a loaded module's name (bg3_dx11.exe).
UserReturn AddressFromModule(lua_State* L, STDString module, int64_t rva)
{
    RequireDeveloperMode(L, "AddressFromModule");
    auto handle = GetModuleHandleW(FromUTF8(module).c_str());
    if (handle == nullptr || rva < 0) {
        lua_pushnil(L);
        return 1;
    }

    auto base = (uint8_t const*)handle;
    auto dos = (IMAGE_DOS_HEADER const*)base;
    auto nt = (IMAGE_NT_HEADERS const*)(base + dos->e_lfanew);
    if ((uint64_t)rva >= nt->OptionalHeader.SizeOfImage) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)(base + rva));
    return 1;
}

// Ext.Debug.ReadValue(address, type) -> the scalar at the address read as `type`, or nil if the
// bytes are not all readable. `type` is one of the scalar type names (int32, float, bool, ...).
// Bounds-checked against committed readable memory, so a wrong address returns nil, never a crash.
UserReturn ReadValue(lua_State* L, int64_t address, STDString type)
{
    RequireDeveloperMode(L, "ReadValue");
    auto scalar = FindScalarType(type);
    if (scalar == nullptr) {
        return luaL_error(L, "ReadValue: unknown scalar type '%s'", type.c_str());
    }

    auto ptr = (uint8_t const*)address;
    if (!IsReadableRange(ptr, scalar->Size)) {
        lua_pushnil(L);
        return 1;
    }

    switch (scalar->Repr) {
    case ScalarType::Bool:
        lua_pushboolean(L, *ptr != 0);
        break;
    case ScalarType::Float:
        if (scalar->Size == 4) lua_pushnumber(L, (lua_Number)*(float const*)ptr);
        else lua_pushnumber(L, (lua_Number)*(double const*)ptr);
        break;
    case ScalarType::Unsigned:
        switch (scalar->Size) {
        case 1: lua_pushinteger(L, (lua_Integer)*(uint8_t const*)ptr); break;
        case 2: lua_pushinteger(L, (lua_Integer)*(uint16_t const*)ptr); break;
        case 4: lua_pushinteger(L, (lua_Integer)*(uint32_t const*)ptr); break;
        default: lua_pushinteger(L, (lua_Integer)*(uint64_t const*)ptr); break;
        }
        break;
    default: // Signed
        switch (scalar->Size) {
        case 1: lua_pushinteger(L, (lua_Integer)*(int8_t const*)ptr); break;
        case 2: lua_pushinteger(L, (lua_Integer)*(int16_t const*)ptr); break;
        case 4: lua_pushinteger(L, (lua_Integer)*(int32_t const*)ptr); break;
        default: lua_pushinteger(L, (lua_Integer)*(int64_t const*)ptr); break;
        }
        break;
    }

    return 1;
}

// Whether [address, address + size) is committed, writable and NOT executable: a write is refused
// on a code page, so this cannot be used to patch the game's instructions, only its data.
static bool IsWritableDataRange(uint8_t const* address, std::size_t size)
{
    if (size == 0) return false;
    auto const* p = address;
    auto const* end = address + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        auto protect = mbi.Protect & 0xff;
        bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY;
        if (!writable || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        p = (uint8_t const*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// Pack a Lua number/boolean as a scalar of `scalar` into `out` (scalar->Size bytes).
static void PackScalar(ScalarType const& scalar, lua_State* L, int index, uint8_t* out)
{
    switch (scalar.Repr) {
    case ScalarType::Bool:
        *out = lua_toboolean(L, index) ? 1 : 0;
        break;
    case ScalarType::Float:
        if (scalar.Size == 4) { float v = (float)lua_tonumber(L, index); memcpy(out, &v, 4); }
        else { double v = (double)lua_tonumber(L, index); memcpy(out, &v, 8); }
        break;
    case ScalarType::Unsigned: {
        uint64_t v = (uint64_t)lua_tointeger(L, index);
        memcpy(out, &v, scalar.Size);
        break;
    }
    default: {
        int64_t v = (int64_t)lua_tointeger(L, index);
        memcpy(out, &v, scalar.Size);
        break;
    }
    }
}

// Ext.Debug.WriteValue(address, type, value) -> true if written. Bounds-checked against committed
// writable data memory; a code page or an unmapped address is refused and returns false. `type` is
// one of the scalar type names. Writing a wrong field still corrupts the game's state: the check is
// only that the bytes are safe to store, not that they mean anything there.
bool WriteValue(lua_State* L, int64_t address, STDString type)
{
    RequireDeveloperMode(L, "WriteValue");
    auto scalar = FindScalarType(type);
    if (scalar == nullptr) {
        luaL_error(L, "WriteValue: unknown scalar type '%s'", type.c_str());
        return false;
    }

    uint8_t bytes[8];
    PackScalar(*scalar, L, 3, bytes);

    auto ptr = (uint8_t*)address;
    if (!IsWritableDataRange(ptr, scalar->Size)) {
        return false;
    }

    memcpy(ptr, bytes, scalar->Size);
    return true;
}

// --- Who writes this field: a hardware watchpoint on write -------------------------------------
// Up to four addresses are watched through the CPU's own debug registers (DR0-DR3, DR7), armed on
// every thread of the process. When one is written, the CPU raises a single-step exception that a
// vectored handler catches: it records the writing instruction and a short return chain as raw
// addresses, the thread, the value before and after, resolves nothing, and resumes at once. The
// Lua side reads the ring buffer and turns each address into module+0xRVA (ModuleFromAddress),
// which the decompiler database turns into a function name. Developer mode only.
//
// Stability: the handler does no allocation and takes only a spinlock, so it is safe on any thread;
// exceptions that are not ours (a single-step with no debug-register status, any other code) are
// passed on untouched. Threads created after arming are picked up by re-arming on each WatchPoll.
// The debug registers belong to whoever holds the thread last: another debugger or anti-tamper
// would contend, but this game ships with none.
//
// The handler never reads the watched address itself: it reads through the kernel
// (ReadProcessMemory), where the user-mode debug registers are not live, so a watch on reads does
// not trap again inside its own handler. Once installed the handler stays installed: a thread whose
// registers still hold a cleared watch (not yet disarmed, or armed with an older set) is answered
// and given the current registers, never left with an exception nobody claims.
//
// A hardware data breakpoint is a trap: it fires after the access, so the address the CPU reports
// is the instruction that FOLLOWS the one that accessed the field (NextInstruction). The accessing
// instruction is the one before it in the function's listing.

namespace {

struct Watchpoint
{
    uint64_t Address{ 0 };
    uint32_t Size{ 0 };          // 1, 2, 4 or 8
    bool ReadWrite{ false };     // false: on write only; true: on read or write
    bool Enabled{ false };
    // The value as last seen: copied when the watch is armed and after every hit, so a hit's
    // Before is the value just before its write - unless something changed it unseen in between
    // (a thread not armed yet, a write from kernel mode such as file I/O into the buffer).
    uint64_t Copy{ 0 };
    // The side whose Lua armed it (InstrumentOwner); it lists, reads and clears it
    uint8_t Owner{ 0 };
    // That side's tracer log, whose running Frame and Flush counters stamp the hits
    ecs::ECSChangeLog const* Log{ nullptr };
};

struct WatchHit
{
    uint64_t NextInstruction{ 0 }; // absolute address of the instruction after the access (ctx.Rip)
    uint64_t Frames[6]{};        // return chain, absolute addresses (Frames[0] == NextInstruction)
    uint32_t FrameCount{ 0 };
    uint32_t Thread{ 0 };
    uint64_t Before{ 0 };
    uint64_t After{ 0 };
    uint32_t Slot{ 0 };
    uint8_t Owner{ 0 };
    uint64_t Poll{ 0 };          // the owner's extender tick count at the time
    uint32_t EcsFrame{ 0 };      // the owner's ECS frame and ECB flush, as on the tracer's rows
    uint32_t EcsFlush{ 0 };
    int64_t Time{ 0 };          // QueryPerformanceCounter, for a hits-per-second rate
};

// Which side an instrument belongs to: the side whose Lua made it
enum InstrumentOwner : uint8_t
{
    OwnerNone = 0,
    OwnerServer = 1,
    OwnerClient = 2
};

static constexpr uint32_t WatchSlots = 4;
static constexpr uint32_t HitRingSize = 512;

Watchpoint gWatches[WatchSlots];
WatchHit gHitRing[HitRingSize];
std::atomic<uint64_t> gHitHead{ 0 };     // total hits ever recorded (monotonic)
std::atomic<uint64_t> gHitRead[3]{};     // per owner: the next hit to hand to its Lua
std::atomic<uint64_t> gWatchPoll[3]{};   // per owner: extender ticks counted
std::atomic_flag gHitLock = ATOMIC_FLAG_INIT;
// Serialises arming, clearing and freezing between the server's and the client's threads (never
// taken by the exception handler)
std::recursive_mutex gInstrumentMutex;
uint64_t gLastRearm{ 0 };
void* gVeh{ nullptr };
bool gWatchActive{ false };
// Set when the handler first runs on a thread and cleared when it leaves; a nested single-step
// on the same thread is the handler's own doing and is only acknowledged.
thread_local bool tInWatchHandler{ false };

// Build the DR7 value for the current enabled watches.
uint64_t ComposeDr7()
{
    uint64_t dr7 = 0;
    for (uint32_t i = 0; i < WatchSlots; i++) {
        if (!gWatches[i].Enabled) continue;
        dr7 |= (1ull << (i * 2));                 // local enable Li
        uint64_t rw = gWatches[i].ReadWrite ? 0b11 : 0b01;
        uint64_t len = gWatches[i].Size == 1 ? 0b00
                     : gWatches[i].Size == 2 ? 0b01
                     : gWatches[i].Size == 8 ? 0b10
                     : 0b11;                       // 4 bytes
        dr7 |= rw << (16 + i * 4);
        dr7 |= len << (18 + i * 4);
    }
    return dr7;
}

void ApplyToContext(CONTEXT& ctx)
{
    ctx.Dr0 = gWatches[0].Enabled ? gWatches[0].Address : 0;
    ctx.Dr1 = gWatches[1].Enabled ? gWatches[1].Address : 0;
    ctx.Dr2 = gWatches[2].Enabled ? gWatches[2].Address : 0;
    ctx.Dr3 = gWatches[3].Enabled ? gWatches[3].Address : 0;
    ctx.Dr7 = ComposeDr7();
}

// Set the debug registers on one thread by id. The current thread is written without suspending.
void ArmThread(DWORD threadId)
{
    bool self = (threadId == GetCurrentThreadId());
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (thread == nullptr) return;

    if (!self) SuspendThread(thread);

    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(thread, &ctx)) {
        ApplyToContext(ctx);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(thread, &ctx);
    }

    if (!self) ResumeThread(thread);
    CloseHandle(thread);
}

// Arm (or clear, when nothing is enabled) every thread of this process.
void ArmAllThreads()
{
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                ArmThread(te.th32ThreadID);
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// Read up to `size` bytes at `address` into a 64-bit value, or 0 if they cannot be read. The copy is
// made by the kernel, where this thread's user-mode debug registers do not fire: reading a field
// under a read watch from here does not trap.
uint64_t SafeReadScalar(uint64_t address, uint32_t size)
{
    uint64_t v = 0;
    SIZE_T read{ 0 };
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, &v, size, &read) || read != size) return 0;
    return v;
}

// A safe x64 stack walk from an interrupted context, up to `maxFrames` (Frames[0] is the faulting
// Rip). Isolated in its own function so __try/__except shares no scope with C++ unwinding. Returns
// the number of frames written. Only POD locals here.
uint32_t SafeStackWalk(CONTEXT const* ctx, uint64_t* frames, uint32_t maxFrames)
{
    uint32_t count = 0;
    if (maxFrames == 0) return 0;
    frames[0] = ctx->Rip;
    count = 1;
    __try {
        CONTEXT walk = *ctx;
        for (uint32_t f = 1; f < maxFrames; f++) {
            DWORD64 imageBase = 0;
            auto fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
            if (fn == nullptr) break;
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
            if (walk.Rip == 0) break;
            frames[f] = walk.Rip;
            count = f + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // leave what was gathered
    }
    return count;
}

// The vectored exception handler. Minimal and allocation-free.
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ex)
{
    auto rec = ex->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;

    auto ctx = ex->ContextRecord;
    uint64_t status = ctx->Dr6 & 0xf;              // B0-B3: which watchpoint fired
    if (status == 0) return EXCEPTION_CONTINUE_SEARCH;
    // Nothing of ours was ever armed: the debug registers are someone else's
    if (gVeh == nullptr) return EXCEPTION_CONTINUE_SEARCH;

    if (tInWatchHandler) {
        // Our own access, made from inside the handler on this thread (the stack walk reading a
        // watched stack slot, say): acknowledge it and go on.
        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    tInWatchHandler = true;

    // Whether the interrupted thread's registers came with the context: then a slot is ours only
    // if the thread was armed with the address the slot holds now (a thread armed with an older
    // set may fire for an address that was cleared or replaced since).
    bool haveRegisters = (ctx->ContextFlags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS;
    uint64_t const armedAt[WatchSlots] = { ctx->Dr0, ctx->Dr1, ctx->Dr2, ctx->Dr3 };
    uint32_t recorded = 0;

    for (uint32_t i = 0; i < WatchSlots; i++) {
        if (!(status & (1ull << i))) continue;
        auto& watch = gWatches[i];
        if (!watch.Enabled) continue;              // cleared: answered below, not recorded
        if (haveRegisters && armedAt[i] != watch.Address) continue;

        WatchHit hit;
        hit.Slot = i;
        hit.NextInstruction = ctx->Rip;
        hit.Thread = GetCurrentThreadId();
        hit.Owner = watch.Owner;
        hit.Poll = gWatchPoll[watch.Owner].load(std::memory_order_relaxed);
        if (watch.Log != nullptr) {
            hit.EcsFrame = watch.Log->Frame;
            hit.EcsFlush = watch.Log->Flush;
        }
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        hit.Time = t.QuadPart;

        uint64_t after = SafeReadScalar(watch.Address, watch.Size);
        hit.Before = watch.Copy;
        hit.After = after;
        watch.Copy = after;

        // A safe x64 stack walk from the interrupted context: a few frames, guarded.
        hit.FrameCount = SafeStackWalk(ctx, hit.Frames, 6);

        while (gHitLock.test_and_set(std::memory_order_acquire)) { /* brief spin */ }
        uint64_t head = gHitHead.load(std::memory_order_relaxed);
        gHitRing[head % HitRingSize] = hit;
        gHitHead.store(head + 1, std::memory_order_relaxed);
        gHitLock.clear(std::memory_order_release);
        recorded++;
    }

    // A trap-flag single step (BS) with only stale B0-B3 bits is someone else's: pass it on.
    if (recorded == 0 && (ctx->Dr6 & 0x4000) != 0) {
        tInWatchHandler = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Every single-step with a B0-B3 bit is answered while the handler is installed, ours or a
    // leftover of a cleared watch; the thread leaves with the registers as they are now, so a
    // cleared watch stops firing on it.
    if (haveRegisters) {
        ApplyToContext(*ctx);
    }
    ctx->Dr6 = 0;                                  // acknowledge
    tInWatchHandler = false;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// The side a Lua state belongs to, as the owner of what it arms
static uint8_t OwnerOf(lua_State* L)
{
    return State::FromLua(L)->IsClient() ? OwnerClient : OwnerServer;
}

static char const* OwnerName(uint8_t owner)
{
    switch (owner) {
    case OwnerServer: return "Server";
    case OwnerClient: return "Client";
    default: return "None";
    }
}

// Arm one slot; the caller holds gInstrumentMutex. Returns the slot index or -1 when none is free.
static int ArmSlot(lua_State* L, uint64_t address, uint32_t size, bool readWrite)
{
    int slot = -1;
    for (uint32_t i = 0; i < WatchSlots; i++) {
        if (!gWatches[i].Enabled) { slot = (int)i; break; }
    }
    if (slot < 0) return -1;

    auto& watch = gWatches[slot];
    watch.Address = address;
    watch.Size = size;
    watch.ReadWrite = readWrite;
    // The value before any hit, so the first hit's Before is true too
    watch.Copy = SafeReadScalar(address, size);
    watch.Owner = OwnerOf(L);
    watch.Log = &State::FromLua(L)->GetEntitySystemHelpers().GetTracer().GetLog();
    watch.Enabled = true;

    if (gVeh == nullptr) {
        gVeh = AddVectoredExceptionHandler(1, &WatchHandler);
    }
    gWatchActive = true;
    return slot;
}

static void UpdateWatchActive()
{
    bool anyEnabled = false;
    for (auto const& w : gWatches) anyEnabled = anyEnabled || w.Enabled;
    gWatchActive = anyEnabled;
}

} // anonymous namespace

// Ext.Debug.WatchWrite(address, size[, readWrite]) -> slot index (1-4) or nil if none free.
// Arms a hardware watchpoint: the writing instruction of the next write to this address is recorded.
// size is 1, 2, 4 or 8; the address must be aligned to its size (the CPU requires it). readWrite
// true fires on reads as well as writes. The watch belongs to the calling side (server or client):
// only that side lists its hits, reads them and clears it.
UserReturn WatchWrite(lua_State* L, int64_t address, int32_t size, std::optional<bool> readWrite)
{
    RequireDeveloperMode(L, "WatchWrite");
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return luaL_error(L, "WatchWrite: size must be 1, 2, 4 or 8");
    }
    if ((uint64_t)address % (uint64_t)size != 0) {
        return luaL_error(L, "WatchWrite: address must be aligned to its size");
    }

    std::lock_guard _(gInstrumentMutex);
    auto slot = ArmSlot(L, (uint64_t)address, (uint32_t)size, readWrite.value_or(false));
    if (slot < 0) {
        lua_pushnil(L);
        return 1;
    }

    ArmAllThreads();
    lua_pushinteger(L, slot + 1);
    return 1;
}

// Ext.Debug.WatchPoll() -> the number of this side's hits waiting. Polling no longer depends on
// it: the extender counts its own ticks, re-arms new threads and applies freezes every tick of
// each side (OnExtenderTick), so a watch left armed across a Lua reset or a load stays served.
UserReturn WatchPoll(lua_State* L)
{
    RequireDeveloperMode(L, "WatchPoll");
    auto owner = OwnerOf(L);
    uint64_t waiting = 0;
    uint64_t head = gHitHead.load();
    uint64_t read = gHitRead[owner].load();
    if (head - read > HitRingSize) read = head - HitRingSize;
    for (uint64_t n = read; n < head; n++) {
        if (gHitRing[n % HitRingSize].Owner == owner) waiting++;
    }
    lua_pushinteger(L, (lua_Integer)waiting);
    return 1;
}

// Ext.Debug.ReadWatchHits([max]) -> this side's hits recorded since its last read, each:
//   { Slot, Owner, Thread, Before, After, Changed, Frame, Flush, Poll, Time,
//     NextInstruction = {Module, RVA, Address}, Instruction (legacy name of NextInstruction),
//     Return = { {Module, RVA, Address}, ... } }
// Before is the value copied at arming or at the slot's previous hit; After is read at the hit.
// Frame and Flush are the side's ECS frame and ECB flush, the same counters as on the tracer's
// rows; Poll is the side's extender tick count.
UserReturn ReadWatchHits(lua_State* L, std::optional<int32_t> max)
{
    RequireDeveloperMode(L, "ReadWatchHits");
    auto owner = OwnerOf(L);

    auto resolve = [L](uint64_t addr) {
        HMODULE module{ nullptr };
        if (addr != 0 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)addr, &module) && module != nullptr) {
            auto base = (uint8_t const*)module;
            wchar_t path[MAX_PATH];
            DWORD len = GetModuleFileNameW(module, path, (DWORD)std::size(path));
            STDString name;
            if (len > 0) {
                std::wstring_view view(path, len);
                auto slash = view.find_last_of(L"\\/");
                name = ToStdUTF8(slash == std::wstring_view::npos ? view : view.substr(slash + 1)).c_str();
            }
            lua_newtable(L);
            settable(L, "Module", name);
            settable(L, "RVA", (int64_t)((uint8_t const*)addr - base));
            settable(L, "Address", (int64_t)addr);
        } else {
            lua_newtable(L);
            settable(L, "Address", (int64_t)addr);
        }
    };

    uint64_t head = gHitHead.load();
    uint64_t read = gHitRead[owner].load();
    if (head - read > HitRingSize) {
        read = head - HitRingSize;          // the oldest kept hit
    }
    uint64_t limit = (uint64_t)max.value_or(0x7fffffff);

    lua_newtable(L);
    int index = 1;
    uint64_t n = read;
    for (; n < head && (uint64_t)(index - 1) < limit; n++) {
        WatchHit hit;
        while (gHitLock.test_and_set(std::memory_order_acquire)) {}
        hit = gHitRing[n % HitRingSize];
        gHitLock.clear(std::memory_order_release);
        if (hit.Owner != owner) continue;

        lua_newtable(L);
        settable(L, "Slot", (int32_t)(hit.Slot + 1));
        settable(L, "Owner", OwnerName(hit.Owner));
        settable(L, "Thread", (int32_t)hit.Thread);
        settable(L, "Before", (int64_t)hit.Before);
        settable(L, "After", (int64_t)hit.After);
        // A read, or a write of the same value, leaves Before == After
        settable(L, "Changed", hit.Before != hit.After);
        settable(L, "Frame", (int64_t)hit.EcsFrame);
        settable(L, "Flush", (int64_t)hit.EcsFlush);
        settable(L, "Poll", (int64_t)hit.Poll);
        settable(L, "Time", (int64_t)hit.Time);

        // The instruction after the access (a data breakpoint is a trap)
        lua_pushstring(L, "NextInstruction");
        resolve(hit.NextInstruction);
        lua_rawset(L, -3);
        // Legacy name of NextInstruction
        lua_pushstring(L, "Instruction");
        resolve(hit.NextInstruction);
        lua_rawset(L, -3);

        lua_pushstring(L, "Return");
        lua_newtable(L);
        for (uint32_t f = 0; f < hit.FrameCount; f++) {
            resolve(hit.Frames[f]);
            lua_rawseti(L, -2, (int)f + 1);
        }
        lua_rawset(L, -3);

        lua_rawseti(L, -2, index++);
    }

    gHitRead[owner].store(n);
    return 1;
}

// Ext.Debug.ClearWatch([slot]) -> true if anything was cleared. Without a slot, clears every watch
// of the calling side; with a slot (1-4), clears it if the calling side owns it. The other side's
// watches are left alone. The handler stays installed, so a thread that still fires for a cleared
// watch before it is disarmed is answered, not left to crash.
bool ClearWatch(lua_State* L, std::optional<int32_t> slot)
{
    RequireDeveloperMode(L, "ClearWatch");
    auto owner = OwnerOf(L);
    if (slot && (*slot < 1 || *slot > (int)WatchSlots)) {
        luaL_error(L, "ClearWatch: slot must be 1-4");
        return false;
    }

    std::lock_guard _(gInstrumentMutex);
    bool cleared = false;
    for (uint32_t i = 0; i < WatchSlots; i++) {
        if (slot && (uint32_t)(*slot - 1) != i) continue;
        if (gWatches[i].Enabled && gWatches[i].Owner == owner) {
            gWatches[i] = Watchpoint{};
            cleared = true;
        }
    }

    if (cleared) {
        ArmAllThreads();                  // writes the new (possibly empty) DR7 to every thread
        UpdateWatchActive();
    }
    return cleared;
}

// Ext.Debug.ListWatches([all]) -> { { Slot, Owner, Address, Size, ReadWrite, Value } } for the
// calling side's watches, or for both sides' with all = true. Value is the copy the next hit's
// Before will be.
UserReturn ListWatches(lua_State* L, std::optional<bool> all)
{
    RequireDeveloperMode(L, "ListWatches");
    auto owner = OwnerOf(L);
    std::lock_guard _(gInstrumentMutex);
    lua_newtable(L);
    int index = 1;
    for (uint32_t i = 0; i < WatchSlots; i++) {
        auto const& w = gWatches[i];
        if (!w.Enabled || (!all.value_or(false) && w.Owner != owner)) continue;
        lua_newtable(L);
        settable(L, "Slot", (int32_t)(i + 1));
        settable(L, "Owner", OwnerName(w.Owner));
        settable(L, "Address", (int64_t)w.Address);
        settable(L, "Size", (int32_t)w.Size);
        settable(L, "ReadWrite", w.ReadWrite);
        settable(L, "Value", (int64_t)w.Copy);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// --- Freeze: hold an address at a value ---------------------------------------------------------
// The value is written back every tick of the owning side by the extender itself (OnExtenderTick),
// rather than by turning writes back inside the exception handler: that keeps the handler off the
// hot path, the watchpoint free for its real job, and a freeze trivially removable. Developer mode
// only.

namespace {

struct FreezeHold
{
    uint64_t Address{ 0 };
    uint32_t Size{ 0 };
    uint8_t Bytes[8]{};
    bool Active{ false };
    uint8_t Owner{ 0 };
};

static constexpr uint32_t FreezeSlots = 16;
FreezeHold gFreezes[FreezeSlots];

uint32_t ApplyFreezes(uint8_t owner)
{
    uint32_t written = 0;
    for (auto& f : gFreezes) {
        if (!f.Active || f.Owner != owner) continue;
        if (IsWritableDataRange((uint8_t const*)f.Address, f.Size)) {
            memcpy((void*)f.Address, f.Bytes, f.Size);
            written++;
        }
    }
    return written;
}

} // anonymous namespace

// Called by each side's extension state every tick, whether or not its Lua is running: counts the
// side's ticks for the hits, re-arms threads made since arming (at most once a second, since
// arming suspends every thread) and writes the side's frozen values.
void OnExtenderTick(bool server)
{
    uint8_t owner = server ? OwnerServer : OwnerClient;
    gWatchPoll[owner].fetch_add(1, std::memory_order_relaxed);

    if (!gExtender->GetConfig().DeveloperMode) return;

    std::lock_guard _(gInstrumentMutex);
    if (gWatchActive) {
        auto now = GetTickCount64();
        if (now - gLastRearm >= 1000) {
            gLastRearm = now;
            ArmAllThreads();
        }
    }

    ApplyFreezes(owner);
}

// Ext.Debug.Freeze(address, type, value) -> slot index or nil if none free. From now until it is
// cleared, the extender rewrites this scalar every tick of the calling side.
UserReturn HoldValue(lua_State* L, int64_t address, STDString type)
{
    RequireDeveloperMode(L, "Freeze");
    auto scalar = FindScalarType(type);
    if (scalar == nullptr) {
        return luaL_error(L, "Freeze: unknown scalar type '%s'", type.c_str());
    }

    std::lock_guard _(gInstrumentMutex);
    int slot = -1;
    for (uint32_t i = 0; i < FreezeSlots; i++) {
        if (!gFreezes[i].Active) { slot = (int)i; break; }
    }
    if (slot < 0) {
        lua_pushnil(L);
        return 1;
    }

    gFreezes[slot].Address = (uint64_t)address;
    gFreezes[slot].Size = scalar->Size;
    PackScalar(*scalar, L, 3, gFreezes[slot].Bytes);
    gFreezes[slot].Owner = OwnerOf(L);
    gFreezes[slot].Active = true;

    lua_pushinteger(L, slot + 1);
    return 1;
}

// Ext.Debug.FreezeApply() -> how many of the calling side's frozen values were written now. Not
// needed any more (the extender applies them every tick); kept for scripts that call it.
UserReturn FreezeApply(lua_State* L)
{
    RequireDeveloperMode(L, "FreezeApply");
    std::lock_guard _(gInstrumentMutex);
    lua_pushinteger(L, ApplyFreezes(OwnerOf(L)));
    return 1;
}

// Ext.Debug.ClearFreezes([slot]) -> true if anything was cleared. Stops holding every address the
// calling side froze, or just one slot of its own.
bool ClearFreezes(lua_State* L, std::optional<int32_t> slot)
{
    RequireDeveloperMode(L, "ClearFreezes");
    auto owner = OwnerOf(L);
    std::lock_guard _(gInstrumentMutex);
    bool cleared = false;
    for (uint32_t i = 0; i < FreezeSlots; i++) {
        if (slot && (uint32_t)(*slot - 1) != i) continue;
        if (gFreezes[i].Active && gFreezes[i].Owner == owner) {
            gFreezes[i] = FreezeHold{};
            cleared = true;
        }
    }
    return cleared;
}

// Ext.Debug.ListFreezes([all]) -> { { Slot, Owner, Address, Size } } for every address the calling
// side holds now, or both sides' with all = true.
UserReturn ListFreezes(lua_State* L, std::optional<bool> all)
{
    RequireDeveloperMode(L, "ListFreezes");
    auto owner = OwnerOf(L);
    std::lock_guard _(gInstrumentMutex);
    lua_newtable(L);
    int index = 1;
    for (uint32_t i = 0; i < FreezeSlots; i++) {
        if (!gFreezes[i].Active || (!all.value_or(false) && gFreezes[i].Owner != owner)) continue;
        lua_newtable(L);
        settable(L, "Slot", (int32_t)(i + 1));
        settable(L, "Owner", OwnerName(gFreezes[i].Owner));
        settable(L, "Address", (int64_t)gFreezes[i].Address);
        settable(L, "Size", (int32_t)gFreezes[i].Size);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// Ext.Debug.GetSystemAddress(name) -> { Address, Index, Activated, UpdateProc } or nil.
// Works for systems the extender has no layout for; name is the engine's own
// (as GetSystemTypes() lists it). The first 16 bytes at Address are the virtual table
// pointer and the EntityWorld pointer every system starts with.
UserReturn GetSystemAddress(lua_State* L, STDString name)
{
    RequireDeveloperMode(L, "GetSystemAddress");
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto world = ecs.GetEntityWorld();

    std::optional<ecs::SystemTypeIndex> index;
    ecs.ForEachSystemType([&](STDString const& systemName, ecs::SystemTypeIndex type, bool) {
        if (systemName == name) index = type;
    });

    if (world == nullptr || !index || (unsigned)*index >= world->Systems.Systems.size()) {
        lua_pushnil(L);
        return 1;
    }

    auto const& entry = world->Systems.Systems[(unsigned)*index];
    if (entry.System == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    settable(L, "Address", (int64_t)entry.System);
    settable(L, "Index", (int32_t)*index);
    settable(L, "Activated", entry.Activated);
    settable(L, "UpdateProc", (int64_t)entry.UpdateProc);
    return 1;
}

// Ext.Debug.GetComponentHolders(name[, max]) -> the entities that carry the component now.
// Unlike Ext.Entity.GetAllEntitiesWithComponent it takes the engine's own name and works for
// types the extender has no layout for, one-frame components included. It walks the storage
// classes, not the entities, so it is cheap enough to call every tick.
Array<EntityHandle> GetComponentHolders(lua_State* L, STDString name, std::optional<int32_t> max)
{
    RequireDeveloperMode(L, "GetComponentHolders");
    Array<EntityHandle> entities;
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto world = ecs.GetEntityWorld();
    auto type = ecs.GetComponentIndex(name);
    if (world == nullptr || !type) return entities;

    auto limit = (uint32_t)max.value_or(0x7fffffff);
    for (auto cls : world->Storage->Storages) {
        if (ecs::IsOneFrame(*type)) {
            if (!cls->HasOneFrameComponents) continue;
            auto pool = cls->OneFrameComponents.try_get(*type);
            if (!pool) continue;
            for (auto const& entity : pool->keys()) {
                if (entities.size() >= limit) return entities;
                entities.push_back(entity);
            }
        } else if (cls->ComponentTypeToIndex.try_get(*type)) {
            for (auto const& entity : cls->InstanceToPageMap.keys()) {
                if (entities.size() >= limit) return entities;
                entities.push_back(entity);
            }
        }
    }

    return entities;
}

// Ext.Debug.GetComponentCounts() -> { [engine name] = number of entities carrying it now },
// for every component type that has at least one holder, mapped by the extender or not.
UserReturn GetComponentCounts(lua_State* L)
{
    RequireDeveloperMode(L, "GetComponentCounts");
    auto& ecs = State::FromLua(L)->GetEntitySystemHelpers();
    auto world = ecs.GetEntityWorld();

    std::unordered_map<uint16_t, uint32_t> counts;
    if (world != nullptr) {
        for (auto cls : world->Storage->Storages) {
            auto entities = (uint32_t)cls->InstanceToPageMap.keys().size();
            if (entities > 0) {
                for (auto type : cls->ComponentTypeToIndex.keys()) {
                    counts[(uint16_t)type] += entities;
                }
            }
            if (cls->HasOneFrameComponents) {
                for (auto it : cls->OneFrameComponents) {
                    counts[(uint16_t)it->Key()] += (uint32_t)it->Value().keys().size();
                }
            }
        }
    }

    lua_newtable(L);
    for (auto const& [type, count] : counts) {
        auto name = ecs.GetComponentName(ecs::ComponentTypeIndex{ type });
        if (name != nullptr && count > 0) {
            lua_pushlstring(L, name->data(), name->size());
            lua_pushinteger(L, (lua_Integer)count);
            lua_rawset(L, -3);
        }
    }
    return 1;
}

void LuaDebugBreak(lua_State* L)
{
    auto debugger = gExtender->GetLuaDebugger();
    if (debugger) {
        debugger->DebugBreak(L);
    }
}

// Recurses until the stack runs out (Crash(3)); noinline so the optimiser cannot fold it away
static __declspec(noinline) uint64_t OverflowStack(volatile uint64_t depth)
{
    volatile uint8_t frame[1024];
    frame[0] = (uint8_t)depth;
    return OverflowStack(depth + 1) + frame[0];
}

// Ext.Debug.Crash(type) - closes the game on purpose, to test crash reporting: 0 divides by zero,
// 1 aborts, 2 writes to a null pointer, 3 overflows the stack. Developer mode only (debug builds:
// always).
void Crash(lua_State* L, int type)
{
#if !defined(_DEBUG)
    RequireDeveloperMode(L, "Crash");
#endif
    switch (type) {
    case 0:
    {
        int d = 0;
        std::cout << (1 / d);
        break;
    }

    case 1:
        abort();
        break;

    case 2:
        *(uint32_t volatile*)0 = 0;
        break;

    case 3:
        OverflowStack(0);
        break;
    }
}

void SetEntityRuntimeCheckLevel(int level)
{
#if defined(_DEBUG)
    if (level >= (int)ecs::RuntimeCheckLevel::None && level <= (int)ecs::RuntimeCheckLevel::FullECS) {
#else
    if (level >= (int)ecs::RuntimeCheckLevel::Once && level <= (int)ecs::RuntimeCheckLevel::FullECS) {
#endif
        ecs::EntitySystemHelpersBase::CheckLevel = (ecs::RuntimeCheckLevel)level;
    } else {
        OsiError("Unsupported check level: " << level);
    }
}

void Reset(std::optional<bool> server, std::optional<bool> client)
{
    if (!gExtender->GetConfig().DeveloperMode) {
        ERR("Reset() only available in developer mode");
        return;
    }

    if (server.value_or(true)) {
        gExtender->GetServer().EnqueueTask([] () {
            gExtender->GetServer().ResetLuaState();
        });
    }

    if (client.value_or(true)) {
        gExtender->GetClient().EnqueueTask([] () {
            gExtender->GetClient().ResetLuaState();
        });
    }
}

void RegisterDebugLib()
{
    DECLARE_MODULE(Debug, Both)
    BEGIN_MODULE()
    MODULE_FUNCTION(DumpStack)
    MODULE_FUNCTION(DebugDumpLifetimes)
    MODULE_FUNCTION(GenerateIdeHelpers)
    MODULE_NAMED_FUNCTION("DebugBreak", LuaDebugBreak)
    MODULE_FUNCTION(IsDeveloperMode)
    MODULE_FUNCTION(ReadMemory)
    MODULE_FUNCTION(WriteMemory)
    MODULE_FUNCTION(GetModuleInfo)
    MODULE_FUNCTION(GetComponentTypes)
    MODULE_FUNCTION(GetSystemTypes)
    MODULE_FUNCTION(GetComponentAddress)
    MODULE_FUNCTION(GetSystemAddress)
    MODULE_FUNCTION(GetComponentHolders)
    MODULE_FUNCTION(GetComponentCounts)
    MODULE_FUNCTION(GetFieldAddress)
    MODULE_FUNCTION(ModuleFromAddress)
    MODULE_FUNCTION(AddressFromModule)
    MODULE_FUNCTION(ReadValue)
    MODULE_FUNCTION(WriteValue)
    MODULE_FUNCTION(WatchWrite)
    MODULE_FUNCTION(WatchPoll)
    MODULE_FUNCTION(ReadWatchHits)
    MODULE_FUNCTION(ClearWatch)
    MODULE_FUNCTION(ListWatches)
    MODULE_NAMED_FUNCTION("Freeze", HoldValue)
    MODULE_FUNCTION(FreezeApply)
    MODULE_FUNCTION(ClearFreezes)
    MODULE_FUNCTION(ListFreezes)
    MODULE_FUNCTION(SetEntityRuntimeCheckLevel)
    MODULE_FUNCTION(Crash)
    MODULE_FUNCTION(Reset)
    END_MODULE()
}

END_NS()
