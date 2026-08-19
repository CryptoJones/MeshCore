#pragma once

#include <Arduino.h>
#include <MeshCore.h>
#include <helpers/TxtDataHelpers.h>

/* A small ring buffer of recently received messages, for on-device reading.

   IMPORTANT: this deliberately does NOT consume from the companion offline queue.
   That queue belongs to the connected client (phone app or meshcore-cli), and
   draining it here would make messages vanish from the app. Instead this captures a
   copy as messages arrive via UITask::newMsg(), so reading on the device and reading
   in the app are independent.

   Consequence: entries here are lost on reboot, and only cover what arrived while
   this firmware was running. That is the right trade for a status/triage view — the
   client remains the system of record.
*/

#define MSGLOG_CAPACITY   12
#define MSGLOG_NAME_LEN   32
#define MSGLOG_TEXT_LEN   96

struct LoggedMsg {
  char from[MSGLOG_NAME_LEN];
  char text[MSGLOG_TEXT_LEN];
  uint32_t recv_millis;
  uint8_t path_len;   // 0xFF = direct/flood unknown, else hop count
  bool is_channel;    // true when `from` names a channel rather than a contact
};

class MsgLog {
  LoggedMsg _msgs[MSGLOG_CAPACITY];
  int _count;     // how many slots are populated (saturates at capacity)
  int _head;      // index of the next slot to write

public:
  MsgLog() : _count(0), _head(0) { }

  int count() const { return _count; }
  bool isEmpty() const { return _count == 0; }

  /* Index 0 is the MOST RECENT message. Callers iterate newest-first because that is
     the order the list screen displays. */
  const LoggedMsg* get(int i) const {
    if (i < 0 || i >= _count) return NULL;
    int slot = (_head - 1 - i + MSGLOG_CAPACITY * 2) % MSGLOG_CAPACITY;
    return &_msgs[slot];
  }

  void add(uint8_t path_len, const char* from_name, const char* text, bool is_channel = false) {
    LoggedMsg* m = &_msgs[_head];
    m->is_channel = is_channel;
    StrHelper::strncpy(m->from, from_name == NULL ? "?" : from_name, MSGLOG_NAME_LEN);
    StrHelper::strncpy(m->text, text == NULL ? "" : text, MSGLOG_TEXT_LEN);
    m->recv_millis = millis();
    m->path_len = path_len;

    _head = (_head + 1) % MSGLOG_CAPACITY;
    if (_count < MSGLOG_CAPACITY) _count++;
  }

  void clear() { _count = 0; _head = 0; }
};
