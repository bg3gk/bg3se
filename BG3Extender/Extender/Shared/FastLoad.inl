// GateKit addition: fast loads (FastLoad.h). Included once, by ScriptExtenderClient.cpp.
#include <Extender/Shared/FastLoad.h>
#include <Extender/Shared/StartupTimeline.h>
#include <Extender/Client/FrameCap.inl>
#include <GameDefinitions/Misc.h>
#include <atomic>
#include <cstdio>

BEGIN_NS(fast_load)

namespace
{
    bool gClientFrames{ false };
    bool gServerTick{ false };
    bool gStreamingWait{ false };
    bool gFades{ false };

    // client thread only
    bool gFramesLifted{ false };
    uint16_t gSavedFrameCap{ 0 };
    uint16_t gSavedLostFocusCap{ 0 };
    bool gPollerCut{ false };
    bool gStreamingGate{ false };   // FastLoadStreamingGate: the most wait kept (FastLoad.h)

    // server thread only
    uint8_t* gServerWorker{ nullptr };
    int64_t gSavedTickUs{ 0 };
    bool gTickLifted{ false };

    constexpr std::size_t ServerWorkerTickUsOffset = 0x90;
    constexpr std::size_t PrepareRunningPollerOffset = 0x20;

    void Step(char const* text)
    {
        startup_timeline::RecordStep(text, startup_timeline::StepBegin);
    }

    bool ClientLoading(int32_t to)
    {
        using S = ecl::GameState;
        auto s = (S)to;
        return s == S::StartLoading || s == S::Idle || s == S::UnloadLevel || s == S::UnloadSession
            || s == S::UnloadModule || s == S::LoadModule || s == S::LoadSession || s == S::LoadLevel
            || s == S::SwapLevel || s == S::StopLoading || s == S::PrepareRunning || s == S::LoadMenu
            || s == S::Disconnect || s == S::InitNetwork || s == S::InitConnection || s == S::StartServer;
    }

    bool ServerLoading(int32_t to)
    {
        using S = esv::GameState;
        auto s = (S)to;
        return s == S::UnloadLevel || s == S::UnloadSession || s == S::UnloadModule || s == S::LoadModule
            || s == S::LoadSession || s == S::LoadLevel || s == S::Sync || s == S::Init || s == S::Idle;
    }

    void ApplyFades()
    {
        auto switches = GetStaticSymbols().GetGlobalSwitches();
        if (switches == nullptr) return;
        if (switches->FadeSpeed != 0.0f) {
            char text[64];
            snprintf(text, sizeof(text), "FastLoad fades: FadeSpeed %.2f -> 0", switches->FadeSpeed);
            switches->FadeSpeed = 0.0f;
            Step(text);
        }
    }

    // A poller's least and most wait look like seconds, the least not above the most.
    bool LooksLikePoller(float const* v)
    {
        return v[0] >= 0.0f && v[0] <= 600.0f && v[1] >= 0.0f && v[1] <= 600.0f && v[0] <= v[1];
    }

    bool CutPoller(uint8_t* state, float& least, float& most)
    {
        __try {
            auto p = reinterpret_cast<float*>(state + PrepareRunningPollerOffset);
            if (!LooksLikePoller(p)) return false;
            least = p[0];
            most = p[1];
            p[0] = 0.0f;
            if (!gStreamingGate) p[1] = 0.0f;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

void Configure(bool clientFrames, bool serverTick, bool streamingWait, bool fades, bool streamingGate)
{
    gStreamingGate = streamingGate;
    gClientFrames = clientFrames;
    gServerTick = serverTick;
    gStreamingWait = streamingWait;
    gFades = fades;
}

bool Any()
{
    return gClientFrames || gServerTick || gStreamingWait || gFades;
}

void OnClientState(int32_t from, int32_t to)
{
    if (!Any()) return;
    if (gFades) ApplyFades();

    if (gClientFrames) {
        if (ClientLoading(to) && !gFramesLifted) {
            auto app = ecl::FrameCapAccess::Probe();
            if (app) {
                gSavedFrameCap = app->FrameCap();
                gSavedLostFocusCap = app->LostFocusFrameCap();
                app->SetFrameCap(0);
                app->SetLostFocusFrameCap(0);
                gFramesLifted = true;
                char text[64];
                snprintf(text, sizeof(text), "FastLoad frames: caps %u/%u -> 0", gSavedFrameCap, gSavedLostFocusCap);
                Step(text);
            }
        } else if (!ClientLoading(to) && gFramesLifted) {
            auto app = ecl::FrameCapAccess::Probe();
            if (app) {
                app->SetFrameCap(gSavedFrameCap);
                app->SetLostFocusFrameCap(gSavedLostFocusCap);
                Step("FastLoad frames: caps put back");
            }
            gFramesLifted = false;
        }
    }

    if ((ecl::GameState)to == ecl::GameState::PrepareRunning) gPollerCut = false;
}

void OnClientUpdate(void* machine, bool prepareRunning)
{
    if (!gStreamingWait || !prepareRunning || gPollerCut || machine == nullptr) return;
    auto state = *reinterpret_cast<uint8_t**>(machine);
    if (state == nullptr) return;
    float least{ 0 }, most{ 0 };
    if (CutPoller(state, least, most)) {
        gPollerCut = true;
        char text[80];
        snprintf(text, sizeof(text), "FastLoad streaming wait: %.2f/%.2f s -> 0", least, most);
        Step(text);
    }
}

void OnServerWorker(void* worker)
{
    gServerWorker = static_cast<uint8_t*>(worker);
}

void OnServerState(int32_t from, int32_t to)
{
    if (!gServerTick || gServerWorker == nullptr) return;
    auto period = reinterpret_cast<int64_t*>(gServerWorker + ServerWorkerTickUsOffset);
    if (ServerLoading(to)) {
        if (*period > 0 && *period <= 1000000) {
            gSavedTickUs = *period;
            *period = 0;
            if (!gTickLifted) {
                char text[64];
                snprintf(text, sizeof(text), "FastLoad server tick: %lld us -> 0", (long long)gSavedTickUs);
                Step(text);
            }
            gTickLifted = true;
        }
    } else if (gTickLifted) {
        if (*period == 0) *period = gSavedTickUs;
        gTickLifted = false;
        Step("FastLoad server tick: put back");
    }
}

END_NS()
