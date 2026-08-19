#pragma once

/* On-device contact browsing, message reading, and canned-message sending.

   MeshCore's stock companion UI is three screens — splash, home, message preview —
   because the design puts the interface in the phone app. These screens add the parts
   that make the hardware usable on its own.

   INPUT: the L1 has a full 5-way joystick plus a back button, but stock MeshCore only
   reads left/right/press. ui-cj wires up, down and back itself (see UITask.cpp), so:

     Contacts / Messages / canned list  vertical lists  -- UP/DOWN scroll, ENTER acts
     Channels                           horizontal pages -- LEFT/RIGHT flip, ENTER acts
     Back button                        exits any screen (KEY_CANCEL)

   Vertical things use vertical keys and horizontal things use horizontal keys, which
   is what makes this read consistently with the stock home screen. LEFT/RIGHT are kept
   as aliases in the lists, and the trailing "Back" row stays as a fallback.

   Contacts, Channels and Messages are pages in the home rotation, opened with ENTER.
*/

#include <Arduino.h>
#include <MeshCore.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "CannedStore.h"
#include "MsgLog.h"

// Defined in main.cpp as a non-static global, so these screens reach the mesh without
// editing any upstream file -- the same reason ui-cj is a separate directory.
extern MyMesh the_mesh;

class UITask;

#define CJ_ROW_HEIGHT     11
#define CJ_LIST_TOP       14
#define CJ_MAX_VISIBLE     4

/* Shared list-scrolling behaviour. Keeps the selected row inside the visible window
   and wraps at both ends, which matters on a 4-row screen with 30+ contacts. */
class CJListScreen : public UIScreen {
protected:
  int _sel;
  int _top;

  CJListScreen() : _sel(0), _top(0) { }

  void clampWindow(int count) {
    if (count <= 0) { _sel = 0; _top = 0; return; }
    if (_sel < 0) _sel = count - 1;
    if (_sel >= count) _sel = 0;
    if (_sel < _top) _top = _sel;
    if (_sel >= _top + CJ_MAX_VISIBLE) _top = _sel - CJ_MAX_VISIBLE + 1;
    if (_top < 0) _top = 0;
  }

  /* `pos`/`total` render as "<2/9>" on the right. The chevrons matter: LEFT/RIGHT are
     the only movement keys this board has, so a vertical list would otherwise read as
     though RIGHT means "down". Everywhere else in this UI left/right flips between
     options, and the header is what reconciles the two. */
  void drawHeader(DisplayDriver& display, const char* title, int pos, int total) {
    char tmp[24];
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.setColor(UIColor::corp_blue);
    display.print(title);

    if (total > 0) {
      sprintf(tmp, "<%d/%d>", pos + 1, total);
    } else {
      sprintf(tmp, "<0>");
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);

    display.drawRect(0, 11, display.width(), 1);
  }

  void drawRow(DisplayDriver& display, int row, const char* text, bool selected) {
    int y = CJ_LIST_TOP + row * CJ_ROW_HEIGHT;
    if (selected) {
      display.setColor(UIColor::primary_txt);
      display.setCursor(0, y);
      display.print(">");
    }
    display.setColor(selected ? UIColor::primary_txt : UIColor::secondary_txt);

    // MUST ellipsize, not print(): a row wider than the display soft-wraps onto the
    // next row's line and the two collide. This is what garbled the message list.
    char filtered[64];
    display.translateUTF8ToBlocks(filtered, text, sizeof(filtered));
    display.drawTextEllipsized(8, y, display.width() - 10, filtered);
  }

  void drawEmpty(DisplayDriver& display, const char* msg) {
    display.setColor(UIColor::secondary_txt);
    display.setCursor(8, CJ_LIST_TOP + CJ_ROW_HEIGHT);
    display.print(msg);
  }
};

/* ---------------------------------------------------------------- contacts ---- */

class CJContactsScreen : public CJListScreen {
  UITask* _task;

public:
  CJContactsScreen(UITask* task) : _task(task) { }

  int selectedIndex() const { return _sel; }

  /* getContactByIdx() indexes the RAW contacts array, whose first MAX_ANON_CONTACTS
     slots are reserved anon entries that are memset to zero at init. getNumContacts()
     already excludes them, so a caller counting with one and indexing with the other
     reads blank records -- which rendered as a nameless " 0h" row. Upstream's own
     iterator documents the fix: startContactsIterator() begins at MAX_ANON_CONTACTS,
     "skip the anon entries". */
  static bool contactAt(int nth, ContactInfo& dest) {
    if (nth < 0 || nth >= the_mesh.getNumContacts()) return false;
    return the_mesh.getContactByIdx(MAX_ANON_CONTACTS + nth, dest);
  }

