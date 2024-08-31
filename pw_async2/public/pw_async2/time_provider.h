// Copyright 2024 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#pragma once

#include <stddef.h>

#include <mutex>

#include "pw_async2/dispatcher.h"
#include "pw_containers/intrusive_list.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_toolchain/no_destructor.h"

namespace pw::async2 {

namespace internal {

// A lock which guards `TimeProvider`'s linked list.
inline pw::sync::InterruptSpinLock& time_lock() {
  static pw::sync::InterruptSpinLock lock;
  return lock;
}

// `Timer` objects must not outlive their `TimeProvider`.
void AssertTimeFutureObjectsAllGone(bool empty);

}  // namespace internal

template <typename Clock>
class TimeFuture;

/// A factory for time and timers.
///
/// This provides similar functionality as the `VirtualSystemClock` interface
/// except it can also create timers, and its template parameter means that it
/// can be used with non-SystemClock clocks.
///
/// `TimeProvider` is designed to be dependency-injection friendly so that
/// code that uses time and timers is not bound to real wall-clock time.
/// This is particularly helpful for testing timing-sensitive code without
/// adding manual delays to tests (which often results in flakiness and
/// long-running tests).
///
/// Note that `Timer` objects must not outlive the `TimeProvider` from which
/// they were created.
template <typename Clock>
class TimeProvider {
 public:
  /// Gets the current `time_point` for the provided clock.
  virtual typename Clock::time_point now() const = 0;

  virtual ~TimeProvider() {
    internal::AssertTimeFutureObjectsAllGone(futures_.empty());
  }

  /// Queues the `callback` to be invoked after `delay`.
  ///
  /// This method is thread-safe and can be invoked from `callback` but may not
  /// be interrupt-safe on all platforms.
  [[nodiscard]] TimeFuture<Clock> WaitFor(typename Clock::duration delay) {
    /// The time_point is computed based on now() plus the specified duration
    /// where a singular clock tick is added to handle partial ticks. This
    /// ensures that a duration of at least 1 tick does not result in [0,1]
    /// ticks and instead in [1,2] ticks.
    return WaitUntil(now() + delay + typename Clock::duration(1));
  }

  /// Queues the `callback` to be invoked after `timestamp`.
  ///
  /// This method is thread-safe and can be invoked from `callback` but may not
  /// be interrupt-safe on all platforms.
  [[nodiscard]] TimeFuture<Clock> WaitUntil(
      typename Clock::time_point timestamp) {
    return TimeFuture<Clock>(*this, timestamp);
  }

 protected:
  /// Run all expired timers with the current (provided) `time_point`.
  ///
  /// This method should be invoked by subclasses when `DoInvokeAt`'s timer
  /// expires.
  void RunExpired(typename Clock::time_point now)
      PW_LOCKS_EXCLUDED(internal::time_lock());

 private:
  friend class TimeFuture<Clock>;

  /// Schedule `RunExpired` to be invoked at `time_point`.
  /// Newer calls to `DoInvokeAt` supersede previous calls.
  virtual void DoInvokeAt(typename Clock::time_point)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) = 0;

  /// Optimistically cancels all pending `DoInvokeAt` requests.
  virtual void DoCancel()
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) = 0;

  // Head of the waiting timers list.
  IntrusiveList<TimeFuture<Clock>> futures_
      PW_GUARDED_BY(internal::time_lock());
};

/// A timer which can asynchronously wait for time to pass.
///
/// This timer uses a `TimeProvider` to control its execution and so can be
/// used with any `TimeProvider` with a compatible `Clock` type.
template <typename Clock>
class [[nodiscard]] TimeFuture : public IntrusiveList<TimeFuture<Clock>>::Item {
 public:
  TimeFuture() : provider_(nullptr) {}
  TimeFuture(const TimeFuture&) = delete;
  TimeFuture& operator=(const TimeFuture&) = delete;

  TimeFuture(TimeFuture&& other) : provider_(nullptr) {
    *this = std::move(other);
  }

