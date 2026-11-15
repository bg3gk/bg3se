#include <Lua/Shared/LuaMethodCallHelpers.h>
#include <Extender/Shared/ExtensionState.h>

BEGIN_NS(lua)

EntityComponentEventHooks::EntityComponentEventHooks(lua::State& state)
    : state_(state)
{}

EntityComponentEventHooks::~EntityComponentEventHooks()
{
    for (uint32_t i = 0; i < hookedComponents_.size(); i++) {
        if (hookedComponentMask_[i]) {
            auto& hooks = hookedComponents_[i];
            auto callbacks = world_->ComponentCallbacks.Get((ecs::ComponentTypeIndex)i);
            callbacks->OnConstruct.Remove(hooks.ConstructRegistrant);
            callbacks->OnDestroy.Remove(hooks.DestructRegistrant);
            hookedComponentMask_.Clear(i);
        }
    }
}

void EntityComponentEventHooks::BindECS()
{
    world_ = state_.GetEntityWorld();
}

void EntityComponentEventHooks::FireDeferredEvents()
{
    OPTICK_EVENT();
    for (auto index : deferredUnsubscriptions_) {
        RemoveSubscription(index);
    }

    deferredUnsubscriptions_.clear();

    Array<DeferredEvent> events;
    std::swap(events, deferredEvents_);

    auto& ecs = state_.GetEntitySystemHelpers();

    LuaVirtualPin lua(state_.GetExtensionState());
    if (lua) {
        BeginDispatch();
        for (auto& e : events) {
            auto hook = subscriptions_.Find(e.Index);
            if (hook != nullptr && !hook->Removed) {
                auto meta = ecs.GetComponentMeta(e.Type);
                if (meta) {
                    auto component = ecs.GetRawComponent(e.Entity, *meta);
                    CallHandlerUnsafe(e.Entity, e.Type, e.Event, component, *hook, e.Index);
                }
            }
        }
        EndDispatch();
    }
}

void EntityComponentEventHooks::BeginDispatch()
{
    dispatchDepth_++;
}

void EntityComponentEventHooks::EndDispatch()
{
    se_assert(dispatchDepth_ > 0);
    if (--dispatchDepth_ > 0) return;

    // Subscriptions first: one made and removed inside the same dispatch is then added and
    // removed in order
    Array<SubscriptionIndex> pending;
    std::swap(pending, pendingSubscriptions_);
    for (auto index : pending) {
        auto sub = subscriptions_.Find(index);
        if (sub != nullptr) {
            AddToHookLists(index, *sub);
        }
    }

    Array<SubscriptionIndex> removals;
    std::swap(removals, deferredUnsubscriptions_);
    for (auto index : removals) {
        RemoveSubscription(index);
    }
}

void EntityComponentEventHooks::OnComponentCreated(ecs::ComponentTypeIndex const& type, ecs::EntityRef* entity, void* component)
{
    OnEntityEvent(*entity->World, entity->Handle, type, EntityComponentEvent::Create, component);
}

void EntityComponentEventHooks::OnComponentDestroyed(ecs::ComponentTypeIndex const& type, ecs::EntityRef* entity, void* component)
{
    OnEntityEvent(*entity->World, entity->Handle, type, EntityComponentEvent::Destroy, component);
}

EntityComponentEventHooks::ComponentHooks& EntityComponentEventHooks::AddComponentType(ecs::ComponentTypeIndex type)
{
    auto index = (uint16_t)type;
    if (!hookedComponentMask_[index]) {
        hookedComponentMask_.Set(index);
        while (hookedComponents_.size() <= index) {
            hookedComponents_.push_back(ComponentHooks{});
        }

        auto callbacks = world_->ComponentCallbacks.Get(type);

        auto& hooks = hookedComponents_[index];
        hooks.ConstructRegistrant = callbacks->OnConstruct.Add(MakeFunction(&EntityComponentEventHooks::OnComponentCreated, this, type));
        hooks.DestructRegistrant = callbacks->OnDestroy.Add(MakeFunction(&EntityComponentEventHooks::OnComponentDestroyed, this, type));
    }

    return hookedComponents_[index];
}

EntityComponentEventHooks::SubscriptionIndex EntityComponentEventHooks::Subscribe(ecs::ComponentTypeIndex type, EntityHandle entity,
    EntityComponentEvent events, EntityComponentEventFlags flags, RegistryEntry&& hook)
{
    SubscriptionIndex index;
    auto sub = subscriptions_.Add(index);
    sub->Events = events;
    sub->Flags = flags;
    sub->Type = type;
    sub->Hook = std::move(hook);
    sub->Entity = entity;
    sub->Removed = false;

    if (dispatchDepth_ > 0) {
        // A dispatch is walking the hook lists (and the engine its own callback list); the new
        // subscription takes effect when the dispatch ends
        pendingSubscriptions_.push_back(index);
    } else {
        AddToHookLists(index, *sub);
    }

    return index;
}