  bool getSelected(ContactInfo& dest) { return contactAt(_sel, dest); }

  int render(DisplayDriver& display) override {
    int count = the_mesh.getNumContacts();
    int rows = count + 1;                  // trailing Back row
    clampWindow(rows);
    drawHeader(display, "Contacts", _sel, rows);

    ContactInfo c;
    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      if (idx >= rows) break;
      if (idx == count) {                  // Back row
        drawRow(display, row, "< Back", idx == _sel);
        continue;
      }
      if (!contactAt(idx, c)) continue;

      /* Show only whether a route is known, not a hop count.
         out_path_len read from this slot does not agree with what the companion
         protocol reports for the same contact (screen showed 64 where the protocol
         said 0), so the numeric value is not trustworthy enough to display. The
         OUT_PATH_UNKNOWN sentinel is still meaningful: it is the difference between
         "I have a path to this node" and "I will have to flood". */
      char line[64];
      if (c.out_path_len == OUT_PATH_UNKNOWN) {
        sprintf(line, "%.18s  ?", c.name);     // no known route -- will flood
      } else {
        sprintf(line, "%.20s", c.name);
      }
      drawRow(display, row, line, idx == _sel);
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp, needs UITask
};

/* ------------------------------------------------------------- message log ---- */

/* One message per screen. A 128x64 display cannot show a useful preview of several
   messages at once -- truncating to ~14 characters told you nothing, and long text
   overflowed into the row below. So each message gets the whole screen and you page
   between them.

   All four axes earn their keep here:
     LEFT/RIGHT  which message      UP/DOWN  which action      ENTER  run it
*/
class CJMsgLogScreen : public UIScreen {
  UITask* _task;
  MsgLog* _log;
  int _sel;       // which message (0 = newest)
  int _action;    // 0 = Delete, 1 = Back

  static const int ACTION_COUNT = 2;

  static const char* actionName(int a) {
    return (a == 0) ? "Delete" : "Back";
  }

  void clampSel() {
    int n = _log->count();
    if (n <= 0) { _sel = 0; return; }
    if (_sel < 0) _sel = n - 1;
    if (_sel >= n) _sel = 0;
  }

public:
  CJMsgLogScreen(UITask* task, MsgLog* log) : _task(task), _log(log), _sel(0), _action(0) { }

  void reset() { _sel = 0; _action = 0; }

  int render(DisplayDriver& display) override {
    int count = _log->count();
    clampSel();

    char tmp[48];
    display.setTextSize(1);

    if (count == 0) {
      display.setCursor(0, 0);
      display.setColor(UIColor::corp_blue);
      display.print("Messages");
      display.drawRect(0, 11, display.width(), 1);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 30, "none received");
      display.drawTextCentered(display.width() / 2, 50, "press to exit");
      return 2000;
    }

    const LoggedMsg* m = _log->get(_sel);
    if (m == NULL) return 500;

    // Header: who it is from, and position in the stack.
    if (m->is_channel) {
      sprintf(tmp, "#%.16s", m->from);
    } else if (m->path_len == 0xFF) {
      sprintf(tmp, "%.16s (D)", m->from);
    } else {
      sprintf(tmp, "%.14s (%d)", m->from, (int) m->path_len);
    }
    char hdr[48];
    display.translateUTF8ToBlocks(hdr, tmp, sizeof(hdr));
    display.setCursor(0, 0);
    display.setColor(UIColor::corp_blue);
    display.drawTextEllipsized(0, 0, display.width() - 32, hdr);

    sprintf(tmp, "<%d/%d>", _sel + 1, count);
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);
    display.drawRect(0, 11, display.width(), 1);

    // Body. Bounded so it cannot run into the action line at the bottom: roughly
    // four lines of ~21 characters between y=14 and the footer rule.
    char body[MSGLOG_BODY_CHARS + 2];
    StrHelper::strncpy(body, m->text, sizeof(body));
    char filtered[sizeof(body)];
    display.translateUTF8ToBlocks(filtered, body, sizeof(filtered));

    display.setCursor(0, 14);
    display.setColor(UIColor::primary_txt);
    display.printWordWrap(filtered, display.width());

    // Action selector, always at the bottom.
    display.drawRect(0, display.height() - 12, display.width(), 1);
    display.setColor(UIColor::warning_txt);
    display.drawTextEllipsized(0, display.height() - 9, display.width() - 2, actionName(_action));
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};

/* ---------------------------------------------------------------- channels ---- */

/* MeshCore has two messaging axes: direct messages to contacts, and posts to group
   channels. The phone app exposes both, so the device UI has to as well. Channels are
   stored in a fixed array with no public count, so they are enumerated by probing
   indices until getChannel() fails. */

