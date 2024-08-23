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

#include "pw_async2/dispatcher.h"
#include "pw_function/function.h"
#include "pw_sync/mutex.h"

namespace pw::async2 {

// A lock guarding OnceReceiver and OnceSender member variables.
//
// This is an ``InterruptSpinLock`` in order to allow sending values from an
// ISR context.
inline pw::sync::InterruptSpinLock& sender_receiver_lock() {
  static NoDestructor<pw::sync::InterruptSpinLock> lock;
  return *lock;
}

template <typename T>
class OnceSender;

/// `OnceReceiver` receives the value sent by the `OnceSender` it is constructed
/// with. It must be constructed using `MakeOnceSenderAndReceiver`.
/// `OnceReceiver::Pend()` is used to poll for the value sent by `OnceSender`.
/// `OnceReceiver` is thread safe and may be used on a different thread than
/// `OnceSender`.
template <typename T>
class OnceReceiver final {
 public:
  OnceReceiver() = default;

  OnceReceiver(OnceReceiver&& other) {
    std::lock_guard lock(sender_receiver_lock());
    sender_ = other.sender_;
    other.sender_ = nullptr;
    if (sender_) {
      sender_->receiver_ = this;
    }
    if (other.value_.has_value()) {
      value_.emplace(std::move(other.value_.value()));
    }
    other.value_.reset();
    waker_ = std::move(other.waker_);
  }

  OnceReceiver(const OnceReceiver&) = delete;
  OnceReceiver& operator=(const OnceReceiver&) = delete;
  OnceReceiver& operator=(OnceReceiver&& other) = delete;

  ~OnceReceiver();

  /// Returns `Ready` with a result containing the value once the value has been
  /// assigned. If the sender is destroyed before sending a value, a `Cancelled`
  /// result will be returned.
  Poll<Result<T>> Pend() {
    std::lock_guard lock(sender_receiver_lock());
    if (value_.has_value()) {
      return Ready(std::move(*value_));
    } else if (!sender_) {
      return Ready(Status::Cancelled());
    }
    return Pending();
  }

 private:
  template <typename U>
  friend std::pair<OnceSender<U>, OnceReceiver<U>> MakeOnceSenderAndReceiver(
      Waker);
  template <typename U>
  friend void InitializeOnceSenderAndReceiver(OnceSender<U>& sender,
                                              OnceReceiver<U>& receiver,
                                              Waker waker);
  friend class OnceSender<T>;

  // `waker` is the `Waker` to be awoken when the value is assigned.
  OnceReceiver(Waker waker) : waker_(std::move(waker)) {}

  OnceSender<T>* sender_ PW_GUARDED_BY(sender_receiver_lock()) = nullptr;
  std::optional<T> value_ PW_GUARDED_BY(sender_receiver_lock());
  Waker waker_;
};

/// `OnceSender` sends the value received by the `OnceReceiver` it is
/// constructed with. It must be constructed using `MakeOnceSenderAndReceiver`.
/// `OnceSender` is thread safe and may be used on a different thread than
/// `OnceReceiver`.
template <typename T>
class OnceSender final {
 public:
  OnceSender() = default;

  ~OnceSender() {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      std::move(receiver_->waker_).Wake();
      receiver_->sender_ = nullptr;
    }
  }

  OnceSender(OnceSender&& other) {
    std::lock_guard lock(sender_receiver_lock());
    receiver_ = other.receiver_;
    other.receiver_ = nullptr;
    if (receiver_) {
      receiver_->sender_ = this;
    }
  }

  OnceSender(const OnceSender&) = delete;
  OnceSender& operator=(const OnceSender&) = delete;
  OnceSender& operator=(OnceSender&& other) = delete;

  /// Construct the sent value in place and wake the `OnceReceiver`.
  template <typename... Args>
  void emplace(Args&&... args) {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      receiver_->value_.emplace(std::forward<Args>(args)...);
      std::move(receiver_->waker_).Wake();
      receiver_->sender_ = nullptr;
      receiver_ = nullptr;
    }
  }

  OnceSender& operator=(const T& value) {
    emplace(value);
    return *this;
  }
  OnceSender& operator=(T&& value) {
    emplace(std::move(value));
    return *this;
  }

 private:
  template <typename U>
  friend std::pair<OnceSender<U>, OnceReceiver<U>> MakeOnceSenderAndReceiver(
      Waker);
  template <typename U>
  friend void InitializeOnceSenderAndReceiver(OnceSender<U>& sender,
                                              OnceReceiver<U>& receiver,
                                              Waker waker);
  friend class OnceReceiver<T>;

  OnceSender(OnceReceiver<T>* receiver) : receiver_(receiver) {}

  OnceReceiver<T>* receiver_ PW_GUARDED_BY(sender_receiver_lock()) = nullptr;
};