void EntityComponentEventHooks::AddToHookLists(SubscriptionIndex index, ComponentHook& sub)
{
    auto& pool = AddComponentType(sub.Type);
    pool.Events |= sub.Events;
    if (!sub.Entity) {
        pool.GlobalHooks.push_back(index);
    } else {
        auto entityHooks = pool.EntityHooks.try_get(sub.Entity);
        if (entityHooks) {
            entityHooks->push_back(index);
        } else {
            entityHooks = pool.EntityHooks.add_key(sub.Entity);
            entityHooks->push_back(index);
        }
    }
}

bool EntityComponentEventHooks::Unsubscribe(SubscriptionIndex index)
{
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr || sub->Removed) {
        return false;
    }

    if (dispatchDepth_ > 0) {
        // Never fires again; freed when the dispatch that walks the lists ends
        sub->Removed = true;
        sub->Events = (EntityComponentEvent)0;
        deferredUnsubscriptions_.push_back(index);
        return true;
    }

    RemoveSubscription(index);
    return true;
}

void EntityComponentEventHooks::RemoveSubscription(SubscriptionIndex index)
{
    auto sub = subscriptions_.Find(index);
    if (sub == nullptr) {
        return;
    }

    if (hookedComponentMask_[(unsigned)sub->Type]) {
        auto& pool = hookedComponents_[(unsigned)sub->Type];
        if (!sub->Entity) {
            for (unsigned i = 0; i < pool.GlobalHooks.size(); i++) {
                if (pool.GlobalHooks[i] == index) {
                    pool.GlobalHooks.ordered_remove_at(i);
                    break;
                }
            }
        } else {
            auto entityHooks = pool.EntityHooks.try_get(sub->Entity);
            if (entityHooks) {
                for (unsigned i = 0; i < entityHooks->size(); i++) {
                    if ((*entityHooks)[i] == index) {
                        entityHooks->ordered_remove_at(i);
                        break;
                    }
                }
            }
        }
    }

    subscriptions_.Free(index);
}

void EntityComponentEventHooks::OnEntityEvent(ecs::EntityWorld& world, EntityHandle entity, ecs::ComponentTypeIndex type, EntityComponentEvent events, void* component)
{
    if (!hookedComponentMask_[(unsigned)type]) return;

    // The Lua lock is taken before any subscription is looked up: otherwise Lua running on
    // another thread (a deferred interface callback) may unsubscribe and free the slot between
    // the lookup and the call
    LuaVirtualPin lua(state_.GetExtensionState());
    if (!lua) return;

    auto& hooks = hookedComponents_[(unsigned)type];
    if ((unsigned)(hooks.Events & events) == 0) return;

    BeginDispatch();
    for (auto index : hooks.GlobalHooks) {
        auto hook = subscriptions_.Find(index);
        if (hook != nullptr && (unsigned)(hook->Events & events) != 0) {
            if ((unsigned)(hook->Flags & EntityComponentEventFlags::Deferred)) {
                DeferHandler(entity, type, events, index);
            } else {
                CallHandlerUnsafe(entity, type, events, component, *hook, index);
            }
        }
    }

    auto entityHooks = hooks.EntityHooks.try_get(entity);
    if (entityHooks) {
        for (auto index : *entityHooks) {
            auto hook = subscriptions_.Find(index);
            if (hook != nullptr && (unsigned)(hook->Events & events) != 0) {
                if ((unsigned)(hook->Flags & EntityComponentEventFlags::Deferred)) {
                    DeferHandler(entity, type, events, index);
                } else {
                    CallHandlerUnsafe(entity, type, events, component, *hook, index);
                }
            }
        }
    }
    EndDispatch();
}

void EntityComponentEventHooks::CallHandlerUnsafe(EntityHandle entity, ecs::ComponentTypeIndex type, EntityComponentEvent events, void* component, ComponentHook& hook, SubscriptionIndex index)
{
    if ((unsigned)(hook.Flags & EntityComponentEventFlags::Once) && !hook.Removed) {
        deferredUnsubscriptions_.push_back(index);
        hook.Events = (EntityComponentEvent)0;
        hook.Removed = true;
    }

    auto L = state_.GetState();
    auto componentType = state_.GetEntitySystemHelpers().GetComponentType(type);
    hook.Hook.Push(L);
    Ref func(L, lua_absindex(L, -1));

    RawComponentRef componentRef{ component, type };
    ProtectedFunctionCaller<std::tuple<EntityHandle, ExtComponentType, RawComponentRef>, void> caller{ func, std::tuple(entity, *componentType, componentRef) };
    caller.Call(L, "Component event dispatch");
    lua_pop(L, 1);
}

void EntityComponentEventHooks::CallHandler(EntityHandle entity, ecs::ComponentTypeIndex type, EntityComponentEvent events, void* component, ComponentHook& hook, SubscriptionIndex index)
{
    LuaVirtualPin lua(state_.GetExtensionState());
    if (lua) {
        BeginDispatch();
        CallHandlerUnsafe(entity, type, events, component, hook, index);
        EndDispatch();
    }
}

void EntityComponentEventHooks::DeferHandler(EntityHandle entity, ecs::ComponentTypeIndex type, EntityComponentEvent events, SubscriptionIndex index)
{
    deferredEvents_.push_back(DeferredEvent{
        .Entity = entity,
        .Type = type,
        .Event = events,
        .Index = index
    });
}

END_NS()