class CJChannelsScreen : public CJListScreen {
  UITask* _task;

public:
  CJChannelsScreen(UITask* task) : _task(task) { }

  static int channelCount() {
    ChannelDetails cd;
    int n = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (the_mesh.getChannel(i, cd) && cd.name[0] != 0) n++;
    }
    return n;
  }

  /* Maps a visible row back to a channel slot, skipping empty ones. */
  static bool channelAt(int nth, ChannelDetails& dest) {
    ChannelDetails cd;
    int n = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (!the_mesh.getChannel(i, cd) || cd.name[0] == 0) continue;
      if (n == nth) { dest = cd; return true; }
      n++;
    }
    return false;
  }

  bool getSelected(ChannelDetails& dest) { return channelAt(_sel, dest); }

  /* One channel per view, paged with LEFT/RIGHT -- deliberately the same shape as the
     home screen's pages rather than a scrolling list. There are only ever a handful of
     channels, so paging costs nothing and the navigation reads identically to the rest
     of the UI. Contacts and the message log stay as lists, where scanning several rows
     at once is worth more. */
  int render(DisplayDriver& display) override {
    int count = channelCount();
    int pages = count + 1;                 // + Back page
    clampWindow(pages);

    char tmp[40];
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.setColor(UIColor::corp_blue);
    display.print("Channels");

    if (count > 0) {
      sprintf(tmp, "<%d/%d>", _sel + 1, pages);
    } else {
      sprintf(tmp, "<0>");
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);
    display.drawRect(0, 11, display.width(), 1);

    // Page dots, matching the home screen's indicator.
    int y = 16;
    int x = display.width() / 2 - 5 * (pages - 1);
    for (int i = 0; i < pages; i++, x += 10) {
      if (i == _sel) {
        display.fillRect(x - 1, y - 1, 4, 4);
      } else {
        display.fillRect(x, y, 2, 2);
      }
    }

    if (count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 34, "none configured");
      return 2000;
    }

    if (_sel >= count) {                   // Back page
      display.setColor(UIColor::primary_txt);
      display.setTextSize(2);
      display.drawTextCentered(display.width() / 2, 30, "Back");
      display.setTextSize(1);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 52, "press to exit");
      return 1000;
    }

    ChannelDetails cd;
    if (channelAt(_sel, cd)) {
      char filtered[sizeof(cd.name) + 2];
      sprintf(tmp, "#%.30s", cd.name);
      display.translateUTF8ToBlocks(filtered, tmp, sizeof(filtered));

      display.setColor(UIColor::primary_txt);
      display.setTextSize(2);
      display.drawTextCentered(display.width() / 2, 30, filtered);

      display.setTextSize(1);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 52, "press to post");
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};

/* ------------------------------------------------------------ send message ---- */

class CJSendScreen : public CJListScreen {
public:
  enum Target { NONE, CONTACT, CHANNEL };

private:
  UITask* _task;
  CannedStore* _canned;
  ContactInfo _recipient;
  ChannelDetails _channel;
  Target _target;

public:
  CJSendScreen(UITask* task, CannedStore* canned)
    : _task(task), _canned(canned), _target(NONE) { }

  void setRecipient(const ContactInfo& c) {
    _recipient = c;
    _target = CONTACT;
    _sel = 0;
    _top = 0;
  }

  void setChannel(const ChannelDetails& ch) {
    _channel = ch;
    _target = CHANNEL;
    _sel = 0;
    _top = 0;
  }

  Target target() const { return _target; }

  const char* recipientName() const {
    if (_target == CONTACT) return _recipient.name;
    if (_target == CHANNEL) return _channel.name;
    return "?";
  }

  /* Returns MSG_SEND_FAILED / MSG_SEND_SENT_FLOOD / MSG_SEND_SENT_DIRECT.
     Group posts have no per-recipient path, so a successful channel send is
     reported as FLOOD -- which is what it actually is on the air.
     Split out from sendSelected() so the keyboard can send typed text down the same
     path rather than duplicating the contact/channel branch. */
  int sendText(const char* text) {
    if (text == NULL || *text == 0) return MSG_SEND_FAILED;

    if (_target == CONTACT) {
      uint32_t expected_ack = 0, est_timeout = 0;
      return the_mesh.sendMessage(_recipient, the_mesh.getRTCClock()->getCurrentTime(),
                                  0, text, expected_ack, est_timeout);
    }
    if (_target == CHANNEL) {
      const char* self = the_mesh.getNodePrefs()->node_name;
      bool ok = the_mesh.sendGroupMessage(the_mesh.getRTCClock()->getCurrentTime(),
                                          _channel.channel, self, text, strlen(text));
      return ok ? MSG_SEND_SENT_FLOOD : MSG_SEND_FAILED;
    }
    return MSG_SEND_FAILED;
  }

