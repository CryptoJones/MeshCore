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
/* MAX_TEXT_LEN (BaseChatMesh.h) is 10*CIPHER_BLOCK_SIZE = 160, the longest direct
   message the protocol carries. Store the full 160 plus a terminator so the log never
   truncates a legal message -- the old 96 silently dropped the tail of anything longer. */
#define MSGLOG_TEXT_LEN   161

/* How much of a message body fits on screen above the action line. This is a HARDWARE
   limit, not a protocol one: 128px at ~6px per character is about 21 characters a
   line, and the body area y=14..52 holds four lines. So a 160-character message
   cannot be shown in full on this display no matter how it is stored -- the text is
   ellipsized rather than allowed to overflow into the footer, which would soft-wrap
   and collide. The full text is still in the log and still reaches the phone app. */
#define MSGLOG_BODY_CHARS 84

struct LoggedMsg {
  char from[MSGLOG_NAME_LEN];
  char text[MSGLOG_TEXT_LEN];
  uint32_t recv_millis;   // for age; survives an unset RTC
  uint32_t recv_epoch;    // RTC time of receipt; 0 when the clock is not set
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

  void add(uint8_t path_len, const char* from_name, const char* text,
           bool is_channel = false, uint32_t epoch = 0) {
    LoggedMsg* m = &_msgs[_head];
    m->is_channel = is_channel;
    m->recv_epoch = epoch;
    StrHelper::strncpy(m->from, from_name == NULL ? "?" : from_name, MSGLOG_NAME_LEN);
    StrHelper::strncpy(m->text, text == NULL ? "" : text, MSGLOG_TEXT_LEN);
    m->recv_millis = millis();
    m->path_len = path_len;

    _head = (_head + 1) % MSGLOG_CAPACITY;
    if (_count < MSGLOG_CAPACITY) _count++;
  }

  void clear() { _count = 0; _head = 0; }

  /* Remove one entry by logical (newest-first) index. The buffer is small enough that
     compacting through a temporary is simpler and safer than juggling head/tail. */
  bool remove(int i) {
    if (i < 0 || i >= _count) return false;

    LoggedMsg tmp[MSGLOG_CAPACITY];
    int n = 0;
    for (int k = 0; k < _count; k++) {
      if (k == i) continue;
      tmp[n++] = *get(k);          // newest-first
    }

    _count = 0;
    _head = 0;
    for (int k = n - 1; k >= 0; k--) {   // re-insert oldest-first
      _msgs[_head] = tmp[k];
      _head = (_head + 1) % MSGLOG_CAPACITY;
      _count++;
    }
    return true;
  }
};
