#pragma once

#include <Arduino.h>
#include <atomic>

namespace mesh {

// Guard cached text Stream references as well as new port lookups. Binary
// owners write through their separate raw facade and are never wrapped here.
class UsbBinaryTextGuard : public Stream {
  std::atomic<Stream*> _port{nullptr};
  bool (*_owned)();
  bool (*_enter)();
  void (*_leave)();
  bool enter() { return _enter ? _enter() : !_owned(); }
  void leave() { if (_leave) _leave(); }
public:
  explicit UsbBinaryTextGuard(bool (*owned)(), bool (*enter)() = nullptr,
                             void (*leave)() = nullptr)
      : _owned(owned), _enter(enter), _leave(leave) { }
  void use(Stream& port) { _port.store(&port, std::memory_order_release); }
  int available() override {
    auto* port = _port.load(std::memory_order_acquire);
    return !_owned() && port ? port->available() : 0;
  }
  int read() override {
    auto* port = _port.load(std::memory_order_acquire);
    return !_owned() && port ? port->read() : -1;
  }
  int peek() override {
    auto* port = _port.load(std::memory_order_acquire);
    return !_owned() && port ? port->peek() : -1;
  }
  void flush() override {
    if (!enter()) return;
    auto* port = _port.load(std::memory_order_acquire);
    if (port) port->flush();
    leave();
  }
  int availableForWrite() override {
    auto* port = _port.load(std::memory_order_acquire);
    return !_owned() && port ? port->availableForWrite() : 0;
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    if (!data) return 0;
    // Suppressed text is deliberately consumed, never deferred into the next
    // CLI session. The caller must not retry it after the raw owner releases.
    if (!enter()) return length;
    auto* port = _port.load(std::memory_order_acquire);
    const size_t written = port ? port->write(data, length) : 0;
    leave();
    return written;
  }
};

}  // namespace mesh
