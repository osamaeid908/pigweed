// Copyright 2020 The Pigweed Authors
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "pw_containers/vector.h"
#include "pw_kvs/checksum.h"
#include "pw_kvs/flash_memory.h"
#include "pw_kvs/format.h"
#include "pw_kvs/internal/entry.h"
#include "pw_kvs/internal/key_descriptor.h"
#include "pw_kvs/internal/sector_descriptor.h"
#include "pw_kvs/internal/span_traits.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::kvs {

struct Options {
  // Perform garbage collection if necessary when writing. If true, garbage
  // collection is attempted if space for an entry cannot be found. This is a
  // relatively lengthy operation. If false, Put calls that would require
  // garbage collection fail with RESOURCE_EXHAUSTED.
  bool partial_gc_on_write = true;

  // Verify an entry's checksum after reading it from flash.
  bool verify_on_read = true;

  // Verify an in-flash entry's checksum after writing it.
  bool verify_on_write = true;
};

class KeyValueStore {
 public:
  // TODO: Rework entry relocation to not need a large buffer.
  static constexpr size_t kWorkingBufferSizeBytes = (4 * 1024);

  // KeyValueStores are declared as instances of
  // KeyValueStoreBuffer<MAX_ENTRIES, MAX_SECTORS>, which allocates buffers for
  // tracking entries and flash sectors.

  // Initializes the key-value store. Must be called before calling other
  // functions.
  //
  // Return values:
  //
  //          OK: KVS successfully initialized.
  //   DATA_LOSS: KVS initialized and is usable, but contains corrupt data.
  //     UNKNOWN: Unknown error. KVS is not initialized.
  //
  Status Init();

  bool initialized() const { return initialized_; }

  // Reads the value of an entry in the KVS. The value is read into the provided
  // buffer and the number of bytes read is returned. If desired, the read can
  // be started at an offset.
  //
  // If the output buffer is too small for the value, Get returns
  // RESOURCE_EXHAUSTED with the number of bytes read. The remainder of the
  // value can be read by calling get with an offset.
  //
  //                    OK: the entry was successfully read
  //             NOT_FOUND: the key is not present in the KVS
  //             DATA_LOSS: found the entry, but the data was corrupted
  //    RESOURCE_EXHAUSTED: the buffer could not fit the entire value, but as
  //                        many bytes as possible were written to it
  //   FAILED_PRECONDITION: the KVS is not initialized
  //      INVALID_ARGUMENT: key is empty or too long or value is too large
  //
  StatusWithSize Get(std::string_view key,
                     span<std::byte> value,
                     size_t offset_bytes = 0) const;

  // This overload of Get accepts a pointer to a trivially copyable object.
  // const T& is used instead of T* to prevent arrays from satisfying this
  // overload. To call Get with an array, pass as_writable_bytes(span(array)),
  // or pass a pointer to the array instead of the array itself.
  template <typename Pointer,
            typename = std::enable_if_t<std::is_pointer_v<Pointer>>>
  Status Get(const std::string_view& key, const Pointer& pointer) const {
    using T = std::remove_reference_t<std::remove_pointer_t<Pointer>>;

    static_assert(std::is_trivially_copyable<T>(), "Values must be copyable");
    static_assert(!std::is_pointer<T>(), "Values cannot be pointers");

    return FixedSizeGet(key, reinterpret_cast<std::byte*>(pointer), sizeof(T));
  }

  // Adds a key-value entry to the KVS. If the key was already present, its
  // value is overwritten.
  //
  // In the current implementation, all keys in the KVS must have a unique hash.
  // If Put is called with a key whose hash matches an existing key, nothing
  // is added and ALREADY_EXISTS is returned.
  //
  //                    OK: the entry was successfully added or updated
  //             DATA_LOSS: checksum validation failed after writing the data
  //    RESOURCE_EXHAUSTED: there is not enough space to add the entry
  //        ALREADY_EXISTS: the entry could not be added because a different key
  //                        with the same hash is already in the KVS
  //   FAILED_PRECONDITION: the KVS is not initialized
  //      INVALID_ARGUMENT: key is empty or too long or value is too large
  //
  Status Put(std::string_view key, span<const std::byte> value);

  // Adds a key-value entry to the KVS, using an object as the value.
  template <typename T,
            typename = std::enable_if_t<std::is_trivially_copyable_v<T> &&
                                        !std::is_pointer_v<T> &&
                                        !ConvertsToSpan<T>::value>>
  Status Put(const std::string_view& key, const T& value) {
    return Put(key, as_bytes(span(&value, 1)));
  }