template <typename T>
OnceReceiver<T>::~OnceReceiver() {
  std::lock_guard lock(sender_receiver_lock());
  if (sender_) {
    sender_->receiver_ = nullptr;
  }
}

/// Construct a pair of `OnceSender` and `OnceReceiver`.
/// @param waker The `Waker` to be awoken when the value is sent.
template <typename T>
std::pair<OnceSender<T>, OnceReceiver<T>> MakeOnceSenderAndReceiver(
    Waker waker) {
  OnceReceiver<T> receiver(std::move(waker));
  OnceSender<T> sender(&receiver);
  {
    std::lock_guard lock(sender_receiver_lock());
    receiver.sender_ = &sender;
  }
  return std::make_pair(std::move(sender), std::move(receiver));
}

/// Initialize a pair of `OnceSender` and `OnceReceiver`.
/// @param waker The `Waker` to be awoken when the value is sent.
template <typename T>
void InitializeOnceSenderAndReceiver(OnceSender<T>& sender,
                                     OnceReceiver<T>& receiver,
                                     Waker waker) {
  std::lock_guard lock(sender_receiver_lock());
  receiver.sender_ = &sender;
  receiver.waker_ = std::move(waker);
  sender.receiver_ = &receiver;
}

template <typename T>
class OnceRefSender;

/// `OnceRefReceiver` is notified when the paired `OnceRefSender` modifies a
/// reference. It must be constructed using `MakeOnceRefSenderAndReceiver()`.
/// `OnceRefReceiver::Pend()` is used to poll for completion by `OnceRefSender`.
/// `OnceRefReceiver` is thread safe and may be used on a different thread than
/// `OnceRefSender`. However, the referenced value must not be modified from the
/// time of construction until either `OnceRefReceiver::Pend()` returns
/// `Ready()` or either of `OnceRefReceiver` or `OnceRefSender` is destroyed.
template <typename T>
class OnceRefReceiver final {
 public:
  OnceRefReceiver() = default;

  OnceRefReceiver(OnceRefReceiver&& other) {
    std::lock_guard lock(sender_receiver_lock());
    sender_ = other.sender_;
    other.sender_ = nullptr;
    if (sender_) {
      sender_->receiver_ = this;
    }
    value_ = other.value_;
    cancelled_ = other.cancelled_;
    waker_ = std::move(other.waker_);
  }

  OnceRefReceiver(const OnceRefReceiver&) = delete;
  OnceRefReceiver& operator=(const OnceRefReceiver&) = delete;
  OnceRefReceiver& operator=(OnceRefReceiver&& other) = delete;

  ~OnceRefReceiver();

  /// Returns `Ready`  with an `ok` status when the modification of the
  /// reference is complete. If the sender is destroyed before updating the
  /// reference, a `cancelled` status is returned.
  Poll<Status> Pend() {
    std::lock_guard lock(sender_receiver_lock());
    if (cancelled_) {
      return Ready(Status::Cancelled());
    }
    if (waker_.IsEmpty()) {
      return Ready(OkStatus());
    }
    return Pending();
  }

 private:
  template <typename U>
  friend std::pair<OnceRefSender<U>, OnceRefReceiver<U>>
  MakeOnceRefSenderAndReceiver(U&, Waker);
  template <typename U>
  friend void InitializeOnceRefSenderAndReceiver(OnceRefSender<U>& sender,
                                                 OnceRefReceiver<U>& receiver,
                                                 U& value,
                                                 Waker waker);
  friend class OnceRefSender<T>;

  // `waker` is the `Waker` to be awoken when the value is assigned.
  OnceRefReceiver(T& value, Waker waker)
      : value_(&value), waker_(std::move(waker)) {}

  OnceRefSender<T>* sender_ PW_GUARDED_BY(sender_receiver_lock()) = nullptr;
  T* value_ PW_GUARDED_BY(sender_receiver_lock()) = nullptr;
  bool cancelled_ PW_GUARDED_BY(sender_receiver_lock()) = false;
  Waker waker_;
};

/// `OnceRefSender` mutates the reference received by the `OnceReceiver` it is
/// constructed with. It must be constructed using
/// `MakeOnceRefSenderAndReceiver`. `OnceRefSender` is thread safe and may be
/// used on a different thread than `OnceRefReceiver`.
template <typename T>
class OnceRefSender final {
 public:
  OnceRefSender() = default;

