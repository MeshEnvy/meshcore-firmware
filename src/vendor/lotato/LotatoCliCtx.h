#pragma once

#include <cstdint>

class MyMesh;

/** Passed as `locommand::Context::app_ctx` for lotato + wifi CLI handlers. */
struct LotatoCliCtx {
  MyMesh* self;
  uint32_t sender_ts;
};