  // Removes a key-value entry from the KVS.
  //
  //                    OK: the entry was successfully added or updated
  //             NOT_FOUND: the key is not present in the KVS
  //             DATA_LOSS: checksum validation failed after recording the erase
  //    RESOURCE_EXHAUSTED: insufficient space to mark the entry as deleted
  //   FAILED_PRECONDITION: the KVS is not initialized
  //      INVALID_ARGUMENT: key is empty or too long
  //
  Status Delete(std::string_view key);

  // Returns the size of the value corresponding to the key.
  //
  //                    OK: the size was returned successfully
  //             NOT_FOUND: the key is not present in the KVS
  //             DATA_LOSS: checksum validation failed after reading the entry
  //   FAILED_PRECONDITION: the KVS is not initialized
  //      INVALID_ARGUMENT: key is empty or too long
  //
  StatusWithSize ValueSize(std::string_view key) const;

  void LogDebugInfo();

  // Classes and functions to support STL-style iteration.
  class iterator;

  class Item {
   public:
    // The key as a null-terminated string.
    const char* key() const { return key_buffer_.data(); }

    StatusWithSize Get(span<std::byte> value_buffer,
                       size_t offset_bytes = 0) const {
      return kvs_.Get(key(), value_buffer, offset_bytes);
    }

    template <typename Pointer,
              typename = std::enable_if_t<std::is_pointer_v<Pointer>>>
    Status Get(const Pointer& pointer) const {
      return kvs_.Get(key(), pointer);
    }

    StatusWithSize ValueSize() const { return kvs_.ValueSize(key()); }

   private:
    friend class iterator;

    constexpr Item(const KeyValueStore& kvs) : kvs_(kvs), key_buffer_{} {}

    const KeyValueStore& kvs_;

    // Buffer large enough for a null-terminated version of any valid key.
    std::array<char, internal::Entry::kMaxKeyLength + 1> key_buffer_;
  };

  class iterator {
   public:
    iterator& operator++();

    iterator& operator++(int) { return operator++(); }

    // Reads the entry's key from flash.
    const Item& operator*();

    const Item* operator->() {
      operator*();  // Read the key into the Item object.
      return &item_;
    }

    constexpr bool operator==(const iterator& rhs) const {
      return index_ == rhs.index_;
    }

    constexpr bool operator!=(const iterator& rhs) const {
      return index_ != rhs.index_;
    }

   private:
    friend class KeyValueStore;

    constexpr iterator(const KeyValueStore& kvs, size_t index)
        : item_(kvs), index_(index) {}

    const internal::KeyDescriptor& descriptor() const {
      return item_.kvs_.key_descriptors_[index_];
    }

    Item item_;
    size_t index_;
  };

  using const_iterator = iterator;  // Standard alias for iterable types.

  iterator begin() const;
  iterator end() const { return iterator(*this, key_descriptors_.size()); }

  // Returns the number of valid entries in the KeyValueStore.
  size_t size() const;

  size_t max_size() const { return key_descriptors_.max_size(); }

  size_t empty() const { return size() == 0u; }

  // Returns the number of transactions that have occurred since the KVS was
  // first used. This value is retained across initializations, but is reset if
  // the underlying flash is erased.
  uint32_t transaction_count() const { return last_transaction_id_; }

  struct StorageStats {
    size_t writable_bytes;
    size_t in_use_bytes;
    size_t reclaimable_bytes;
  };

  StorageStats GetStorageStats() const;

 protected:
  using Address = FlashPartition::Address;
  using Entry = internal::Entry;
  using KeyDescriptor = internal::KeyDescriptor;
  using SectorDescriptor = internal::SectorDescriptor;

  // In the future, will be able to provide additional EntryFormats for
  // backwards compatibility.
  KeyValueStore(FlashPartition* partition,
                Vector<KeyDescriptor>& key_descriptor_list,
                Vector<SectorDescriptor>& sector_descriptor_list,
                const EntryFormat& format,
                const Options& options);

 private:
  Status FixedSizeGet(std::string_view key,
                      std::byte* value,
                      size_t size_bytes) const;

  Status CheckOperation(std::string_view key) const;

  Status FindKeyDescriptor(std::string_view key,
                           const KeyDescriptor** result) const;

  // Non-const version of FindKeyDescriptor.
  Status FindKeyDescriptor(std::string_view key, KeyDescriptor** result) {
    return static_cast<const KeyValueStore&>(*this).FindKeyDescriptor(
        key, const_cast<const KeyDescriptor**>(result));
  }

  Status FindExistingKeyDescriptor(std::string_view key,
                                   const KeyDescriptor** result) const;

  Status FindExistingKeyDescriptor(std::string_view key,
                                   KeyDescriptor** result) {
    return static_cast<const KeyValueStore&>(*this).FindExistingKeyDescriptor(
        key, const_cast<const KeyDescriptor**>(result));
  }

