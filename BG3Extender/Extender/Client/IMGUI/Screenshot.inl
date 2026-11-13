#pragma once

#include <GameDefinitions/Base/Base.h>
#include <Extender/Shared/ScriptHelpers.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

BEGIN_NS(extui)

// A picture of the game, taken out of the frame the renderer is about to show.
//
// The request comes from Lua and is filled on the rendering thread, at the moment the game hands
// its finished frame to the swap chain and before the extender draws anything of its own over it,
// so the picture is the game alone: no extender windows, no debug tools. Only the copy out of the
// frame happens there; encoding the PNG and writing it are left to a thread of their own, so a
// screenshot costs the game one frame copy and nothing else.
//
// The file is written beside the wanted name and renamed into place, so that a reader waiting for
// it never opens half a picture.
class ScreenshotWriter
{
public:
    // `path` is below the Script Extender folder of the player's profile, as Ext.IO.SaveFile()
    // takes it. False when a screenshot is already on its way.
    bool Request(StringView path)
    {
        std::lock_guard _(mutex_);
        if (requested_ || state_->Writing) return false;
        path_ = STDString(path);
        requested_ = true;
        return true;
    }

    bool Pending() const
    {
        return requested_ || state_->Writing;
    }

    // The rendering thread: the file wanted this frame, if any. A request is handed out once, so
    // that the frames that pass while the file is being written are not captured again.
    std::optional<STDString> Take()
    {
        if (!requested_) return {};

        std::lock_guard _(mutex_);
        if (!requested_) return {};
        requested_ = false;
        state_->Writing = true;
        return path_;
    }

    // The rendering thread, with the frame as it copied it out of the swap chain. `convert` turns
    // those bytes into RGBA8 rows top to bottom; it runs on the writing thread, because the colour
    // of an HDR frame costs more arithmetic than a frame of the game may be held up for.
    using Converter = std::function<bool(std::vector<uint8_t> const& raw, std::vector<uint8_t>& rgba)>;

    void Finish(STDString path, uint32_t width, uint32_t height, std::vector<uint8_t>&& raw, Converter convert)
    {
        auto state = state_;
        std::thread([state, path = std::move(path), width, height, raw = std::move(raw), convert = std::move(convert)]() {
            std::vector<uint8_t> rgba;
            if (convert(raw, rgba)) {
                Write(path, width, height, rgba);
            }
            state->Writing = false;
        }).detach();
    }

    // The rendering thread, when the frame could not be read: nothing is written, and whoever
    // asked stops waiting instead of waiting for a file that will not come.
    void Abandon()
    {
        state_->Writing = false;
    }

private:
    struct SharedState
    {
        // Held by the writing thread as well, which may outlive the backend
        std::atomic<bool> Writing{ false };
    };

    static void Write(STDString const& path, uint32_t width, uint32_t height, std::vector<uint8_t> const& pixels)
    {
        std::vector<uint8_t> png;
        auto append = [](void* context, void* data, int size) {
            auto& out = *reinterpret_cast<std::vector<uint8_t>*>(context);
            out.insert(out.end(), (uint8_t*)data, (uint8_t*)data + size);
        };

        if (!stbi_write_png_to_func(append, &png, (int)width, (int)height, 4, pixels.data(), (int)(width * 4))) {
            ERR("Could not encode a %dx%d screenshot as PNG", width, height);
            return;
        }

        auto partial = path + ".part";
        if (!script::SaveExternalFile(partial, PathRootType::UserProfile,
            StringView((char const*)png.data(), png.size()))) {
            return;
        }

        auto from = script::GetPathForExternalIo(partial, PathRootType::UserProfile);
        auto to = script::GetPathForExternalIo(path, PathRootType::UserProfile);
        if (!from || !to) return;

        std::error_code ec;
        std::filesystem::rename(from->c_str(), to->c_str(), ec);
        if (ec) {
            ERR("Could not put the screenshot in place: '%s'", path.c_str());
        }
    }

    std::shared_ptr<SharedState> state_{ std::make_shared<SharedState>() };
    std::atomic<bool> requested_{ false };
    std::mutex mutex_;
    STDString path_;
};

END_NS()