  ~OnceRefSender() {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      receiver_->sender_ = nullptr;
      if (!receiver_->waker_.IsEmpty()) {
        receiver_->cancelled_ = true;
        std::move(receiver_->waker_).Wake();
      }
    }
  }

  OnceRefSender(OnceRefSender&& other) {
    std::lock_guard lock(sender_receiver_lock());
    receiver_ = other.receiver_;
    other.receiver_ = nullptr;
    if (receiver_) {
      receiver_->sender_ = this;
    }
  }

  OnceRefSender(const OnceRefSender&) = delete;
  OnceRefSender& operator=(const OnceRefSender&) = delete;
  OnceRefSender& operator=(OnceRefSender&& other) = delete;

  /// Copy assigns the reference and awakens the receiver.
  void Set(const T& value) {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      *(receiver_->value_) = value;
      std::move(receiver_->waker_).Wake();
      receiver_->sender_ = nullptr;
      receiver_ = nullptr;
    }
  }

  /// Move assigns the reference and awakens the receiver.
  void Set(T&& value) {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      *(receiver_->value_) = std::move(value);
      std::move(receiver_->waker_).Wake();
      receiver_->sender_ = nullptr;
      receiver_ = nullptr;
    }
  }

  /// Care must be taken not to save the reference passed to `func` or to call
  /// any other Once*Sender/Once*Receiver APIs from within `func`. This should
  /// be a simple modification. After all modifications are complete, `Commit`
  /// should be called.
  void ModifyUnsafe(pw::Function<void(T&)> func) {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      // There is a risk of re-entrancy here if the user isn't careful.
      func(*(receiver_->value_));
    }
  }

  /// When using `ModifyUnsafe()`, call `Commit()` after all modifications have
  /// been made to awaken the `OnceRefReceiver`.
  void Commit() {
    std::lock_guard lock(sender_receiver_lock());
    if (receiver_) {
      std::move(receiver_->waker_).Wake();
      receiver_->sender_ = nullptr;
      receiver_ = nullptr;
    }
  }

 private:
  template <typename U>
  friend std::pair<OnceRefSender<U>, OnceRefReceiver<U>>
  MakeOnceRefSenderAndReceiver(U&, Waker);
  template <typename U>
  friend void InitializeOnceRefSenderAndReceiver(OnceRefSender<U>& sender,
                                                 OnceRefReceiver<U>& receiver,
                                                 U& value,
                                                 Waker waker);
  friend class OnceRefReceiver<T>;

  OnceRefSender(OnceRefReceiver<T>* receiver) : receiver_(receiver) {}

  OnceRefReceiver<T>* receiver_ PW_GUARDED_BY(sender_receiver_lock()) = nullptr;
};

template <typename T>
OnceRefReceiver<T>::~OnceRefReceiver() {
  std::lock_guard lock(sender_receiver_lock());
  if (sender_) {
    sender_->receiver_ = nullptr;
  }
}

/// Constructs a joined pair of `OnceRefSender` and `OnceRefReceiver`.
/// @param[in] value The reference to be mutated by the sender. It must mot be
/// read or modified until either `OnceRefSender` indicates `Ready()` or
/// either the `OnceRefSender` or `OnceRefReceiver` is destroyed.
/// @param[in] waker The `Waker` to be awoken when the reference is updated.
template <typename T>
std::pair<OnceRefSender<T>, OnceRefReceiver<T>> MakeOnceRefSenderAndReceiver(
    T& value, Waker waker) {
  OnceRefReceiver<T> receiver(value, std::move(waker));
  OnceRefSender<T> sender(&receiver);
  return std::make_pair(std::move(sender), std::move(receiver));
}

/// Initialize a pair of `OnceRefSender` and `OnceRefReceiver`.
/// @param[in] value The reference to be mutated by the sender. It must mot be
/// read or modified until either `OnceRefSender` indicates `Ready()` or
/// either the `OnceRefSender` or `OnceRefReceiver` is destroyed.
/// @param[in] waker The `Waker` to be awoken when the reference is updated.
template <typename T>
void InitializeOnceRefSenderAndReceiver(OnceRefSender<T>& sender,
                                        OnceRefReceiver<T>& receiver,
                                        T& value,
                                        Waker waker) {
  std::lock_guard lock(sender_receiver_lock());
  receiver.sender_ = &sender;
  receiver.value_ = &value;
  receiver.cancelled_ = false;
  receiver.waker_ = std::move(waker);
  sender.receiver_ = &receiver;
}

}  // namespace pw::async2