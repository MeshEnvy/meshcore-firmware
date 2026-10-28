#include "RecentAdvertsPage.h"

int RecentAdvertsPage::render(CardputerDisplay &lcd) {
  char tmp[80];
  AdvertPath recent[UI_RECENT_LIST_SIZE];

  the_mesh_cp.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
  lcd.setColor(CardputerDisplay::P_GREEN);

  const int row = lcd.getRowHeight();
  int y = 20;
  lcd.drawTextCentered(lcd.width() / 2, y, "Recent adverts");

  y += row + 1;

  for (int i = 0; i < UI_RECENT_LIST_SIZE && y + row <= lcd.height(); i++, y += row) {
    auto a = &recent[i];
    if (a->name[0] == 0) continue; // empty slot
    int secs = _rtc->getCurrentTime() - a->recv_timestamp;
    if (secs < 60) {
      snprintf(tmp, sizeof(tmp), "%ds", secs);
    } else if (secs < 60 * 60) {
      snprintf(tmp, sizeof(tmp), "%dm", secs / 60);
    } else {
      snprintf(tmp, sizeof(tmp), "%dh", secs / (60 * 60));
    }

    int timestamp_width = lcd.getTextWidth(tmp);
    int max_name_width = lcd.width() - timestamp_width - 1;

    lcd.drawTextEllipsized(0, y, max_name_width, a->name);
    lcd.setCursor(lcd.width() - timestamp_width - 1, y);
    lcd.print(tmp);
  }

  return 5000;
}
