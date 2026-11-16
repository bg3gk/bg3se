#pragma once

BEGIN_NS(lua)

class SystemEventHooks
{
public:
    using SubscriptionIndex = uint32_t;

    SystemEventHooks(State& state);
    ~SystemEventHooks();

    void BindECS();
    void FireDeferredEvents();
    std::optional<SubscriptionIndex> Subscribe(ecs::SystemTypeIndex system, RegistryEntry&& hook, bool postUpdate, bool once);
    bool Unsubscribe(SubscriptionIndex index);

private:
    struct SystemHook
    {
        RegistryEntry Hook;
        ecs::SystemTypeIndex System;
        bool PostUpdate;
        bool Once;
        bool Active;
    };

    struct SystemHooks
    {
        Array<SubscriptionIndex> PreUpdateHooks;
        Array<SubscriptionIndex> PostUpdateHooks;
    };

    State& state_;
    ContextType context_{ ContextType::None };
    ecs::EntitySystemHelpersBase* ecs_{ nullptr };
    BitSet<> hookedSystemMask_;
    Array<SystemHooks> hookedSystems_;
    SaltedPool<SystemHook> subscriptions_;
    Array<SubscriptionIndex> deferredUnsubscriptions_;
    // Subscriptions made while hooks were running; added to the hook lists when the run ends,
    // so that the list being walked never changes under it
    Array<SubscriptionIndex> pendingSubscriptions_;
    // Number of hook runs in progress on the thread that holds the Lua lock
    uint32_t dispatchDepth_{ 0 };

    bool AddToHookLists(SubscriptionIndex index, SystemHook& sub);
    void RemoveSubscription(SubscriptionIndex index);
    void EndDispatch();
    void RunHooks(ecs::SystemTypeIndex type, Array<SubscriptionIndex> const& hooks);
    void CallHandler(ecs::SystemTypeIndex type, SystemHook& hook, SubscriptionIndex index);
    void CallHandlerUnsafe(ecs::SystemTypeIndex type, SystemHook& hook, SubscriptionIndex index);

    SystemHooks* TryAddSystemType(ecs::SystemTypeIndex type);
    void OnSystemPreUpdate(ecs::EntitySystemHelpersBase*, BaseSystem*, GameTime const&, ecs::SystemTypeIndex systemType);
    void OnSystemPostUpdate(ecs::EntitySystemHelpersBase*, BaseSystem*, GameTime const&, ecs::SystemTypeIndex systemType);
};

END_SE()
