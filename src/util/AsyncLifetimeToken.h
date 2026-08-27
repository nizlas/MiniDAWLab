#pragma once

#include <atomic>
#include <memory>

// =============================================================================
// AsyncLifetimeToken — stale-safe async callbacks (Stability Slice 4)
// =============================================================================
// Owners (hosts, coordinators, caches) keep an `AsyncLifetimeOwnerToken` member; its destructor
// flips the shared flag. Delayed work (juce::Timer::callAfterDelay, MessageManager::callAsync,
// FileChooser::launchAsync, worker-thread completions) captures `owner.guard()` *by value* and
// checks `isAlive()` before dereferencing the owner. The guard shares only a flag — it never
// keeps the owner alive, and there are no ownership cycles.
//
// All checks happen on the message thread (owner destruction and callback delivery are both
// message-thread serialized), so "alive at check" implies "alive for the whole callback".
// =============================================================================

namespace mini_daw
{

class AsyncCallbackGuard final
{
public:
    AsyncCallbackGuard() = default;

    explicit AsyncCallbackGuard(std::shared_ptr<const std::atomic<bool>> flag) noexcept
        : flag_(std::move(flag))
    {
    }

    [[nodiscard]] bool isAlive() const noexcept
    {
        return flag_ != nullptr && flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<const std::atomic<bool>> flag_;
};

class AsyncLifetimeOwnerToken final
{
public:
    AsyncLifetimeOwnerToken()
        : flag_(std::make_shared<std::atomic<bool>>(true))
    {
    }

    ~AsyncLifetimeOwnerToken()
    {
        flag_->store(false, std::memory_order_release);
    }

    AsyncLifetimeOwnerToken(const AsyncLifetimeOwnerToken&) = delete;
    AsyncLifetimeOwnerToken& operator=(const AsyncLifetimeOwnerToken&) = delete;

    [[nodiscard]] AsyncCallbackGuard guard() const noexcept
    {
        return AsyncCallbackGuard(flag_);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace mini_daw
