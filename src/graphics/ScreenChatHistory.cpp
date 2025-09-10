#include "graphics/ScreenChatHistory.h"
#include "modules/ChatHistoryStore.h"  // <-- NECESARIO
#include <cstdio>
#include <algorithm>
#include <string>

using chat::ChatHistoryStore;

namespace graphics {
namespace chatui {

int ScreenChatHistory::visibleLines() {
  int lh = DisplayIface::lineHeight();
  if (lh <= 0) lh = 10;
  // deja 1 línea para cabecera
  int vis = (DisplayIface::height() - lh) / lh;
  if (vis < 1) vis = 1;
  return vis;
}

void ScreenChatHistory::enterPicker(Mode m) {
  picker_.mode = m;
  picker_.cursor = picker_.first = 0;
  picker_.peers.clear();
  picker_.chans.clear();
  if (m == Mode::ByNode)
    picker_.peers = ChatHistoryStore::instance().listDMPeers();
  else
    picker_.chans = ChatHistoryStore::instance().listChannels();
}

void ScreenChatHistory::clampList(int total, int &cursor, int &first, int vis) {
  if (total <= 0) { cursor = first = 0; return; }
  if (cursor < 0) cursor = 0;
  if (cursor >= total) cursor = total - 1;
  if (first > cursor) first = cursor;
  if (cursor >= first + vis) first = cursor - vis + 1;
  if (first < 0) first = 0;
}

std::string ScreenChatHistory::peerName(uint32_t nodeId) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "Node %08X", (unsigned)nodeId);
  return std::string(buf);
}

std::string ScreenChatHistory::chanName(uint8_t ch) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "Channel %u", (unsigned)ch);
  return std::string(buf);
}

void ScreenChatHistory::renderPicker() {
  DisplayIface::clear();
  const int vis = visibleLines();

  DisplayIface::drawText(0, 0,
    picker_.mode == Mode::ByNode ? "Chat history: Nodes" : "Chat history: Channels",
    true);

  int y = DisplayIface::lineHeight();
  if (picker_.mode == Mode::ByNode) {
    int total = (int)picker_.peers.size();
    clampList(total, picker_.cursor, picker_.first, vis);
    for (int i=0; i<vis && (picker_.first+i)<total; ++i) {
      auto id = picker_.peers[picker_.first + i];
      auto line = peerName(id);
      DisplayIface::drawText(0, y + i*DisplayIface::lineHeight(), line.c_str(),
                             (picker_.first+i)==picker_.cursor);
    }
  } else {
    int total = (int)picker_.chans.size();
    clampList(total, picker_.cursor, picker_.first, vis);
    for (int i=0; i<vis && (picker_.first+i)<total; ++i) {
      auto ch = picker_.chans[picker_.first + i];
      auto line = chanName(ch);
      DisplayIface::drawText(0, y + i*DisplayIface::lineHeight(), line.c_str(),
                             (picker_.first+i)==picker_.cursor);
    }
  }
}

void ScreenChatHistory::handlePickerUp()   { picker_.cursor--; }
void ScreenChatHistory::handlePickerDown() { picker_.cursor++; }

bool ScreenChatHistory::handlePickerSelect() {
  if (picker_.mode == Mode::ByNode) {
    if (picker_.peers.empty()) return false;
    detail_.isChannel = false;
    detail_.node = picker_.peers[picker_.cursor];
  } else {
    if (picker_.chans.empty()) return false;
    detail_.isChannel = true;
    detail_.channel = picker_.chans[picker_.cursor];
  }
  detail_.cursor = detail_.first = 0;
  return true;
}

void ScreenChatHistory::renderDetail() {
  DisplayIface::clear();
  auto& store = ChatHistoryStore::instance();
  const auto& q = detail_.isChannel ? store.getCHAN(detail_.channel) : store.getDM(detail_.node);

  const int vis = visibleLines();
  auto title = detail_.isChannel ? ("Chan " + chanName(detail_.channel)) : peerName(detail_.node);
  DisplayIface::drawText(0, 0, title.c_str(), true);

  int total = (int)q.size();
  clampList(total, detail_.cursor, detail_.first, vis);
  int y = DisplayIface::lineHeight();

  for (int i=0; i<vis && (detail_.first+i)<total; ++i) {
    const auto& e = q[detail_.first + i];
    char prefix[4];
    std::snprintf(prefix, sizeof(prefix), "%s ", e.outgoing ? ">" : "<");
    std::string line = std::string(prefix) + e.text;
    DisplayIface::drawText(0, y + i*DisplayIface::lineHeight(), line.c_str(),
                           (detail_.first+i)==detail_.cursor);
  }
}

void ScreenChatHistory::handleDetailUp()   { detail_.cursor--; }
void ScreenChatHistory::handleDetailDown() { detail_.cursor++; }

} // namespace chatui
} // namespace graphics