  Status LoadEntry(Address entry_address, Address* next_entry_address);
  Status ScanForEntry(const SectorDescriptor& sector,
                      Address start_address,
                      Address* next_entry_address);
  Status AppendNewOrOverwriteStaleExistingDescriptor(
      const KeyDescriptor& key_descriptor);
  Status AppendEmptyDescriptor(KeyDescriptor** new_descriptor);

  Status WriteEntryForExistingKey(KeyDescriptor* key_descriptor,
                                  KeyDescriptor::State new_state,
                                  std::string_view key,
                                  span<const std::byte> value);

  Status WriteEntryForNewKey(std::string_view key, span<const std::byte> value);

  Status RelocateEntry(KeyDescriptor& key_descriptor);

  Status FindSectorWithSpace(SectorDescriptor** found_sector,
                             size_t size,
                             const SectorDescriptor* sector_to_skip = nullptr,
                             bool bypass_empty_sector_rule = false);

  Status FindOrRecoverSectorWithSpace(SectorDescriptor** sector, size_t size);

  Status GarbageCollectOneSector();

  SectorDescriptor* FindSectorToGarbageCollect();

  KeyDescriptor* FindDescriptor(uint32_t hash);

  Status AppendEntry(SectorDescriptor* sector,
                     KeyDescriptor* key_descriptor,
                     std::string_view key,
                     span<const std::byte> value,
                     KeyDescriptor::State new_state);

  bool AddressInSector(const SectorDescriptor& sector, Address address) const {
    const Address sector_base = SectorBaseAddress(&sector);
    const Address sector_end = sector_base + partition_.sector_size_bytes();

    return ((address >= sector_base) && (address < sector_end));
  }

  unsigned SectorIndex(const SectorDescriptor* sector) const {
    return sector - sectors_.begin();
  }

  Address SectorBaseAddress(const SectorDescriptor* sector) const {
    return SectorIndex(sector) * partition_.sector_size_bytes();
  }

  SectorDescriptor* SectorFromKey(const KeyDescriptor& descriptor) {
    const size_t index = descriptor.address() / partition_.sector_size_bytes();
    // TODO: Add boundary checking once asserts are supported.
    // DCHECK_LT(index, sector_map_size_);
    return &sectors_[index];
  }

  SectorDescriptor* SectorFromKey(const KeyDescriptor* descriptor) {
    return SectorFromKey(*descriptor);
  }

  Address NextWritableAddress(const SectorDescriptor* sector) const {
    return SectorBaseAddress(sector) + partition_.sector_size_bytes() -
           sector->writable_bytes();
  }

  internal::Entry CreateEntry(Address address,
                              std::string_view key,
                              span<const std::byte> value,
                              KeyDescriptor::State state);

  void Reset();

  void LogSectors() const;
  void LogKeyDescriptor() const;

  FlashPartition& partition_;
  const EntryFormat entry_header_format_;

  // Unordered list of KeyDescriptors. Finding a key requires scanning and
  // verifying a match by reading the actual entry.
  Vector<KeyDescriptor>& key_descriptors_;

  // List of sectors used by this KVS.
  Vector<SectorDescriptor>& sectors_;

  Options options_;

  bool initialized_;

  // The last sector that was selected as the "new empty sector" to write to.
  // This last new sector is used as the starting point for the next "find a new
  // empty sector to write to" operation. By using the last new sector as the
  // start point we will cycle which empty sector is selected next, spreading
  // the wear across all the empty sectors, rather than putting more wear on the
  // lower number sectors.
  //
  // Use SectorDescriptor* for the persistent storage rather than sector index
  // because SectorDescriptor* is the standard way to identify a sector.
  SectorDescriptor* last_new_sector_;
  uint32_t last_transaction_id_;

  // Working buffer is a general purpose buffer for operations (such as init or
  // relocate) to use for working space to remove the need to allocate temporary
  // space.
  std::array<std::byte, kWorkingBufferSizeBytes> working_buffer_;
};

template <size_t kMaxEntries, size_t kMaxUsableSectors>
class KeyValueStoreBuffer : public KeyValueStore {
 public:
  KeyValueStoreBuffer(FlashPartition* partition,
                      const EntryFormat& format,
                      const Options& options = {})
      : KeyValueStore(partition, key_descriptors_, sectors_, format, options) {}

 private:
  static_assert(kMaxEntries > 0u);
  static_assert(kMaxUsableSectors > 0u);

  Vector<KeyDescriptor, kMaxEntries> key_descriptors_;
  Vector<SectorDescriptor, kMaxUsableSectors> sectors_;
};

}  // namespace pw::kvs
