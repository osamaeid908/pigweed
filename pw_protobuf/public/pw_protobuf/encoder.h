// Copyright 2021 The Pigweed Authors
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

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "pw_assert/assert.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_protobuf/config.h"
#include "pw_protobuf/wire_format.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/memory_stream.h"
#include "pw_stream/stream.h"
#include "pw_varint/varint.h"

namespace pw::protobuf {

// Provides a size estimate to help with sizing buffers passed to
// StreamEncoder and MemoryEncoder objects.
//
// Args:
//   max_message_size: For MemoryEncoder objects, this is the max expected size
//     of the final proto. For StreamEncoder objects, this should be the max
//     size of any nested proto submessage that will be built with this encoder
//     (recursively accumulating the size from the root submessage). If your
//     proto will encode many large submessages, this value should just be the
//     size of the largest one.
//  max_nested_depth: The max number of nested submessage encoders that are
//     expected to be open simultaneously to encode this proto message.
static constexpr size_t MaxScratchBufferSize(size_t max_message_size,
                                             size_t max_nested_depth) {
  return max_message_size + max_nested_depth * config::kMaxVarintSize;
}

// Forward declaration. StreamEncoder and MemoryEncoder are very tightly
// coupled.
class MemoryEncoder;

// A protobuf encoder that encodes serialized proto data to a
// pw::stream::Writer.
class StreamEncoder {
 public:
  // The StreamEncoder will serialize proto data to the pw::stream::Writer
  // provided through the constructor. The scratch buffer provided is for
  // internal use ONLY and should not be considered valid proto data.
  //
  // If a StreamEncoder object will be writing nested proto messages, it must
  // provide a scratch buffer large enough to hold the largest submessage some
  // additional overhead incurred by the encoder's implementation. It's a good
  // idea to be generous when sizing this buffer. MaxScratchBufferSize() can be
  // helpful in providing an estimated size for this buffer. The scratch buffer
  // must exist for the lifetime of the StreamEncoder object.
  //
  // StreamEncoder objects that do not write nested proto messages can
  // provide a zero-length scratch buffer.
  constexpr StreamEncoder(stream::Writer& writer, ByteSpan scratch_buffer)
      : status_(OkStatus()),
        parent_(nullptr),
        nested_field_number_(0),
        memory_writer_(scratch_buffer),
        writer_(writer) {}

  // Precondition: Encoder has no active child encoder.
  //
  // Postcondition: If this encoder is a nested one, the parent encoder is
  //     unlocked and proto encoding may resume on the parent.
  ~StreamEncoder();

  // Disallow copy/assign to avoid confusion about who owns the buffer.
  StreamEncoder& operator=(const StreamEncoder& other) = delete;
  StreamEncoder(const StreamEncoder& other) = delete;

  // It's not safe to move an encoder as it could cause another encoder's
  // parent_ pointer to become invalid.
  StreamEncoder& operator=(StreamEncoder&& other) = delete;

  // Forwards the conservative write limit of the underlying
  // pw::stream::Writer.
  //
  // Precondition: Encoder has no active child encoder.
  size_t ConservativeWriteLimit() const {
    PW_ASSERT(!nested_encoder_open());
    return writer_.ConservativeWriteLimit();
  }

  // Creates a nested encoder with the provided field number. Once this is
  // called, the parent encoder is locked and not available for use until the
  // nested encoder is finalized (either explicitly or through destruction).
  //
  // Precondition: Encoder has no active child encoder.
  //
  // Postcondition: Until the nested child encoder has been destroyed, this
  //     encoder cannot be used.
  StreamEncoder GetNestedEncoder(uint32_t field_number);

  // Returns the current encoder's status.
  //
  // Precondition: Encoder has no active child encoder.
  Status status() const {
    PW_ASSERT(!nested_encoder_open());
    return status_;
  }

  // Writes a proto uint32 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteUint32(uint32_t field_number, uint32_t value) {
    return WriteUint64(field_number, value);
  }

