#pragma once

#include <dobby.h>
#include <stdexcept>
#include <unordered_set>

// Plugin-owned ARM hooks; the native Hyprland hook API only supports x86_64.
// Must be used from the compositor thread, while targets are not executing.
class Arm64Hook {
  public:
    Arm64Hook(void* source, void* destination) : source_(source), destination_(destination) {
        if (!source_ || !destination_)
            throw std::invalid_argument("Null ARM64 hook target");
    }
    Arm64Hook(const Arm64Hook&) = delete;
    Arm64Hook& operator=(const Arm64Hook&) = delete;

    ~Arm64Hook() { unhook(); }

    bool hook() {
        if (active_)
            return true;
        if (owned_.contains(source_))
            return false;
        if (DobbyHook(source_, destination_, &m_original) != 0)
            return false;
        active_ = true;
        owned_.insert(source_);
        return true;
    }

    bool unhook() {
        if (!active_)
            return true;
        if (DobbyDestroy(source_) != 0)
            return false;
        active_ = false;
        owned_.erase(source_);
        // Dobby retains the relocation block. Keep it callable for a replacement
        // that is completing while the overview restores its rendering hooks.
        return true;
    }

    void* m_original = nullptr;

  private:
    void* source_;
    void* destination_;
    bool active_ = false;
    inline static std::unordered_set<void*> owned_;
};
