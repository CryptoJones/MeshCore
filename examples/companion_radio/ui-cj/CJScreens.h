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
  int _sel;       // which message within the current view (0 = newest)
  int _action;    // Reply / Delete / [Scroll] / Back -- see actionName()
  bool _confirm;  // Delete is armed and waiting for a second press
  bool _scrolling;  // body-scroll mode: UP/DOWN move the text window
  int  _scroll;     // first character of the visible window

  /* When set, the view is restricted to one origin -- used to read a single
     channel's traffic from the Channels screen instead of the mixed log. */
  char _filter[MSGLOG_NAME_LEN];
  bool _filtered;

  /* Scroll only exists when the message does not fit, so short messages keep the
     three-action list and never make you page past an action you cannot use. */
  static const int SCROLL_STEP = 21;   // ~one rendered line at 6px on a 128px panel

  bool overflows() const {
    const LoggedMsg* m = viewGet(_sel);
    return m != NULL && (int) strlen(m->text) > MSGLOG_BODY_CHARS;
  }

  int actionCount() const { return overflows() ? 4 : 3; }

  const char* actionName(int a) const {
    bool ov = overflows();
    switch (a) {
      case 0:  return "Reply";
      case 1:  return "Delete";
      case 2:  return ov ? "Scroll" : "Back";
      default: return "Back";
    }
  }

  /* Age is computed from millis(), not the RTC, so it stays correct on a node whose
     clock was never set -- the common case on a fresh radio. The log is RAM-only and
     dies with a reboot, so millis() can never have wrapped past it. */
  static void formatAge(char* out, size_t n, const LoggedMsg* m) {
    if (m == NULL) { out[0] = 0; return; }
    uint32_t secs = (millis() - m->recv_millis) / 1000;
    if (secs < 60)          snprintf(out, n, "%lus", (unsigned long) secs);
    else if (secs < 3600)   snprintf(out, n, "%lum", (unsigned long) (secs / 60));
    else if (secs < 86400)  snprintf(out, n, "%luh", (unsigned long) (secs / 3600));
    else                    snprintf(out, n, "%lud", (unsigned long) (secs / 86400));
  }

  /* Absolute clock time, only when the RTC actually holds a real date. Anything
     before 2020 means the clock was never synced and a printed time would be a lie. */
  static bool formatClock(char* out, size_t n, const LoggedMsg* m) {
    if (m == NULL || m->recv_epoch < 1577836800UL) { out[0] = 0; return false; }
    uint32_t secs_today = m->recv_epoch % 86400UL;
    snprintf(out, n, "%02lu:%02lu",
             (unsigned long) (secs_today / 3600), (unsigned long) ((secs_today % 3600) / 60));
    return true;
  }

  bool matches(const LoggedMsg* m) const {
    if (!_filtered) return true;
    return m != NULL && strncmp(m->from, _filter, MSGLOG_NAME_LEN - 1) == 0;
  }

  /* Views count and index over MATCHING entries; logIndex() maps back to the real
     MsgLog position so delete removes the right one. */
  int viewCount() const {
    if (!_filtered) return _log->count();
    int n = 0;
    for (int i = 0; i < _log->count(); i++) {
      if (matches(_log->get(i))) n++;
    }
    return n;
  }

  int logIndex(int nth) const {
    if (!_filtered) return nth;
    int n = 0;
    for (int i = 0; i < _log->count(); i++) {
      if (!matches(_log->get(i))) continue;
      if (n == nth) return i;
      n++;
    }
    return -1;
  }

  const LoggedMsg* viewGet(int nth) const {
    int idx = logIndex(nth);
    return (idx < 0) ? NULL : _log->get(idx);
  }

  void clampSel() {
    int n = viewCount();
    if (n <= 0) { _sel = 0; return; }
    if (_sel < 0) _sel = n - 1;
    if (_sel >= n) _sel = 0;
  }

