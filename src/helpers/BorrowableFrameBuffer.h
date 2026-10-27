#pragma once

#include <new>
#include <stddef.h>
#include <string.h>

namespace mesh {

template<class Frame, size_t Count> class InlineRetainedFrames {
  Frame frames[Count];
public:
  static constexpr size_t frame_count = Count;
  Frame& at(size_t index) { return frames[index]; }
};

// One owner at a time: an offline FIFO borrows the workspace's storage while
// idle. Switching owners compacts the ring in place, without allocating RAM or
// dropping unread frames. The caller owns the FIFO's length and head index.
// Qualified boards can supply a physically separate retained-frame bank. It
// replaces only existing retained slots, never the extra frames or workspace.
template<class Frame, size_t Capacity, size_t Reserved, class Workspace,
         class RetainedFrames = InlineRetainedFrames<Frame, Reserved>>
class BorrowableFrameBuffer {
  static_assert(Reserved > 0 && Reserved < Capacity, "Invalid shared queue capacities");
  static_assert(RetainedFrames::frame_count == Reserved,
                "The retained backing must preserve every active-OTA slot");
  struct ExtraFrames { Frame frames[Capacity - Reserved]; };
  static_assert(sizeof(Workspace) <= sizeof(ExtraFrames), "mOTA exceeds borrowed queue storage");

  RetainedFrames _retained;
  union Extra {
    ExtraFrames queue;
    Workspace workspace;
    Extra() : queue() {}
    ~Extra() {}
  } _extra;
  bool _borrowed = false;

  void reverse(size_t first, size_t end) {
    while (first < end && first < --end) {
      unsigned char saved[sizeof(Frame)];
      memcpy(saved, &at(first), sizeof(Frame));
      memcpy(&at(first), &at(end), sizeof(Frame));
      memcpy(&at(end), saved, sizeof(Frame));
      ++first;
    }
  }

  void compact(int& head) {
    if (head == 0) return;
    // Rotate the entire ring, including unused slots, using one frame of stack.
    reverse(0, head);
    reverse(head, capacity());
    reverse(0, capacity());
    head = 0;
  }

public:
  BorrowableFrameBuffer() = default;
  BorrowableFrameBuffer(Frame* fast, Frame* slow) : _retained(fast, slow) {}
  BorrowableFrameBuffer(const BorrowableFrameBuffer&) = delete;
  BorrowableFrameBuffer& operator=(const BorrowableFrameBuffer&) = delete;
  ~BorrowableFrameBuffer() {
    if (_borrowed) _extra.workspace.~Workspace();
  }

  size_t capacity() const { return _borrowed ? Reserved : Capacity; }
  Frame& at(size_t physical_index) {
    return physical_index < Reserved ? _retained.at(physical_index)
                                    : _extra.queue.frames[physical_index - Reserved];
  }

  Workspace* acquire(int length, int& head) {
    if (_borrowed) return &_extra.workspace;
    if (length > static_cast<int>(Reserved)) return nullptr;
    compact(head);
    Workspace* workspace = new (&_extra.workspace) Workspace();
    _borrowed = true;
    return workspace;
  }

  void release(int& head) {
    if (!_borrowed) return;
    compact(head);
    _extra.workspace.~Workspace();
    new (&_extra.queue) ExtraFrames();
    _borrowed = false;
  }
};

} // namespace mesh
