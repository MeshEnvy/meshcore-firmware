#include "ChannelsPage.h"

int ChannelsPage::getChannelCount() { // not sure if there is no gaps
  ChannelDetails channel;
  int num = 0;

  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!the_mesh_cp.getChannel(i, channel) || strlen(channel.name) == 0) {
      break;
    }
    num++;
  }

  return num;
}

int ChannelsPage::render(CardputerDisplay &lcd) {
  const int row = lcd.getRowHeight();
  const int title_y = 20;
  const int list_y = title_y + row + 1;
  const int rows = std::max(1, (lcd.height() - 1 - list_y) / row);
  const int name_x = 5 + lcd.getTextWidth(">") + 6;

  lcd.setColor(CardputerDisplay::P_GREEN);
  lcd.drawTextCentered(lcd.width() / 2, title_y, "Channels");

  int real_idx = 0;
  int list_page = channel_list_idx / rows;
  int list_idx = channel_list_idx % rows;
  char buf[6];

  for (int i = 0; i < rows; i++) {
    lcd.setColor(CardputerDisplay::P_GREEN);
    const int y = list_y + i * row;

    ChannelDetails channel;
    real_idx = list_page * rows + i;

    if (!the_mesh_cp.getChannel(real_idx, channel) || strlen(channel.name) == 0) {
      break;
    }

    int unread_count = 0;

    for (int j = 0; j < _p->getUnread()->countChats(); j++) {
      const UnreadCounterItem *item = _p->getUnread()->get(j);
      if (item && item->is_channel && memcmp(item->pkey, channel.channel.secret, CONTACT_LOOKUP_BYTES) == 0) {
        unread_count = item->count;
        break;
      }
    }

    if (i == list_idx) {
      lcd.drawTextLeftAlign(5, y, ">");
    }

    int right_pad = 0;

    if (unread_count > 0) {
      snprintf(buf, sizeof(buf), "%d", unread_count);
      right_pad = lcd.getTextWidth(buf) + 5;
      lcd.setColor(CardputerDisplay::P_ORANGE);
      lcd.drawTextRightAlign(lcd.width() - 1, y, buf);
    }

    lcd.drawTextEllipsized(name_x, y, lcd.width() - name_x - right_pad, channel.name);
  }

  return 5000;
}

bool ChannelsPage::handleInput(Keyboard::Event &e) {
  if (e.key == Keyboard::ARROW_UP) {
    if (channel_list_idx == 0) {
      channel_list_idx = std::max(getChannelCount() - 1, 0);
    } else {
      channel_list_idx--;
    }
    return true;
  }
  if (e.key == Keyboard::ARROW_DOWN) {
    if (channel_list_idx < getChannelCount() - 1) {
      channel_list_idx++;
    } else {
      channel_list_idx = 0;
    }
    return true;
  }
  if (e.key == Keyboard::KEY_RETURN) {
    ChannelDetails c;
    if (the_mesh_cp.getChannel(channel_list_idx, c) && strlen(c.name) > 0) {
      _p->selectChannel(c, channel_list_idx);
    }
    return true;
  }

  return false;
}
