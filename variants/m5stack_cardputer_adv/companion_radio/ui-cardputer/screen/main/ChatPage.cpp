#include "ChatPage.h"

#include "helpers.h"

int ChatPage::render(CardputerDisplay &lcd) {
  refreshSelectedContact();
  refreshSelectedChannel();
  char buf[64];
  const int row = display.getRowHeight();
  const int line_h = display.getFontLineHeight();  // advance used when print() wraps
  const int title_y = 20;
  const int input_y = display.height() - row - 2;
  const int history_top = title_y + row + 2;
  const int max_lines = std::max(1, (input_y - 3 - history_top) / line_h);

  display.setColor(CardputerDisplay::P_GREEN);

  if (current_contact_idx == -1 && current_channel_idx == -1) {
    display.setColor(CardputerDisplay::P_ORANGE);
    display.drawTextCentered(display.width() / 2, title_y, "No chat selected");
  } else if (current_contact_idx != -1) {
    // out_path_len packs the path hash count in the lower 6 bits, and (hash size - 1) in the upper 2 bits
    uint8_t path_hops = current_contact.out_path_len & 63;
    if (current_contact.out_path_len == OUT_PATH_UNKNOWN) {
      snprintf(buf, sizeof(buf), "%s F", current_contact.name);
    } else if (path_hops == 0) {
      snprintf(buf, sizeof(buf), "%s D", current_contact.name);
    } else {
      snprintf(buf, sizeof(buf), "%s %uH", current_contact.name, path_hops);
    }
    display.drawTextCentered(display.width() / 2, title_y, buf);
  } else if (current_channel_idx != -1) {
    display.drawTextCentered(display.width() / 2, title_y, current_channel.name);
  }

  display.setColor(CardputerDisplay::P_GREEN);

  int current_y = history_top + (max_lines - 1) * line_h;
  int total_lines_drawn = 0;

  int start_index = (int)chat_history.count() - 1 - chat_history_offset;

  for (int i = start_index; i >= 0; i--) {
    if (total_lines_drawn >= max_lines) {
      break;
    }

    const HistoryMessage *msg;

    if (!chat_history.get(i, msg)) {
      break;
    }

    int text_w = display.getTextWidth(msg->text);
    int message_lines = (text_w / display.width()) + 1;

    if (total_lines_drawn + message_lines > max_lines) {
      message_lines = max_lines - total_lines_drawn;
    }

    int message_top_y = current_y - ((message_lines - 1) * line_h);

    if (msg->out) {
      if (text_w <= display.width()) {
        display.setCursor(display.width() - text_w - 5, message_top_y);
      } else {
        display.setCursor(5, message_top_y);
      }
    } else {
      display.setCursor(5, message_top_y);
    }

    if ((text_w / display.width()) + 1 == message_lines) {
      display.print(msg->text); // wraps text automatically
    } else {
      display.drawTextEllipsized(5, message_top_y, display.width(), msg->text);
    }

    current_y -= (message_lines * line_h);
    total_lines_drawn += message_lines;
  }

  int start_x = 5;
  display.drawRect(1, input_y - 2, display.width() - 1, 1);

  display.setColor(CardputerDisplay::P_WHITE);

  int available_width = display.width() - start_x;

  if (chat_text_box.isEmpty()) {
    lcd.setColor(CardputerDisplay::P_GRAY);
    lcd.drawTextLeftAlign(5, input_y, "Write a message...");
  } else {
    display.drawTextLeftAlignWithScroll(start_x, input_y, available_width,
                                        chat_text_box.c_str());
  }

  return 15000;
}

void ChatPage::sendChatMessage() {
  if (chat_text_box.isEmpty()) {
    return;
  }

  refreshSelectedContact();
  refreshSelectedChannel();
  uint32_t ts = the_mesh_cp.getRTCClock()->getCurrentTimeUnique();

  if (current_contact_idx != -1 && current_contact.type == ADV_TYPE_CHAT) { // direct msg
    int result = the_mesh_cp.sendDirectMessage(current_contact, ts, chat_text_box.c_str());

    if (result != MSG_SEND_FAILED) {
      HistoryMessage *msg = chat_history.push_ref();
      msg->out = true;
      snprintf(msg->text, MAX_MESSAGE_LENGTH, "%s", chat_text_box.c_str());

      last_sent_message = chat_text_box;
      chat_text_box.clear();
      chat_history_offset = 0;
    } else {
      _p->getUiTask()->showAlert("Unable to send message", 2000);
    }
    return;
  }

  if (current_channel_idx != -1) { // channel msg

    bool ok = the_mesh_cp.sendChannelMessage(current_channel_idx, ts,
                                             chat_text_box.c_str(), chat_text_box.length());
    if (ok) {
      HistoryMessage *msg = chat_history.push_ref();
      msg->out = true;

      snprintf(msg->text, MAX_MESSAGE_LENGTH, "%s", chat_text_box.c_str());

      last_sent_message = chat_text_box;
      chat_text_box.clear();
      chat_history_offset = 0;
      _p->getUiTask()->showAlert("Waiting for repeats...", 2000);
    } else {
      _p->getUiTask()->showAlert("Unable to send message", 2000);
    }
    return;
  }
}

