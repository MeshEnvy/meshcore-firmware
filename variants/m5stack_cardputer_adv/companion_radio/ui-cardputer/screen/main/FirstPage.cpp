#include "FirstPage.h"

int FirstPage::render(CardputerDisplay &lcd) {
  char tmp[80];

  if (_p->getUiTask()->hasConnection()) {
    lcd.setColor(CardputerDisplay::P_GREEN);
    lcd.setTextSize(1);
    lcd.drawTextCentered(lcd.width() / 2, 43, "< Connected >");
  } else if (the_mesh_cp.getBLEPin() != 0) { // BT pin
    lcd.setColor(CardputerDisplay::P_RED);
    lcd.setTextSize(2);
    snprintf(tmp, sizeof(tmp), "Pin: %d", the_mesh_cp.getBLEPin());
    lcd.drawTextCentered(lcd.width() / 2, 43, tmp);
  }

  lcd.setTextSize(1);

  if (_p->getUnread()->countChats() > 0) {
    lcd.setColor(CardputerDisplay::P_YELLOW);
    snprintf(tmp, sizeof(tmp), "Unread: %u chats, %u msgs", _p->getUnread()->countChats(),
            _p->getUnread()->countMessages());
    lcd.drawTextCentered(lcd.width() / 2, lcd.height() - lcd.getRowHeight() * 3 - 2, tmp);
  }

  lcd.setColor(CardputerDisplay::P_GREEN);
  lcd.drawTextCentered(lcd.width() / 2, lcd.height() - lcd.getRowHeight() * 2 - 1, "Opt / Fn+S: Settings");

  lcd.setColor(CardputerDisplay::P_ORANGE);
  lcd.drawTextCentered(lcd.width() / 2, lcd.height() - lcd.getRowHeight(), "T / Fn+T: Tools");

  return 5000;
}

bool FirstPage::handleInput(Keyboard::Event &e) {
  if (e.modifiers.opt) {
    _p->getUiTask()->gotoSettingsScreen();
    return true;
  }

  if (e.key == Keyboard::KEY_T) {
    _p->getUiTask()->gotoToolsScreen();
    return true;
  }

  return false;
}
