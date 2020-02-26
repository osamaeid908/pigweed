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

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "pw_kvs/alignment.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::kvs {

enum class PartitionPermission : bool {
  kReadOnly,
  kReadAndWrite,
};

class FlashMemory {
 public:
  // The flash address is in the range of: 0 to FlashSize.
  typedef uint32_t Address;
  constexpr FlashMemory(size_t sector_size,
                        size_t sector_count,
                        size_t alignment,
                        uint32_t start_address = 0,
                        uint32_t sector_start = 0,
                        std::byte erased_memory_content = std::byte{0xFF})
      : sector_size_(sector_size),
        flash_sector_count_(sector_count),
        alignment_(alignment),
        start_address_(start_address),
        start_sector_(sector_start),
        erased_memory_content_(erased_memory_content) {
    // TODO: The smallest possible alignment is 1 B; 0 is invalid.
    // DCHECK_NE(alignment_, 0);
  }

  virtual ~FlashMemory() = default;

  virtual Status Enable() = 0;
  virtual Status Disable() = 0;
  virtual bool IsEnabled() const = 0;
  virtual Status SelfTest() { return Status::UNIMPLEMENTED; }

  // Erase num_sectors starting at a given address. Blocking call.
  // Address should be on a sector boundary.
  //
  //                OK: success
  // DEADLINE_EXCEEDED: timeout
  //  INVALID_ARGUMENT: address is not sector-aligned
  //      OUT_OF_RANGE: erases past the end of the memory
  //
  virtual Status Erase(Address flash_address, size_t num_sectors) = 0;

  // Reads bytes from flash into buffer. Blocking call.
  //
  //                OK: success
  // DEADLINE_EXCEEDED: timeout
  //      OUT_OF_RANGE: write does not fit in the flash memory
  virtual StatusWithSize Read(Address address, span<std::byte> output) = 0;

  StatusWithSize Read(Address address, void* buffer, size_t len) {
    return Read(address, span(static_cast<std::byte*>(buffer), len));
  }

  // Writes bytes to flash. Blocking call.
  //
  //                OK: success
  // DEADLINE_EXCEEDED: timeout
  //  INVALID_ARGUMENT: address or data size are not aligned
  //      OUT_OF_RANGE: write does not fit in the memory
  //
  virtual StatusWithSize Write(Address destination_flash_address,
                               span<const std::byte> data) = 0;

  StatusWithSize Write(Address destination_flash_address,
                       const void* data,
                       size_t len) {
    return Write(destination_flash_address,
                 span(static_cast<const std::byte*>(data), len));
  }

  // Convert an Address to an MCU pointer, this can be used for memory
  // mapped reads. Return NULL if the memory is not memory mapped.
  virtual std::byte* FlashAddressToMcuAddress(Address) const { return nullptr; }

  // start_sector() is useful for FlashMemory instances where the
  // sector start is not 0. (ex.: cases where there are portions of flash
  // that should be handled independently).
  constexpr uint32_t start_sector() const { return start_sector_; }
  constexpr size_t sector_size_bytes() const { return sector_size_; }
  constexpr size_t sector_count() const { return flash_sector_count_; }
  constexpr size_t alignment_bytes() const { return alignment_; }
  constexpr size_t size_bytes() const {
    return sector_size_ * flash_sector_count_;
  }
  // Address of the start of flash (the address of sector 0)
  constexpr uint32_t start_address() const { return start_address_; }
  constexpr std::byte erased_memory_content() const {
    return erased_memory_content_;
  }

 private:
  const uint32_t sector_size_;
  const uint32_t flash_sector_count_;
  const uint8_t alignment_;
  const uint32_t start_address_;
  const uint32_t start_sector_;
  const std::byte erased_memory_content_;
};

class FlashPartition {
 public:
  // The flash address is in the range of: 0 to PartitionSize.
  using Address = uint32_t;

  // Implement Output for the Write method.
  class Output final : public pw::Output {
   public:
    constexpr Output(FlashPartition& flash, FlashPartition::Address address)
        : flash_(flash), address_(address) {}

    StatusWithSize Write(span<const std::byte> data) override;

   private:
    FlashPartition& flash_;
    FlashPartition::Address address_;
  };

