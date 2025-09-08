#pragma once
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <stdint.h>

namespace chat {

/**
 * Entrada de historial de chat.
 * - Para DM: isChannel=false, 'node' es el peer y 'channel' es 0.
 * - Para Canal: isChannel=true, 'channel' es el índice del canal y 'node' es el remitente (0 si no disponible).
 */
struct ChatEntry {
  uint32_t ts{0};           // epoch segundos
  bool     outgoing{false}; // true si lo enviaste desde este nodo
  bool     isChannel{false}; // true=canal, false=DM por nodo
  uint32_t node{0};         // DM: peer; Canal: nodeId del remitente (0 si no disponible)
  uint8_t  channel{0};      // válido si isChannel==true
  std::string text;         // UTF-8 renderizable en OLED
};

class ChatHistoryStore {
public:
  static ChatHistoryStore& instance();

  // Añadir mensajes
  void addDM(uint32_t peer, bool outgoing, const std::string& text, uint32_t ts);
  void addCHAN(uint8_t channel, uint32_t fromNode, bool outgoing, const std::string& text, uint32_t ts);

  // Acceso de sólo lectura al historial (devuelven un deque estable; vacío si no existe)
  const std::deque<ChatEntry>& getDM(uint32_t peer) const;
  const std::deque<ChatEntry>& getCHAN(uint8_t channel) const;

  // Gestión
  void clearDM(uint32_t peer);
  void clearCHAN(uint8_t channel);
  void removeByNode(uint32_t peer);     // borra toda la conversación DM con ese peer
  void removeChannel(uint8_t channel);  // borra todo el historial del canal

  // Listados
  std::vector<uint32_t> listDMPeers() const;
  std::vector<uint8_t>  listChannels() const;

  // Límite por conversación/canal
  static constexpr size_t kMaxPerGroup = 15;

private:
  ChatHistoryStore() = default;
  static void pushBounded(std::deque<ChatEntry>& q, ChatEntry e);

  std::map<uint32_t, std::deque<ChatEntry>> dm_;
  std::map<uint8_t , std::deque<ChatEntry>> ch_;
};

} // namespace chat