public:
  CJMsgLogScreen(UITask* task, MsgLog* log)
    : _task(task), _log(log), _sel(0), _action(0), _confirm(false),
      _scrolling(false), _scroll(0), _filtered(false) {
    _filter[0] = 0;
  }

  void reset() { _sel = 0; _action = 0; _confirm = false; _scrolling = false; _scroll = 0; }
  void disarm() { _confirm = false; }
  void resetScroll() { _scroll = 0; _scrolling = false; }

  void clearFilter() { _filtered = false; _filter[0] = 0; _sel = 0; _action = 0; }
  void setFilter(const char* origin) {
    StrHelper::strncpy(_filter, origin == NULL ? "" : origin, sizeof(_filter));
    _filtered = (_filter[0] != 0);
    _sel = 0;
    _action = 0;
  }
  bool isFiltered() const { return _filtered; }

  int render(DisplayDriver& display) override {
    int count = viewCount();
    clampSel();

    char tmp[48];
    display.setTextSize(1);

    if (count == 0) {
      display.setCursor(0, 0);
      display.setColor(UIColor::corp_blue);
      display.print(_filtered ? _filter : "Messages");
      display.drawRect(0, 11, display.width(), 1);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 30, "none received");
      display.drawTextCentered(display.width() / 2, 50, "press to exit");
      return 2000;
    }

    const LoggedMsg* m = viewGet(_sel);
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
    display.drawTextEllipsized(0, 0, display.width() - 62, hdr);

    char clock[8];
    if (formatClock(clock, sizeof(clock), m)) {
      sprintf(tmp, "%s <%d/%d>", clock, _sel + 1, count);
    } else {
      sprintf(tmp, "<%d/%d>", _sel + 1, count);
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);
    display.drawRect(0, 11, display.width(), 1);

    // Body. Bounded so it cannot run into the action line at the bottom: roughly
    // four lines of ~21 characters between y=14 and the footer rule.
    /* Visible window into the message. Both ends are marked so the reader can always
       tell whether there is more text above or below -- without that, a scrolled view
       is indistinguishable from a short message. */
    int total = (int) strlen(m->text);
    if (_scroll > total) _scroll = 0;
    char body[MSGLOG_BODY_CHARS + 8];
    body[0] = 0;
    if (_scroll > 0) strcat(body, "...");
    int room = MSGLOG_BODY_CHARS - (int) strlen(body);
    strncat(body, m->text + _scroll, room);
    if (_scroll + room < total) strcat(body, "...");
    char filtered[sizeof(body)];
    display.translateUTF8ToBlocks(filtered, body, sizeof(filtered));

    display.setCursor(0, 14);
    display.setColor(UIColor::primary_txt);
    display.printWordWrap(filtered, display.width());

    // Action selector, always at the bottom.
    display.drawRect(0, display.height() - 12, display.width(), 1);
    display.setColor(UIColor::warning_txt);
    const char* footer;
    if (_scrolling)      footer = "SCROLL up/dn  back=done";
    else if (_confirm)   footer = "Delete? press again";
    else                 footer = actionName(_action);
    // Age sits right-aligned on the action line: it costs no body space, and "how
    // long ago" is the question you actually ask of a message you are looking at.
    char age[8];
    formatAge(age, sizeof(age), m);
    int age_w = display.getTextWidth(age);
    display.drawTextEllipsized(0, display.height() - 9, display.width() - age_w - 6, footer);
    display.setCursor(display.width() - age_w - 2, display.height() - 9);
    display.print(age);
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
  int _action;   // 0 = Post, 1 = Read, 2 = Back

  static const int ACTION_COUNT = 3;
  static const char* actionName(int a) {
    switch (a) {
      case 0:  return "Post";
      case 1:  return "Read";
      default: return "Back";
    }
  }