  // Writes a repeated uint32 using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedUint32(uint32_t field_number,
                           std::span<const uint32_t> values) {
    return WritePackedVarints(field_number, values, VarintEncodeType::kNormal);
  }

  // Writes a proto uint64 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteUint64(uint32_t field_number, uint64_t value) {
    return WriteVarintField(field_number, value);
  }

  // Writes a repeated uint64 using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedUint64(uint64_t field_number,
                           std::span<const uint64_t> values) {
    return WritePackedVarints(field_number, values, VarintEncodeType::kNormal);
  }

  // Writes a proto int32 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteInt32(uint32_t field_number, int32_t value) {
    return WriteUint64(field_number, value);
  }

  // Writes a repeated int32 using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedInt32(uint32_t field_number,
                          std::span<const int32_t> values) {
    return WritePackedVarints(
        field_number,
        std::span(reinterpret_cast<const uint32_t*>(values.data()),
                  values.size()),
        VarintEncodeType::kNormal);
  }

  // Writes a proto int64 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteInt64(uint32_t field_number, int64_t value) {
    return WriteUint64(field_number, value);
  }

  // Writes a repeated int64 using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedInt64(uint32_t field_number,
                          std::span<const int64_t> values) {
    return WritePackedVarints(
        field_number,
        std::span(reinterpret_cast<const uint64_t*>(values.data()),
                  values.size()),
        VarintEncodeType::kNormal);
  }

  // Writes a proto sint32 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteSint32(uint32_t field_number, int32_t value) {
    return WriteUint64(field_number, varint::ZigZagEncode(value));
  }

  // Writes a repeated sint32 using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedSint32(uint32_t field_number,
                           std::span<const int32_t> values) {
    return WritePackedVarints(
        field_number,
        std::span(reinterpret_cast<const uint32_t*>(values.data()),
                  values.size()),
        VarintEncodeType::kZigZag);
  }

  // Writes a proto sint64 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteSint64(uint32_t field_number, int64_t value) {
    return WriteUint64(field_number, varint::ZigZagEncode(value));
  }

  // Writes a repeated sint64 using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedSint64(uint32_t field_number,
                           std::span<const int64_t> values) {
    return WritePackedVarints(
        field_number,
        std::span(reinterpret_cast<const uint64_t*>(values.data()),
                  values.size()),
        VarintEncodeType::kZigZag);
  }

  // Writes a proto bool key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteBool(uint32_t field_number, bool value) {
    return WriteUint32(field_number, static_cast<uint32_t>(value));
  }

  // Writes a proto fixed32 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteFixed32(uint32_t field_number, uint32_t value) {
    std::array<std::byte, sizeof(value)> data =
        bytes::CopyInOrder(std::endian::little, value);
    return WriteFixed(field_number, data);
  }

  // Writes a repeated fixed32 field using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedFixed32(uint32_t field_number,
                            std::span<const uint32_t> values) {
    return WritePackedFixed(
        field_number, std::as_bytes(values), sizeof(uint32_t));
  }

  // Writes a proto fixed64 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteFixed64(uint32_t field_number, uint64_t value) {
    std::array<std::byte, sizeof(value)> data =
        bytes::CopyInOrder(std::endian::little, value);
    return WriteFixed(field_number, data);
  }

  // Writes a repeated fixed64 field using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedFixed64(uint32_t field_number,
                            std::span<const uint64_t> values) {
    return WritePackedFixed(
        field_number, std::as_bytes(values), sizeof(uint64_t));
  }

  // Writes a proto sfixed32 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteSfixed32(uint32_t field_number, int32_t value) {
    return WriteFixed32(field_number, static_cast<uint32_t>(value));
  }

  // Writes a repeated sfixed32 field using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedSfixed32(uint32_t field_number,
                             std::span<const int32_t> values) {
    return WritePackedFixed(
        field_number, std::as_bytes(values), sizeof(int32_t));
  }

  // Writes a proto sfixed64 key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteSfixed64(uint32_t field_number, int64_t value) {
    return WriteFixed64(field_number, static_cast<uint64_t>(value));
  }

  // Writes a repeated sfixed64 field using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedSfixed64(uint32_t field_number,
                             std::span<const int64_t> values) {
    return WritePackedFixed(
        field_number, std::as_bytes(values), sizeof(int64_t));
  }

  // Writes a proto float key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteFloat(uint32_t field_number, float value) {
    static_assert(sizeof(float) == sizeof(uint32_t),
                  "Float and uint32_t are not the same size");
    uint32_t integral_value;
    std::memcpy(&integral_value, &value, sizeof(value));
    std::array<std::byte, sizeof(value)> data =
        bytes::CopyInOrder(std::endian::little, integral_value);
    return WriteFixed(field_number, data);
  }

  // Writes a repeated float field using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedFloat(uint32_t field_number,
                          std::span<const float> values) {
    return WritePackedFixed(field_number, std::as_bytes(values), sizeof(float));
  }

  // Writes a proto double key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteDouble(uint32_t field_number, double value) {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t are not the same size");
    uint64_t integral_value;
    std::memcpy(&integral_value, &value, sizeof(value));
    std::array<std::byte, sizeof(value)> data =
        bytes::CopyInOrder(std::endian::little, integral_value);
    return WriteFixed(field_number, data);
  }

  // Writes a repeated double field using packed encoding.
  //
  // Precondition: Encoder has no active child encoder.
  Status WritePackedDouble(uint32_t field_number,
                           std::span<const double> values) {
    return WritePackedFixed(
        field_number, std::as_bytes(values), sizeof(double));
  }

  // Writes a proto `bytes` field as a key-value pair. This can also be used to
  // write a pre-encoded nested submessage directly without using a nested
  // encoder.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteBytes(uint32_t field_number, ConstByteSpan value) {
    return WriteLengthDelimitedField(field_number, value);
  }

  // Writes a proto 'bytes' field from the stream bytes_reader.
  //
  // The payload for the value is provided through the stream::Reader
  // `bytes_reader`. The method reads a chunk of the data from the reader using
  // the `stream_pipe_buffer` and writes it to the encoder.
  //
  // Precondition: The stream_pipe_buffer.byte_size() >= 1
  // Precondition: Encoder has no active child encoder.
  //
  // Returns:
  // OK - Bytes field is written successfully.
  // RESOURCE_EXHAUSTED - Exceeds write limits.
  // OUT_OF_RANGE - `bytes_reader` is exhausted before `num_bytes` of
  //                bytes is read.
  //
  // Other errors encountered by the writer will be returned as it is.
  Status WriteBytesFromStream(uint32_t field_number,
                              stream::Reader& bytes_reader,
                              size_t num_bytes,
                              ByteSpan stream_pipe_buffer) {
    return WriteLengthDelimitedFieldFromStream(
        field_number, bytes_reader, num_bytes, stream_pipe_buffer);
  }

  // Writes a proto string key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteString(uint32_t field_number, std::string_view value) {
    return WriteBytes(field_number, std::as_bytes(std::span(value)));
  }

  // Writes a proto string key-value pair.
  //
  // Precondition: Encoder has no active child encoder.
  Status WriteString(uint32_t field_number, const char* value, size_t len) {
    return WriteBytes(field_number, std::as_bytes(std::span(value, len)));
  }

  // Writes a proto 'string' field from the stream bytes_reader.
  //
  // The payload for the value is provided through the stream::Reader
  // `bytes_reader`. The method reads a chunk of the data from the reader using
  // the `stream_pipe_buffer` and writes it to the encoder.
  //
  // Precondition: The stream_pipe_buffer.byte_size() >= 1
  // Precondition: Encoder has no active child encoder.
  //
  // Returns:
  // OK - String field is written successfully.
  // RESOURCE_EXHAUSTED - Exceeds write limits.
  // OUT_OF_RANGE - `bytes_reader` is exhausted before `num_bytes` of
  //                bytes is read.
  //
  // Other errors encountered by the writer will be returned as it is.
  Status WriteStringFromStream(uint32_t field_number,
                               stream::Reader& bytes_reader,
                               size_t num_bytes,
                               ByteSpan stream_pipe_buffer) {
    return WriteBytesFromStream(
        field_number, bytes_reader, num_bytes, stream_pipe_buffer);
  }

 protected:
  // Specialized move constructor used only for codegen.
  //
  // Postcondition: The other encoder is invalidated and cannot be used as it
  //     acts like a parent encoder with an active child encoder.
  constexpr StreamEncoder(StreamEncoder&& other)
      : status_(other.status_),
        parent_(other.parent_),
        nested_field_number_(other.nested_field_number_),
        memory_writer_(std::move(other.memory_writer_)),
        writer_(&other.writer_ == &other.memory_writer_ ? memory_writer_
                                                        : other.writer_) {
    PW_ASSERT(nested_field_number_ == 0);
    // Make the nested encoder look like it has an open child to block writes
    // for the remainder of the object's life.
    other.nested_field_number_ = kFirstReservedNumber;
    other.parent_ = nullptr;
  }

 private:
  friend class MemoryEncoder;

  enum class VarintEncodeType {
    kNormal,
    kZigZag,
  };

  constexpr StreamEncoder(StreamEncoder& parent, ByteSpan scratch_buffer)
      : status_(scratch_buffer.empty() ? Status::ResourceExhausted()
                                       : OkStatus()),
        parent_(&parent),
        nested_field_number_(0),
        memory_writer_(scratch_buffer),
        writer_(memory_writer_) {}

  bool nested_encoder_open() const { return nested_field_number_ != 0; }

  // CloseNestedMessage() is called on the parent encoder as part of the nested
  // encoder destructor.
  void CloseNestedMessage(StreamEncoder& nested);

  // Implementation for encoding all varint field types.
  Status WriteVarintField(uint32_t field_number, uint64_t value);

  // Implementation for encoding all length-delimited field types.
  Status WriteLengthDelimitedField(uint32_t field_number, ConstByteSpan data);

  // Encoding of length-delimited field where payload comes from `bytes_reader`.
  Status WriteLengthDelimitedFieldFromStream(uint32_t field_number,
                                             stream::Reader& bytes_reader,
                                             size_t num_bytes,
                                             ByteSpan stream_pipe_buffer);

  // Implementation for encoding all fixed-length integer types.
  Status WriteFixed(uint32_t field_number, ConstByteSpan data);

  // Encodes a base-128 varint to the buffer.
  Status WriteVarint(uint64_t value);

  Status WriteZigzagVarint(int64_t value) {
    return WriteVarint(varint::ZigZagEncode(value));
  }

  // Writes a list of varints to the buffer in length-delimited packed encoding.
  template <typename T, typename = std::enable_if_t<std::is_integral<T>::value>>
  Status WritePackedVarints(uint32_t field_number,
                            std::span<T> values,
                            VarintEncodeType encode_type) {
    static_assert(std::is_same<T, const uint32_t>::value ||
                      std::is_same<T, const int32_t>::value ||
                      std::is_same<T, const uint64_t>::value ||
                      std::is_same<T, const int64_t>::value,
                  "Packed varints must be of type uint32_t, int32_t, uint64_t, "
                  "or int64_t");

    size_t payload_size = 0;
    for (T val : values) {
      if (encode_type == VarintEncodeType::kZigZag) {
        int64_t integer =
            static_cast<int64_t>(static_cast<std::make_signed_t<T>>(val));
        payload_size += varint::EncodedSize(varint::ZigZagEncode(integer));
      } else {
        uint64_t integer = static_cast<uint64_t>(val);
        payload_size += varint::EncodedSize(integer);
      }
    }

    if (!UpdateStatusForWrite(field_number, WireType::kDelimited, payload_size)
             .ok()) {
      return status_;
    }

    WriteVarint(FieldKey(field_number, WireType::kDelimited))
        .IgnoreError();  // TODO(pwbug/387): Handle Status properly
    WriteVarint(payload_size)
        .IgnoreError();  // TODO(pwbug/387): Handle Status properly
    for (T value : values) {
      if (encode_type == VarintEncodeType::kZigZag) {
        WriteZigzagVarint(static_cast<std::make_signed_t<T>>(value))
            .IgnoreError();  // TODO(pwbug/387): Handle Status properly
      } else {
        WriteVarint(value)
            .IgnoreError();  // TODO(pwbug/387): Handle Status properly
      }
    }

    return status_;
  }

  // Writes a list of fixed-size types to the buffer in length-delimited
  // packed encoding. Only float, double, uint32_t, int32_t, uint64_t, and
  // int64_t are permitted
  Status WritePackedFixed(uint32_t field_number,
                          std::span<const std::byte> values,
                          size_t elem_size);

  // Checks if a write is invalid or will cause the encoder to enter an error
  // state, and preemptively sets this encoder's status to that error to block
  // the write. Only the first error encountered is tracked.
  //
  // Precondition: Encoder has no active child encoder.
  //
  // Returns:
  //   InvalidArgument: The field number provided was invalid.
  //   ResourceExhausted: The requested write would have exceeded the
  //     stream::Writer's conservative write limit.
  //   Other: If any Write() operations on the stream::Writer caused an error,
  //     that error will be repeated here.
  Status UpdateStatusForWrite(uint32_t field_number,
                              WireType type,
                              size_t data_size);

  // The current encoder status. This status is only updated to reflect the
  // first error encountered. Any further write operations are blocked when the
  // encoder enters an error state.
  Status status_;

  // If this is a nested encoder, this points to the encoder that created it.
  // For user-created MemoryEncoders, parent_ points to this object as an
  // optimization for the MemoryEncoder and nested encoders to use the same
  // underlying buffer.
  StreamEncoder* parent_;

  // If an encoder has a child encoder open, this is the field number of that
  // submessage. Otherwise, this is 0 to indicate no child encoder is open.
  uint32_t nested_field_number_;

  // This memory writer is used for staging proto submessages to the
  // scratch_buffer.
  stream::MemoryWriter memory_writer_;

  // All proto encode operations are directly written to this writer.
  stream::Writer& writer_;
};

