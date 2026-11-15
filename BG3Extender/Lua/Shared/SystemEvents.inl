#include <Lua/Shared/LuaMethodCallHelpers.h>

BEGIN_NS(lua)

SystemEventHooks::SystemEventHooks(lua::State& state)
    : state_(state)
{}

SystemEventHooks::~SystemEventHooks()
{
    for (uint32_t i = 0; i < hookedSystems_.size(); i++) {
        if (hookedSystemMask_[i]) {
            ecs_->SetSystemUpdateHook((ecs::SystemTypeIndex)i, {}, {});
            hookedSystemMask_.Clear(i);
        }
    }
}

void SystemEventHooks::BindECS()
{
    ecs_ = &state_.GetEntitySystemHelpers();
    hookedSystems_.clear();
    hookedSystems_.resize(ecs_->GetEntityWorld()->Systems.Systems.size());
    context_ = state_.IsClient() ? ContextType::Client : ContextType::Server;
}

void SystemEventHooks::FireDeferredEvents()
{
    OPTICK_EVENT();
    for (auto index : deferredUnsubscriptions_) {
        RemoveSubscription(index);
    }

    deferredUnsubscriptions_.clear();
}

SystemEventHooks::SystemHooks* SystemEventHooks::TryAddSystemType(ecs::SystemTypeIndex type)
{
    auto index = (uint16_t)type;
    assert(index < hookedSystems_.size());
    if (!hookedSystemMask_[index]) {
        hookedSystemMask_.Set(index);
        using namespace std::placeholders;
        bool bound = ecs_->SetSystemUpdateHook(type,
            std::bind(&SystemEventHooks::OnSystemPreUpdate, this, _1, _2, _3, _4),
            std::bind(&SystemEventHooks::OnSystemPostUpdate, this, _1, _2, _3, _4));
        if (!bound) return nullptr;
    }

    return &hookedSystems_[index];
}

std::optional<SystemEventHooks::SubscriptionIndex> SystemEventHooks::Subscribe(ecs::SystemTypeIndex system, RegistryEntry&& hook, bool postUpdate, bool once)
{
    SubscriptionIndex index;
    auto sub = subscriptions_.Add(index);
    sub->Hook = std::move(hook);
    sub->System = system;
    sub->PostUpdate = postUpdate;
    sub->Once = once;
    sub->Active = true;

    if (dispatchDepth_ > 0) {
        // A hook run is walking the lists; the subscription takes effect when it ends
        pendingSubscriptions_.push_back(index);
        return index;
    }

    if (!AddToHookLists(index, *sub)) {
        subscriptions_.Free(index);
        return {};
    }

    return index;
}

bool SystemEventHooks::AddToHookLists(SubscriptionIndex index, SystemHook& sub)
{
    auto pool = TryAddSystemType(sub.System);
    if (!pool) return false;

    if (sub.PostUpdate) {
        pool->PostUpdateHooks.push_back(index);
    } else {
        pool->PreUpdateHooks.push_back(index);
    }

    return true;
}

bool SystemEventHooks::Unsubscribe(SubscriptionIndex index)
{
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr) {
        return false;
    }

    if (dispatchDepth_ > 0) {
        // Never runs again; freed when the hook run that walks the list ends
        if (!sub->Active && !sub->Once) return false;
        sub->Active = false;
        sub->Once = false;
        deferredUnsubscriptions_.push_back(index);
        return true;
    }

    RemoveSubscription(index);
    return true;
}

void SystemEventHooks::RemoveSubscription(SubscriptionIndex index)
{
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr) {
        return;
    }

    auto& pool = hookedSystems_[(unsigned)sub->System];
    if (sub->PostUpdate) {
        for (unsigned i = 0; i < pool.PostUpdateHooks.size(); i++) {
            if (pool.PostUpdateHooks[i] == index) {
                pool.PostUpdateHooks.ordered_remove_at(i);
                break;
            }
        }
    } else {
        for (unsigned i = 0; i < pool.PreUpdateHooks.size(); i++) {
            if (pool.PreUpdateHooks[i] == index) {
                pool.PreUpdateHooks.ordered_remove_at(i);
                break;
            }
        }
    }

    subscriptions_.Free(index);
}

void SystemEventHooks::EndDispatch()
{
    se_assert(dispatchDepth_ > 0);
    if (--dispatchDepth_ > 0) return;

    Array<SubscriptionIndex> pending;
    std::swap(pending, pendingSubscriptions_);
    for (auto index : pending) {
        auto sub = subscriptions_.Find(index);
        if (sub != nullptr && !AddToHookLists(index, *sub)) {
            subscriptions_.Free(index);
        }
    }

    Array<SubscriptionIndex> removals;
    std::swap(removals, deferredUnsubscriptions_);
    for (auto index : removals) {
        RemoveSubscription(index);
    }
}

void SystemEventHooks::OnSystemPreUpdate(ecs::EntitySystemHelpersBase*, BaseSystem*, GameTime const&, ecs::SystemTypeIndex systemType)
{
    assert(hookedSystemMask_[(unsigned)systemType]);
    RunHooks(systemType, hookedSystems_[(unsigned)systemType].PreUpdateHooks);
}

void SystemEventHooks::OnSystemPostUpdate(ecs::EntitySystemHelpersBase*, BaseSystem*, GameTime const&, ecs::SystemTypeIndex systemType)
{
    assert(hookedSystemMask_[(unsigned)systemType]);
    RunHooks(systemType, hookedSystems_[(unsigned)systemType].PostUpdateHooks);
}

void SystemEventHooks::RunHooks(ecs::SystemTypeIndex type, Array<SubscriptionIndex> const& hooks)
{
    if (hooks.empty()) return;

    ContextGuardAnyThread _(context_);
    LuaVirtualPin lua(state_.GetExtensionState());

    if (lua) {
        dispatchDepth_++;
        for (auto index : hooks) {
            auto hook = subscriptions_.Find(index);
            if (hook != nullptr) {
                CallHandler(type, *hook, index);
            }
        }
        EndDispatch();
    }
}

void SystemEventHooks::CallHandlerUnsafe(ecs::SystemTypeIndex type, SystemHook& hook, SubscriptionIndex index)
{
    auto L = state_.GetState();
    hook.Hook.Push(L);
    Ref func(L, lua_absindex(L, -1));

    ProtectedFunctionCaller<std::tuple<>, void> caller{ func, std::tuple() };
    caller.Call(L, "System update event");
    lua_pop(L, 1);
}

void SystemEventHooks::CallHandler(ecs::SystemTypeIndex type, SystemHook& hook, SubscriptionIndex index)
{
    if (!hook.Active) return;

    if (hook.Once) {
        deferredUnsubscriptions_.push_back(index);
        hook.Active = false;
        hook.Once = false;
    }

    CallHandlerUnsafe(type, hook, index);
}

END_NS()