  constexpr FlashPartition(
      FlashMemory* flash,
      uint32_t start_sector_index,
      uint32_t sector_count,
      uint32_t alignment_bytes = 0,  // Defaults to flash alignment
      PartitionPermission permission = PartitionPermission::kReadAndWrite)
      : flash_(*flash),
        start_sector_index_(start_sector_index),
        sector_count_(sector_count),
        alignment_bytes_(alignment_bytes == 0 ? flash_.alignment_bytes()
                                              : alignment_bytes),
        permission_(permission) {}

  // Creates a FlashPartition that uses the entire flash with its alignment.
  constexpr FlashPartition(FlashMemory* flash)
      : FlashPartition(
            flash, 0, flash->sector_count(), flash->alignment_bytes()) {}

  FlashPartition(const FlashPartition&) = delete;
  FlashPartition& operator=(const FlashPartition&) = delete;

  virtual ~FlashPartition() = default;

  // Performs any required partition or flash-level initialization.
  virtual Status Init() { return Status::OK; }

  // Erase num_sectors starting at a given address. Blocking call.
  // Address should be on a sector boundary.
  // Returns: OK, on success.
  //          TIMEOUT, on timeout.
  //          INVALID_ARGUMENT, if address or sector count is invalid.
  //          PERMISSION_DENIED, if partition is read only.
  //          UNKNOWN, on HAL error
  virtual Status Erase(Address address, size_t num_sectors);

  Status Erase() { return Erase(0, this->sector_count()); }

  // Reads bytes from flash into buffer. Blocking call.
  // Returns: OK, on success.
  //          TIMEOUT, on timeout.
  //          INVALID_ARGUMENT, if address or length is invalid.
  //          UNKNOWN, on HAL error
  virtual StatusWithSize Read(Address address, span<std::byte> output);

  StatusWithSize Read(Address address, size_t length, void* output) {
    return Read(address, span(static_cast<std::byte*>(output), length));
  }

  // Writes bytes to flash. Blocking call.
  // Returns: OK, on success.
  //          TIMEOUT, on timeout.
  //          INVALID_ARGUMENT, if address or length is invalid.
  //          PERMISSION_DENIED, if partition is read only.
  //          UNKNOWN, on HAL error
  virtual StatusWithSize Write(Address address, span<const std::byte> data);

  // Check to see if chunk of flash memory is erased. Address and len need to
  // be aligned with FlashMemory.
  // Returns: OK, on success.
  //          TIMEOUT, on timeout.
  //          INVALID_ARGUMENT, if address or length is invalid.
  //          UNKNOWN, on HAL error
  // TODO: Result<bool>
  virtual Status IsRegionErased(Address source_flash_address,
                                size_t len,
                                bool* is_erased);

  // Checks to see if the data appears to be erased. No reads or writes occur;
  // the FlashPartition simply compares the data to
  // flash_.erased_memory_content().
  bool AppearsErased(span<const std::byte> data) const;

  // Overridden by derived classes. The reported sector size is space available
  // to users of FlashPartition. It accounts for space reserved in the sector
  // for FlashPartition to store metadata.
  virtual size_t sector_size_bytes() const {
    return flash_.sector_size_bytes();
  }

  size_t size_bytes() const { return sector_count() * sector_size_bytes(); }

  size_t alignment_bytes() const { return alignment_bytes_; }

  size_t sector_count() const { return sector_count_; }

  // Convert a FlashMemory::Address to an MCU pointer, this can be used for
  // memory mapped reads. Return NULL if the memory is not memory mapped.
  std::byte* PartitionAddressToMcuAddress(Address address) const {
    return flash_.FlashAddressToMcuAddress(PartitionToFlashAddress(address));
  }

  // Converts an address from the partition address space to the flash address
  // space. If the partition reserves additional space in the sector, the flash
  // address space may not be contiguous, and this conversion accounts for that.
  virtual FlashMemory::Address PartitionToFlashAddress(Address address) const {
    return flash_.start_address() +
           (start_sector_index_ - flash_.start_sector()) * sector_size_bytes() +
           address;
  }

  bool writable() const {
    return permission_ == PartitionPermission::kReadAndWrite;
  }

  uint32_t start_sector_index() const { return start_sector_index_; }

 protected:
  Status CheckBounds(Address address, size_t len) const;
  FlashMemory& flash() const { return flash_; }

 private:
  FlashMemory& flash_;
  const uint32_t start_sector_index_;
  const uint32_t sector_count_;
  const uint32_t alignment_bytes_;
  const PartitionPermission permission_;
};

}  // namespace pw::kvs
