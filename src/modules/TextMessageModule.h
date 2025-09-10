#pragma once
#include "Observer.h"
#include "SinglePortModule.h"
#include <string>

/**
 * TextMessageModule
 * Maneja la recepción de mensajes de texto:
 *  - Mantiene el último mensaje recibido para la pantalla
 *  - Notifica a observadores (pantalla, etc.)
 *  - Inserta el mensaje en el ChatHistoryStore (por Nodo o por Canal)
 *  - Registra también los mensajes salientes en el ChatHistoryStore
 */
class TextMessageModule : public SinglePortModule, public Observable<const meshtastic_MeshPacket *>
{
  public:
    /** Constructor
     * name es para logging
     */
    TextMessageModule() : SinglePortModule("text", meshtastic_PortNum_TEXT_MESSAGE_APP) {}

    /** Envía un mensaje de texto y lo guarda en el historial local */
    bool sendText(uint32_t to, uint8_t channel, const std::string &text);

  protected:
    /** Maneja un mensaje entrante
     * @return ProcessMessage::CONTINUE para permitir que otros módulos también lo procesen
     */
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
};

extern TextMessageModule *textMessageModule;