public:
  CJChannelsScreen(UITask* task) : _task(task), _action(0) { }

  void reset() { _sel = 0; _top = 0; _action = 0; }
  int action() const { return _action; }

  static int channelCount() {
    ChannelDetails cd;
    int n = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (the_mesh.getChannel(i, cd) && cd.name[0] != 0) n++;
    }
    return n;
  }

  /* Maps a visible page back to a channel slot, skipping empty ones. */
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

  /* One channel per page, paged with LEFT/RIGHT -- deliberately the same shape as the
     home screen's pages rather than a scrolling list, since there are only ever a
     handful. UP/DOWN pick what ENTER does, matching the message reader. */
  int render(DisplayDriver& display) override {
    int count = channelCount();
    clampWindow(count > 0 ? count : 1);
    if (_action < 0) _action = ACTION_COUNT - 1;
    if (_action >= ACTION_COUNT) _action = 0;

    char tmp[40];
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.setColor(UIColor::corp_blue);
    display.print("Channels");

    if (count > 0) {
      sprintf(tmp, "<%d/%d>", _sel + 1, count);
    } else {
      sprintf(tmp, "<0>");
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);
    display.drawRect(0, 11, display.width(), 1);

    if (count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 30, "none configured");
      display.drawTextCentered(display.width() / 2, 50, "press to exit");
      return 2000;
    }

    ChannelDetails cd;
    if (channelAt(_sel, cd)) {
      char filtered[sizeof(cd.name) + 2];
      sprintf(tmp, "#%.30s", cd.name);
      display.translateUTF8ToBlocks(filtered, tmp, sizeof(filtered));
      display.setColor(UIColor::primary_txt);
      display.setTextSize(2);
      display.drawTextCentered(display.width() / 2, 26, filtered);
      display.setTextSize(1);
    }

    display.drawRect(0, display.height() - 12, display.width(), 1);
    display.setColor(UIColor::warning_txt);
    display.drawTextEllipsized(0, display.height() - 9, display.width() - 2, actionName(_action));
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
    int rows = count + 2;                  // + "Manage..." + Back
    clampWindow(rows);

    char hdr[40];
    sprintf(hdr, _target == CHANNEL ? "To #%.17s" : "To %.18s", recipientName());
    drawHeader(display, hdr, _sel, rows);

    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      if (idx >= rows) break;
      if (idx == count) {
        drawRow(display, row, "Manage...", idx == _sel);
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



/* -------------------------------------------------------------- manage menu ---- */

/* Everything that is not "pick a canned message and send it" lives one level down, so
   the compose list stays a list of messages rather than a list of messages plus
   chrome. Reached from the "Manage..." row. */
class CJManageScreen : public CJListScreen {
  UITask* _task;

  static const int ITEM_COUNT = 3;   // Type custom, Delete canned, Back

  static const char* itemName(int i) {
    switch (i) {
      case 0:  return "Type custom...";
      case 1:  return "Delete canned...";
      default: return "< Back";
    }
  }

public:
  CJManageScreen(UITask* task) : _task(task) { }

  void reset() { _sel = 0; _top = 0; }

  int render(DisplayDriver& display) override {
    clampWindow(ITEM_COUNT);
    drawHeader(display, "Manage", _sel, ITEM_COUNT);
    for (int row = 0; row < CJ_MAX_VISIBLE && row < ITEM_COUNT; row++) {
      int idx = _top + row;
      if (idx >= ITEM_COUNT) break;
      drawRow(display, row, itemName(idx), idx == _sel);
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};

/* ----------------------------------------------------------- canned manager ---- */

/* Deleting canned messages. Needed because SAVE is one keypress and a mis-saved entry
   would otherwise be permanent -- the file is the source of truth and there is no
   other way to edit it from the device.

   ENTER deletes the highlighted entry outright rather than asking to confirm: the
   list is short, the cost of a mistake is retyping one phrase, and a confirm step
   doubles the keypresses on a five-button device. */
class CJCannedMgrScreen : public CJListScreen {
  UITask* _task;
  CannedStore* _canned;
  bool _confirm;   // armed and waiting for a second press

public:
  CJCannedMgrScreen(UITask* task, CannedStore* canned)
    : _task(task), _canned(canned), _confirm(false) { }

  void reset() { _sel = 0; _top = 0; _confirm = false; }
  void disarm() { _confirm = false; }

  int render(DisplayDriver& display) override {
    int count = _canned->count();
    int rows = count + 1;                  // + Back
    clampWindow(rows);
    drawHeader(display, _confirm ? "Press again!" : "Delete canned", _sel, rows);

    if (count == 0) {
      drawEmpty(display, "list is empty");
    }

    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      if (idx >= rows) break;
      if (idx == count) {
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

#define KB_ROWS  5

class CJKeyboardScreen : public UIScreen {
  UITask* _task;
  CJSendScreen* _send;
  CannedStore* _canned;

  char _text[CANNED_MAX_LEN];
  int  _len;
  int  _row, _col;
  bool _shift;     // false = UPPER (the default for short radio traffic)

  /* Five rows in 40 vertical pixels, so the step is 8px and every row must fit the
     128px width. Letters split across two rows of 13; digits and punctuation get
     their own rows so neither is buried behind a modifier. */
  static const char* rowChars(int r) {
    switch (r) {
      case 0:  return "ABCDEFGHIJKLM";
      case 1:  return "NOPQRSTUVWXYZ";
      case 2:  return "0123456789";
      case 3:  return ".,!?$&@-'/:";
      default: return "";   // row 4 is the verb row
    }
  }

  /* No EXIT key: six labels do not fit across 128px (SAVE alone is 24px against a
     21px uniform column, so they ran together). The back button already leaves this
     screen, so the row drops to five and is laid out by measured width. */
  static const char* verbName(int i, bool shift) {
    switch (i) {
      case 0:  return shift ? "Aa" : "aA";   // shows what it will switch TO
      case 1:  return "SP";
      case 2:  return "DEL";
      case 3:  return "SAVE";
      default: return "SEND";
    }
  }
  static const int VERB_COUNT = 5;

  static int rowLen(int r) {
    return (r == KB_ROWS - 1) ? VERB_COUNT : (int) strlen(rowChars(r));
  }

  /* Shift applies to letters only. Digits and punctuation are on their own rows, so
     there is no second symbol layer to remember. */
  char keyAt(int r, int col) const {
    char ch = rowChars(r)[col];
    if (_shift && ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
    return ch;
  }

public:
  CJKeyboardScreen(UITask* task, CJSendScreen* send, CannedStore* canned)
    : _task(task), _send(send), _canned(canned), _len(0), _row(0), _col(0), _shift(false) {
    _text[0] = 0;
  }

  void reset() { _len = 0; _text[0] = 0; _row = 0; _col = 0; _shift = false; }
  const char* text() const { return _text; }
  void toggleShift() { _shift = !_shift; }

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

    sprintf(tmp, "%.13s", _send->recipientName());
    display.setColor(UIColor::corp_blue);
    display.drawTextEllipsized(0, 0, display.width() - 28, tmp);
    sprintf(tmp, "[%d]", _len);
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);

    // Typed text, ellipsized to a single line so it can never reach the grid.
    display.setColor(UIColor::primary_txt);
    display.drawTextEllipsized(0, 10, display.width() - 2, _len ? _text : "_");
    display.drawRect(0, 20, display.width(), 1);

    for (int r = 0; r < KB_ROWS; r++) {
      int y = 24 + r * 8;
      int n = rowLen(r);
      if (n <= 0) continue;

      /* Single characters are uniform width, so an even step is fine for them. The
         verb row has labels of differing widths, so it is measured and the leftover
         space shared out as equal gaps -- a uniform step overlaps the wider ones. */
      bool verbs = (r == KB_ROWS - 1);
      int gap = 0, x = 1;
      if (verbs) {
        int total = 0;
        for (int i = 0; i < n; i++) total += display.getTextWidth(verbName(i, _shift));
        gap = (display.width() - total) / (n + 1);
        if (gap < 2) gap = 2;
        x = gap;
      }
      int step = display.width() / n;

      for (int col = 0; col < n; col++) {
        bool sel = (r == _row && col == _col);
        char label[6];
        if (verbs) {
          StrHelper::strncpy(label, verbName(col, _shift), sizeof(label));
        } else {
          label[0] = keyAt(r, col);
          label[1] = 0;
        }
        int w = display.getTextWidth(label);
        int cx = verbs ? x : (col * step + 1);

        display.setColor(sel ? UIColor::warning_txt : UIColor::secondary_txt);
        if (sel) {
          display.drawRect(cx - 1, y - 1, w + 3, 9);
        }
        display.setCursor(cx, y);
        display.print(label);

        if (verbs) x += w + gap;
      }
    }
    return 400;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};

