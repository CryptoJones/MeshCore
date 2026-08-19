#pragma once

#include <Arduino.h>
#include <MeshCore.h>
#include <helpers/TxtDataHelpers.h>
#include "../DataStore.h"

/* Canned (pre-written) messages, so sending from the device never requires typing
   on a joystick.

   Stored one-per-line in a plain text file on the device filesystem. The file is the
   source of truth; the compiled defaults below only seed it the first time, so the
   list can be changed later without a reflash. Entries can also be appended at
   runtime — the message log screen can promote a received message to a canned reply,
   which is how the list grows without a keyboard.
*/

#ifndef CANNED_FILE
  #define CANNED_FILE "/cj_canned.txt"
#endif

#define CANNED_MAX_ENTRIES   16
#define CANNED_MAX_LEN       64

class CannedStore {
  char _msgs[CANNED_MAX_ENTRIES][CANNED_MAX_LEN];
  int  _count;
  DataStore* _store;

  static const char* defaults(int i) {
    static const char* DEFAULTS[] = {
      "Yes",
      "No",
      "OK",
      "On my way",
      "Standing by",
      "Message received",
      "Need assistance",
      "All clear",
    };
    return (i >= 0 && i < 8) ? DEFAULTS[i] : NULL;
  }

public:
  CannedStore() : _count(0), _store(NULL) { }

  int count() const { return _count; }

  const char* get(int i) const {
    return (i >= 0 && i < _count) ? _msgs[i] : "";
  }

  void begin(DataStore* store) {
    _store = store;
    _count = 0;

    if (!load() || _count == 0) {
      loadDefaults();
      save();   // seed the file so it can be edited later without a reflash
    }
  }

  void loadDefaults() {
    _count = 0;
    for (int i = 0; defaults(i) != NULL && _count < CANNED_MAX_ENTRIES; i++) {
      StrHelper::strncpy(_msgs[_count], defaults(i), CANNED_MAX_LEN);
      _count++;
    }
  }

  bool load() {
    if (_store == NULL) return false;
    FILESYSTEM* fs = _store->getPrimaryFS();
    if (fs == NULL) return false;

  #if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    File f = fs->open(CANNED_FILE, FILE_O_READ);
  #else
    File f = fs->open(CANNED_FILE, "r");
  #endif
    if (!f) return false;

    _count = 0;
    char line[CANNED_MAX_LEN];
    int len = 0;
    while (f.available() && _count < CANNED_MAX_ENTRIES) {
      char c = f.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) {
          line[len] = 0;
          StrHelper::strncpy(_msgs[_count], line, CANNED_MAX_LEN);
          _count++;
          len = 0;
        }
      } else if (len < CANNED_MAX_LEN - 1) {
        line[len++] = c;
      }
      // Over-long lines are truncated rather than split, so one bad line cannot
      // cascade into a screenful of fragments.
    }
    if (len > 0 && _count < CANNED_MAX_ENTRIES) {
      line[len] = 0;
      StrHelper::strncpy(_msgs[_count], line, CANNED_MAX_LEN);
      _count++;
    }
    f.close();
    return true;
  }

  bool save() {
    if (_store == NULL) return false;
    FILESYSTEM* fs = _store->getPrimaryFS();
    if (fs == NULL) return false;

  #if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    fs->remove(CANNED_FILE);
    File f = fs->open(CANNED_FILE, FILE_O_WRITE);
  #else
    File f = fs->open(CANNED_FILE, "w", true);
  #endif
    if (!f) return false;

    for (int i = 0; i < _count; i++) {
      f.write((const uint8_t *) _msgs[i], strlen(_msgs[i]));
      f.write((const uint8_t *) "\n", 1);
    }
    f.close();
    return true;
  }

  /* Append a message to the list and persist it. Returns false when full or when the
     text is already present — re-adding the same quick reply is the common misfire. */
  bool add(const char* text) {
    if (text == NULL || *text == 0) return false;
    if (_count >= CANNED_MAX_ENTRIES) return false;
    for (int i = 0; i < _count; i++) {
      if (strncmp(_msgs[i], text, CANNED_MAX_LEN - 1) == 0) return false;
    }
    StrHelper::strncpy(_msgs[_count], text, CANNED_MAX_LEN);
    _count++;
    return save();
  }

  bool remove(int idx) {
    if (idx < 0 || idx >= _count) return false;
    for (int i = idx; i < _count - 1; i++) {
      StrHelper::strncpy(_msgs[i], _msgs[i + 1], CANNED_MAX_LEN);
    }
    _count--;
    return save();
  }
};
