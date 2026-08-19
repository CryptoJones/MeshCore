#pragma once

/* On-device contact browsing, message reading, and canned-message sending.

   MeshCore's stock companion UI is three screens — splash, home, message preview —
   because the design puts the interface in the phone app. These screens add the parts
   that make the hardware usable on its own.

   Reached from the home screen: UP opens contacts, DOWN opens the message log. Inside
   a list: UP/DOWN scroll, ENTER selects, CANCEL/LEFT goes back. Those keys are free on
   the home screen, which uses LEFT/RIGHT for paging and ENTER for per-page actions, so
   nothing existing is displaced.
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

  void drawHeader(DisplayDriver& display, const char* title, int count) {
    char tmp[24];
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.setColor(UIColor::corp_blue);
    display.print(title);

    sprintf(tmp, "%d", count);
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
    display.setCursor(8, y);

    char filtered[64];
    display.translateUTF8ToBlocks(filtered, text, sizeof(filtered));
    display.print(filtered);
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

  bool getSelected(ContactInfo& dest) {
    int n = the_mesh.getNumContacts();
    if (n <= 0 || _sel < 0 || _sel >= n) return false;
    return the_mesh.getContactByIdx(_sel, dest);
  }

  int render(DisplayDriver& display) override {
    int count = the_mesh.getNumContacts();
    clampWindow(count);
    drawHeader(display, "Contacts", count);

    if (count == 0) {
      drawEmpty(display, "none yet");
      return 2000;
    }

    ContactInfo c;
    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      if (idx >= count) break;
      if (!the_mesh.getContactByIdx(idx, c)) continue;

      // Path length tells you whether a direct route is known -- the single most
      // useful thing to see next to a name when deciding who to message.
      char line[64];
      if (c.out_path_len == 0xFF) {
        sprintf(line, "%.20s", c.name);
      } else {
        sprintf(line, "%.16s %dh", c.name, (int) c.out_path_len);
      }
      drawRow(display, row, line, idx == _sel);
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp, needs UITask
};

/* ------------------------------------------------------------- message log ---- */

class CJMsgLogScreen : public CJListScreen {
  UITask* _task;
  MsgLog* _log;
  bool _detail;      // false = list, true = full text of selected message

public:
  CJMsgLogScreen(UITask* task, MsgLog* log) : _task(task), _log(log), _detail(false) { }

  void reset() { _sel = 0; _top = 0; _detail = false; }
  const LoggedMsg* selected() const { return _log->get(_sel); }

  int render(DisplayDriver& display) override {
    int count = _log->count();
    clampWindow(count);

    if (count == 0) {
      drawHeader(display, "Messages", 0);
      drawEmpty(display, "none received");
      return 2000;
    }

    if (_detail) {
      const LoggedMsg* m = _log->get(_sel);
      if (m == NULL) { _detail = false; return 100; }

      char hdr[40];
      if (m->path_len == 0xFF) {
        sprintf(hdr, "(D) %.24s", m->from);
      } else {
        sprintf(hdr, "(%d) %.24s", (int) m->path_len, m->from);
      }
      drawHeader(display, hdr, 0);

      display.setCursor(0, CJ_LIST_TOP);
      display.setColor(UIColor::primary_txt);
      char filtered[MSGLOG_TEXT_LEN];
      display.translateUTF8ToBlocks(filtered, m->text, sizeof(filtered));
      display.printWordWrap(filtered, display.width());
      return 1000;
    }

    drawHeader(display, "Messages", count);
    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      const LoggedMsg* m = _log->get(idx);
      if (m == NULL) break;

      char line[64];
      sprintf(line, "%.10s: %.14s", m->from, m->text);
      drawRow(display, row, line, idx == _sel);
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
  bool inDetail() const { return _detail; }
  void setDetail(bool d) { _detail = d; }
};

/* ------------------------------------------------------------ send message ---- */

class CJSendScreen : public CJListScreen {
  UITask* _task;
  CannedStore* _canned;
  ContactInfo _recipient;
  bool _have_recipient;

public:
  CJSendScreen(UITask* task, CannedStore* canned)
    : _task(task), _canned(canned), _have_recipient(false) { }

  void setRecipient(const ContactInfo& c) {
    _recipient = c;
    _have_recipient = true;
    _sel = 0;
    _top = 0;
  }

  const char* recipientName() const { return _have_recipient ? _recipient.name : "?"; }

  /* Returns one of MSG_SEND_FAILED / MSG_SEND_SENT_FLOOD / MSG_SEND_SENT_DIRECT. */
  int sendSelected() {
    if (!_have_recipient) return MSG_SEND_FAILED;
    const char* text = _canned->get(_sel);
    if (text == NULL || *text == 0) return MSG_SEND_FAILED;

    uint32_t expected_ack = 0, est_timeout = 0;
    return the_mesh.sendMessage(_recipient, the_mesh.getRTCClock()->getCurrentTime(),
                                0, text, expected_ack, est_timeout);
  }

  int render(DisplayDriver& display) override {
    int count = _canned->count();
    clampWindow(count);

    char hdr[40];
    sprintf(hdr, "To %.18s", recipientName());
    drawHeader(display, hdr, count);

    if (count == 0) {
      drawEmpty(display, "no canned msgs");
      return 2000;
    }

    for (int row = 0; row < CJ_MAX_VISIBLE; row++) {
      int idx = _top + row;
      if (idx >= count) break;
      drawRow(display, row, _canned->get(idx), idx == _sel);
    }
    return 1000;
  }

  bool handleInput(char c) override;   // defined in UITask.cpp
};