// A protobuf encoder that writes directly to a provided buffer.
//
// Example:
//
//   // Writes a proto response to the provided buffer, returning the encode
//   // status and number of bytes written.
//   StatusWithSize WriteProtoResponse(ByteSpan response) {
//     // All proto writes are directly written to the `response` buffer.
//     MemoryEncoder encoder(response);
//     encoder.WriteUint32(kMagicNumberField, 0x1a1a2b2b);
//     encoder.WriteString(kFavoriteFood, "cookies");
//     return StatusWithSize(encoder.status(), encoder.size());
//   }
//
// Note: Avoid using a MemoryEncoder reference as an argument for a function.
// The StreamEncoder is more generic.
class MemoryEncoder : public StreamEncoder {
 public:
  constexpr MemoryEncoder(ByteSpan dest) : StreamEncoder(*this, dest) {}

  // Precondition: Encoder has no active child encoder.
  //
  // Postcondition: If this encoder is a nested one, the parent encoder is
  //     unlocked and proto encoding may resume on the parent.
  ~MemoryEncoder() = default;

  // Disallow copy/assign to avoid confusion about who owns the buffer.
  MemoryEncoder(const MemoryEncoder& other) = delete;
  MemoryEncoder& operator=(const MemoryEncoder& other) = delete;

  // It's not safe to move an encoder as it could cause another encoder's
  // parent_ pointer to become invalid.
  MemoryEncoder& operator=(MemoryEncoder&& other) = delete;

  const std::byte* data() const { return memory_writer_.data(); }
  size_t size() const { return memory_writer_.bytes_written(); }

 protected:
  // This is needed by codegen.
  MemoryEncoder(MemoryEncoder&& other) = default;
};

}  // namespace pw::protobuf
