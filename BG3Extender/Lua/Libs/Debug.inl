#include <Extender/ScriptExtender.h>
#include <Lua/Debugger/LuaDebugger.h>

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

void LuaDebugBreak(lua_State* L)
{
    auto debugger = gExtender->GetLuaDebugger();
    if (debugger) {
        debugger->DebugBreak(L);
    }
}

// Development-only function for testing crash reporting
void Crash(int type)
{
#if defined(_DEBUG)
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
        *(uint32_t*)0 = 0;
        break;
    }
#endif
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
    MODULE_FUNCTION(SetEntityRuntimeCheckLevel)
    MODULE_FUNCTION(Crash)
    MODULE_FUNCTION(Reset)
    END_MODULE()
}

END_NS()