  int sendSelected() { return sendText(_canned->get(_sel)); }

  int render(DisplayDriver& display) override {
    int count = _canned->count();
    int rows = count + 2;                  // + "Type custom" + Back
    clampWindow(rows);

    char hdr[40];
    sprintf(hdr, _target == CHANNEL ? "To #%.17s" : "To %.18s", recipientName());
    drawHeader(display, hdr, _sel, rows);

    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      if (idx >= rows) break;
      if (idx == count) {
        drawRow(display, row, "Type custom...", idx == _sel);
        continue;
      }
      if (idx == count + 1) {
        drawRow(display, row, "< Back", idx == _sel);
        continue;
      }
      drawRow(display, row, _canned->get(idx), idx == _sel);
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};

/* ---------------------------------------------------------------- keyboard ---- */

/* On-screen keyboard for when the canned list is not enough.

   A 128x64 display and five buttons is a hostile place to type, so the layout is a
   fixed grid rather than anything modal: UP/DOWN pick the row, LEFT/RIGHT the column,
   ENTER commits the key. Uppercase only -- a shift state would double the keypresses
   for something that is mostly short radio traffic.

   The bottom row carries the verbs, including SAVE, which is why the canned list can
   grow from here: type it once, save it, pick it from the list forever after. */

#define KB_ROWS  4

class CJKeyboardScreen : public UIScreen {
  UITask* _task;
  CJSendScreen* _send;
  CannedStore* _canned;

  char _text[CANNED_MAX_LEN];
  int  _len;
  int  _row, _col;

  static const char* rowChars(int r) {
    switch (r) {
      case 0:  return "ABCDEFGHIJKLM";
      case 1:  return "NOPQRSTUVWXYZ";
      case 2:  return "0123456789.,?";
      default: return "";   // row 3 is the verb row
    }
  }

  static const char* verbName(int i) {
    switch (i) {
      case 0:  return "SP";
      case 1:  return "DEL";
      case 2:  return "SAVE";
      case 3:  return "SEND";
      default: return "EXIT";
    }
  }
  static const int VERB_COUNT = 5;

  static int rowLen(int r) {
    return (r == KB_ROWS - 1) ? VERB_COUNT : (int) strlen(rowChars(r));
  }

public:
  CJKeyboardScreen(UITask* task, CJSendScreen* send, CannedStore* canned)
    : _task(task), _send(send), _canned(canned), _len(0), _row(0), _col(0) { _text[0] = 0; }

  void reset() { _len = 0; _text[0] = 0; _row = 0; _col = 0; }
  const char* text() const { return _text; }

  void append(char c) {
    if (_len < (int) sizeof(_text) - 1) { _text[_len++] = c; _text[_len] = 0; }
  }
  void backspace() {
    if (_len > 0) { _text[--_len] = 0; }
  }

  void clampCursor() {
    if (_row < 0) _row = KB_ROWS - 1;
    if (_row >= KB_ROWS) _row = 0;
    int n = rowLen(_row);
    if (n <= 0) n = 1;
    if (_col < 0) _col = n - 1;
    if (_col >= n) _col = 0;
  }

  int render(DisplayDriver& display) override {
    clampCursor();
    char tmp[40];
    display.setTextSize(1);

    // Header: who it is going to, and how many characters so far.
    sprintf(tmp, "%.14s", _send->recipientName());
    display.setColor(UIColor::corp_blue);
    display.drawTextEllipsized(0, 0, display.width() - 30, tmp);
    sprintf(tmp, "[%d]", _len);
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);
    display.drawRect(0, 10, display.width(), 1);

    // What has been typed so far, ellipsized to one line so it can never run into
    // the grid below.
    display.setColor(UIColor::primary_txt);
    display.drawTextEllipsized(0, 13, display.width() - 2, _len ? _text : "_");
    display.drawRect(0, 23, display.width(), 1);

    // The grid. Columns are evenly spaced across the full width.
    for (int r = 0; r < KB_ROWS; r++) {
      int y = 27 + r * 10;
      int n = rowLen(r);
      int step = display.width() / (n > 0 ? n : 1);

      for (int col = 0; col < n; col++) {
        bool sel = (r == _row && col == _col);
        char label[6];
        if (r == KB_ROWS - 1) {
          StrHelper::strncpy(label, verbName(col), sizeof(label));
        } else {
          label[0] = rowChars(r)[col];
          label[1] = 0;
        }
        int x = col * step + 1;
        display.setColor(sel ? UIColor::warning_txt : UIColor::secondary_txt);
        if (sel) {
          display.drawRect(x - 1, y - 1, display.getTextWidth(label) + 3, 10);
        }
        display.setCursor(x, y);
        display.print(label);
      }
    }
    return 500;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};