  TimeFuture& operator=(TimeFuture&& other) {
    std::lock_guard lock(internal::time_lock());
    UnlistLocked();

    provider_ = other.provider_;
    expiration_ = other.expiration_;

    // Replace the entry of `other_` in the list.
    if (provider_ != nullptr) {
      auto previous = provider_->futures_.before_begin();
      while (&*std::next(previous) != &other) {
        previous++;
      }
      other.unlist(&*previous);
      provider_->futures_.insert_after(previous, *this);
    }

    // Zero out other.
    // NOTE: this will leave `other` reporting (falsely) that it has expired.
    // However, `other` should not be used post-`move`.
    other.provider_ = nullptr;
    return *this;
  }

  /// Destructs `timer`.
  ///
  /// Destruction is thread-safe, but not necessarily interrupt-safe.
  ~TimeFuture() { Unlist(); }

  Poll<typename Clock::time_point> Pend(Context& cx)
      PW_LOCKS_EXCLUDED(internal::time_lock()) {
    std::lock_guard lock(internal::time_lock());
    if (provider_ == nullptr) {
      return Ready(expiration_);
    }
    // NOTE: this is done under the lock in order to ensure that `provider_` is
    // not set to `nullptr` between it being initially read and `waker_` being
    // set.
    waker_ = cx.GetWaker(WaitReason::Timeout());
    return Pending();
  }

 private:
  friend class TimeProvider<Clock>;

  /// Constructs a `Timer` from a `TimeProvider` and a `time_point`.
  TimeFuture(TimeProvider<Clock>& provider,
             typename Clock::time_point expiration)
      : waker_(), provider_(&provider), expiration_(expiration) {
    InitialEnlist();
  }

  void InitialEnlist() PW_LOCKS_EXCLUDED(internal::time_lock()) {
    std::lock_guard lock(internal::time_lock());
    // Skip enlisting if the expiration of the timer is in the past.
    // NOTE: this *does not* trigger a waker since `Poll` has not yet been
    // invoked, so none has been registered.
    if (provider_->now() >= expiration_) {
      provider_ = nullptr;
    }

    if (provider_->futures_.empty() ||
        provider_->futures_.front().expiration_ > expiration_) {
      provider_->futures_.push_front(*this);
      provider_->DoInvokeAt(expiration_);
      return;
    }
    auto current = provider_->futures_.begin();
    while (std::next(current) != provider_->futures_.end() &&
           std::next(current)->expiration_ < expiration_) {
      current++;
    }
    provider_->futures_.insert_after(current, *this);
  }

  void Unlist() PW_LOCKS_EXCLUDED(internal::time_lock()) {
    std::lock_guard lock(internal::time_lock());
    UnlistLocked();
  }

  // Removes this timer from the `TimeProvider`'s list (if listed).
  //
  // If this timer was previously the `head` element of the `TimeProvider`'s
  // list, the `TimeProvider` will be rescheduled to wake up based on the
  // new `head`'s expiration time.
  void UnlistLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(internal::time_lock()) {
    if (provider_ == nullptr) {
      return;
    }
    auto& provider = *provider_;
    provider_ = nullptr;

    if (&provider.futures_.front() == this) {
      provider.futures_.pop_front();
      if (provider.futures_.empty()) {
        provider.DoCancel();
      } else {
        provider.DoInvokeAt(provider.futures_.front().expiration_);
      }
      return;
    }

    provider.futures_.remove(*this);
  }

  Waker waker_;
  // NOTE: `nullptr` is used as a sentinel to indicate that this future has
  // expired and is no longer listed. This prevents unnecessary extra calls
  // to `Unlist` and `provider_->now()`.
  TimeProvider<Clock>* provider_ PW_GUARDED_BY(internal::time_lock());
  typename Clock::time_point expiration_ PW_GUARDED_BY(internal::time_lock());
};

template <typename Clock>
void TimeProvider<Clock>::RunExpired(typename Clock::time_point now) {
  std::lock_guard lock(internal::time_lock());
  while (!futures_.empty()) {
    if (futures_.front().expiration_ > now) {
      DoInvokeAt(futures_.front().expiration_);
      return;
    }
    futures_.front().provider_ = nullptr;
    std::move(futures_.front().waker_).Wake();
    futures_.pop_front();
  }
}

}  // namespace pw::async2