bool ChatPage::handleInput(Keyboard::Event &e) {
  CardputerLayout *lay = _p->getUiTask()->getBoard()->getLayout();

  if (e.modifiers.ctrl && e.key == Keyboard::KEY_T) {
    the_mesh_cp.cancelResending();
    return true;
  }

  if (the_mesh_cp.isMessageSending()) {
    return true;
  }

  if (e.key == Keyboard::KEY_BACKSPACE) {
    Helpers::removeLastStringChar(chat_text_box);
    return true;
  }

  if (e.key == Keyboard::KEY_ESC) {
    _p->returnToLastPage();
    return true;
  }

  if (e.modifiers.ctrl && e.key == Keyboard::ARROW_UP && chat_text_box.isEmpty()) {
    chat_text_box = last_sent_message;
    return true;
  }

  if (e.key == Keyboard::KEY_RETURN) {
    sendChatMessage();
    return true;
  }

  // Determine if arrow keys should act as scrolling/navigation or text input
  bool fnPressed = (chat_text_box.length() == 0) ? !e.modifiers.fn : e.modifiers.fn;

  if (fnPressed) {
    if (e.key == Keyboard::ARROW_DOWN) {
      if (chat_history_offset > 0) {
        chat_history_offset--;
      }
      return true;
    }

    if (e.key == Keyboard::ARROW_UP) {
      if (chat_history.count() > 0 && chat_history_offset < (int)chat_history.count() - 1) {
        chat_history_offset++;
      }
      return true;
    }
  }

  bool skip_input = (fnPressed && (e.key == Keyboard::ARROW_RIGHT || e.key == Keyboard::ARROW_LEFT)) ||
                    e.key == Keyboard::KEY_ESC;

  if (!skip_input) {
    const char *repl = lay->lookup(e);

    if (repl[0]) {
      int maxlen = MAX_MESSAGE_LENGTH - strlen(_p->getUiTask()->getNodeName());

      if (chat_text_box.length() + strlen(repl) <= maxlen) {
        chat_text_box += repl;
      }
      return true;
    }
  }
  return false;
}

void ChatPage::selectContact(const ContactInfo &contact, int idx) {
  current_contact = contact;
  current_contact_idx = idx;
  current_channel_idx = -1;
  the_mesh_cp.loadMessageHistory(contact.id.pub_key, false, chat_history);
  chat_history_offset = 0;
  last_sent_message = "";

  const HistoryMessage *msg;
  for (size_t i = chat_history.count(); i > 0; i--) {
    if (chat_history.get(i - 1, msg) && msg->out) {
      last_sent_message = msg->text;
      break;
    }
  }
}

void ChatPage::selectChannel(const ChannelDetails &channel, int idx) {
  current_channel = channel;
  current_channel_idx = idx;
  current_contact_idx = -1;
  the_mesh_cp.loadMessageHistory(channel.channel.secret, true, chat_history);
  chat_history_offset = 0;
  last_sent_message = "";

  const HistoryMessage *msg;
  for (size_t i = chat_history.count(); i > 0; i--) {
    if (chat_history.get(i - 1, msg) && msg->out) {
      last_sent_message = msg->text;
      break;
    }
  }
}

bool ChatPage::onChannelMessageRecv(const mesh::GroupChannel &channel, const char *text) {

  if (current_channel_idx != -1 &&
      memcmp(channel.secret, current_channel.channel.secret, PUB_KEY_SIZE) == 0) {
    HistoryMessage *msg = chat_history.push_ref();
    msg->out = false;
    snprintf(msg->text, MAX_MESSAGE_LENGTH, "%s", text);
    chat_history_offset = 0;
    return true;
  }
  return false;
}

bool ChatPage::onContactMessageRecv(const ContactInfo &contact, const char *text) {
  if (current_contact_idx != -1 &&
      memcmp(contact.id.pub_key, current_contact.id.pub_key, PUB_KEY_SIZE) == 0) {
    HistoryMessage *msg = chat_history.push_ref();
    msg->out = false;
    snprintf(msg->text, MAX_MESSAGE_LENGTH, "%s", text);
    chat_history_offset = 0;
    return true;
  }
  return false;
}

void ChatPage::refreshSelectedContact() {
  if (current_contact_idx != -1) {
    ContactInfo *contact = the_mesh_cp.lookupPersistentContactByPubKey(current_contact.id.pub_key, PUB_KEY_SIZE);
    if (contact) {
      current_contact = *contact;
    } else {
      current_contact_idx = -1;
      current_contact = ContactInfo{};
      chat_history.clear();
      chat_history_offset = 0;
      last_sent_message = "";
    }
  }
}

bool ChatPage::refreshSelectedChannel() {
  if (current_channel_idx == -1) return false;
  ChannelDetails channel;
  if (!the_mesh_cp.getChannel(current_channel_idx, channel) || !channel.name[0]
      || memcmp(channel.channel.secret, current_channel.channel.secret, PUB_KEY_SIZE) != 0) {
    current_channel_idx = -1;
    current_channel = ChannelDetails{};
    chat_history.clear();
    chat_history_offset = 0;
    last_sent_message = "";
    return false;
  }
  current_channel = channel;
  return true;
}
