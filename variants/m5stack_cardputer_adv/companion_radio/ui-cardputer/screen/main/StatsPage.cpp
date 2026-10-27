#include "StatsPage.h"

int StatsPage::render(CardputerDisplay &lcd) {
  char tmp[80];
  const int row = display.getRowHeight();
  int y = 20;
  display.setColor(CardputerDisplay::P_GREEN);
  display.drawTextCentered(display.width() / 2, y, "Info");
  display.setColor(CardputerDisplay::P_YELLOW);

  CardputerUITask *task = _p->getUiTask();
  CompanionNodePrefs *prefs = the_mesh_cp.getNodePrefs();
  const uint16_t mv = task->getBattMilliVoltsCorrected();
  int pct = ((int)mv - BATT_MIN_MILLIVOLTS) * 100 / (BATT_MAX_MILLIVOLTS - BATT_MIN_MILLIVOLTS);
  pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);

  const char *lines[6];
  char l0[48], l1[32], l2[40], l3[32], l4[32], l5[32];
  snprintf(l0, sizeof(l0), "Name: %s", prefs->node_name);
  snprintf(l1, sizeof(l1), "Battery: %d%% %umV", pct, mv);
  snprintf(l2, sizeof(l2), "%.3f SF%d BW%g", prefs->freq, prefs->sf, prefs->bw);
  snprintf(l3, sizeof(l3), "Noise: %d dBm", radio_driver.getNoiseFloor());
  snprintf(l4, sizeof(l4), "Packets RX: %llu", (unsigned long long)the_mesh_cp.receivedPacketsCount());
  snprintf(l5, sizeof(l5), "Unsynced msgs: %d", task->getMsgCount());
  lines[0] = l0; lines[1] = l1; lines[2] = l2; lines[3] = l3; lines[4] = l4; lines[5] = l5;

  for (int i = 0; i < 6; i++) {
    y += row;
    if (y + row > display.height()) break;
    display.drawTextEllipsized(5, y, display.width() - 10, lines[i]);
  }

  (void)tmp;
  return 5000;
}
