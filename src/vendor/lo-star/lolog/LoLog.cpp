#include "LoLog.h"

#include <loserial/LoSerial.h>
#include <losettings/ConfigHub.h>
#include <losettings/LoSettings.h>

#include <cstdio>

namespace lolog {

namespace {

bool g_verbose = false;
bool g_registered = false;

void lolog_on_cfg_changed(void*) { LoLog::loadFromSettings(); }

void emit(const char* level, const char* tag, const char* fmt, va_list ap) {
  loserial::LoSerial::printf("[%s] %s: ", level, tag ? tag : "?");
  loserial::LoSerial::vprintf(fmt, ap);
  loserial::LoSerial::printf("\r\n");
}

}  // namespace

void LoLog::registerConfigSchema() {
  if (g_registered) return;
  g_registered = true;

  static const losettings::ConfigEntry kLolog[] = {
      {"verbose",
       losettings::ConfigValueKind::Bool,
       false,
       0,
       0,
       nullptr,
       "LoLog verbose (debug) output",
       false,
       false,
       0,
       0,
       lolog_on_cfg_changed,
       nullptr},
  };
  static const losettings::ConfigRegistry kReg("lolog", kLolog,
                                               (int)(sizeof(kLolog) / sizeof(kLolog[0])));
  losettings::ConfigHub::instance().registerModule(kReg);
}

void LoLog::loadFromSettings() {
  losettings::LoSettings st("lolog");
  g_verbose = st.getBool("verbose", false);
}

bool LoLog::isVerbose() { return g_verbose; }

void LoLog::setVerbose(bool on) {
  g_verbose = on;
  losettings::LoSettings st("lolog");
  st.setBool("verbose", on);
}

void LoLog::debug(const char* tag, const char* fmt, ...) {
  if (!g_verbose) return;
  va_list ap;
  va_start(ap, fmt);
  emit("D", tag, fmt, ap);
  va_end(ap);
}

void LoLog::info(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("I", tag, fmt, ap);
  va_end(ap);
}

void LoLog::warn(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("W", tag, fmt, ap);
  va_end(ap);
}

void LoLog::error(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit("E", tag, fmt, ap);
  va_end(ap);
}

}  // namespace lolog
