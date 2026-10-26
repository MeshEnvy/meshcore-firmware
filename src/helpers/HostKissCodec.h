#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mesh { namespace hostkiss {

// Generic KISS framing, independently implemented. No RNode commands or
// over-air fragmentation are implied by the host stream.
class Codec {
 public:
  static constexpr uint8_t Fend = 0xc0, Fesc = 0xdb, Tfend = 0xdc, Tfesc = 0xdd;
  static constexpr size_t MaximumFrame = 255, QueueDepth = 2;
  static constexpr size_t MaximumEncoded = MaximumFrame * 2 + 3;
  static constexpr uint8_t Ready = 0x0f, TxDone = 0xf8;
 private:
  struct Frame { uint8_t bytes[MaximumFrame]; uint16_t length = 0; } tx_[QueueDepth];
  struct Output { uint8_t bytes[MaximumEncoded]; uint16_t length = 0; } out_[QueueDepth];
  uint8_t tx_head_ = 0, tx_count_ = 0, out_head_ = 0, out_count_ = 0;
  uint8_t input_[MaximumFrame];
  uint16_t input_size_ = 0, output_offset_ = 0;
  uint8_t command_ = 0;
  bool framed_ = false, has_command_ = false, escaped_ = false, discard_ = false;
  bool return_ = false;
  uint32_t malformed_ = 0, dropped_ = 0;

  void malformed() { if (!discard_) ++malformed_; discard_ = true; }
  bool queueOutput(uint8_t command, const uint8_t* bytes, size_t length) {
    if (length > MaximumFrame || out_count_ >= QueueDepth || (!bytes && length)) {
      ++dropped_; return false;
    }
    auto& frame = out_[(out_head_ + out_count_) % QueueDepth];
    size_t at = 0;
    frame.bytes[at++] = Fend;
    // Commands used by this service never need escaping.
    frame.bytes[at++] = command;
    for (size_t i = 0; i < length; ++i) {
      if (bytes[i] == Fend || bytes[i] == Fesc) {
        frame.bytes[at++] = Fesc;
        frame.bytes[at++] = bytes[i] == Fend ? Tfend : Tfesc;
      } else frame.bytes[at++] = bytes[i];
    }
    frame.bytes[at++] = Fend;
    frame.length = uint16_t(at);
    ++out_count_;
    return true;
  }
 public:
  void reset() {
    tx_head_ = tx_count_ = out_head_ = out_count_ = 0;
    input_size_ = output_offset_ = 0;
    framed_ = has_command_ = escaped_ = discard_ = return_ = false;
  }
  bool canAcceptByte() const { return tx_count_ < QueueDepth && !return_; }
  // False leaves ownership of this byte with the transport (backpressure).
  bool feed(uint8_t byte) {
    if (!canAcceptByte()) return false;
    if (byte == Fend) {
      if (framed_ && has_command_ && !discard_) {
        if (escaped_) malformed();
        else if (command_ == 0xff) {
          if (!input_size_) return_ = true;
          else malformed();
        } else if (command_ == 0) {
          if (!input_size_) malformed();
          else {
            auto& frame = tx_[(tx_head_ + tx_count_) % QueueDepth];
            memcpy(frame.bytes, input_, input_size_);
            frame.length = input_size_;
            ++tx_count_;
          }
        }
      }
      framed_ = true;
      has_command_ = escaped_ = discard_ = false;
      input_size_ = 0;
      return true;
    }
    if (!framed_ || discard_) return true;
    if (!has_command_) {
      has_command_ = true;
      command_ = byte;
      // Port 0 DATA always means radio2. Other ports and unsupported KISS
      // parameter commands cannot alter the radio or reach MeshCore radio1.
      if (command_ != 0 && command_ != 0xff) discard_ = true;
      return true;
    }
    if (escaped_) {
      escaped_ = false;
      if (byte == Tfend) byte = Fend;
      else if (byte == Tfesc) byte = Fesc;
      else { malformed(); return true; }
    } else if (byte == Fesc) {
      escaped_ = true;
      return true;
    }
    if (input_size_ >= MaximumFrame) { malformed(); return true; }
    input_[input_size_++] = byte;
    return true;
  }
  bool returned() const { return return_; }
  bool peekTransmit(const uint8_t*& bytes, size_t& length) const {
    if (!tx_count_) { bytes = nullptr; length = 0; return false; }
    bytes = tx_[tx_head_].bytes;
    length = tx_[tx_head_].length;
    return true;
  }
  void consumeTransmit() {
    if (tx_count_) { tx_head_ = (tx_head_ + 1) % QueueDepth; --tx_count_; }
  }
  bool queueReceive(const uint8_t* bytes, size_t length) {
    if (!length) { ++dropped_; return false; }
    return queueOutput(0, bytes, length);
  }
  bool queueReady() { const uint8_t value = 1; return queueOutput(Ready, &value, 1); }
  bool queueTxDone(bool success) {
    // Match the existing standalone modem's SetHardware extension. A command
    // byte of 0xf8 would instead mean port 15, command 8 to a generic parser.
    const uint8_t value[] = {TxDone, uint8_t(success ? 1 : 0)};
    return queueOutput(0x06, value, sizeof(value));
  }
  bool peekOutput(const uint8_t*& bytes, size_t& length) const {
    if (!out_count_) { bytes = nullptr; length = 0; return false; }
    bytes = out_[out_head_].bytes + output_offset_;
    length = out_[out_head_].length - output_offset_;
    return true;
  }
  void consumeOutput(size_t length) {
    if (!out_count_) return;
    const size_t remaining = out_[out_head_].length - output_offset_;
    if (length >= remaining) {
      output_offset_ = 0;
      out_head_ = (out_head_ + 1) % QueueDepth;
      --out_count_;
    } else output_offset_ += uint16_t(length);
  }
  uint8_t queuedTransmit() const { return tx_count_; }
  uint8_t queuedOutput() const { return out_count_; }
  uint32_t malformedFrames() const { return malformed_; }
  uint32_t droppedFrames() const { return dropped_; }
};

}}  // namespace mesh::hostkiss
