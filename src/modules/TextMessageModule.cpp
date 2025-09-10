#include "TextMessageModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "buzz.h"
#include "configuration.h"
#include "graphics/Screen.h"

// Historial de chat
#include "modules/ChatHistoryStore.h"

#include <string>

TextMessageModule *textMessageModule;

ProcessMessage TextMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    LOG_INFO("Received text msg from=0x%08x, id=0x%08x, ch=%u, to=0x%08x, msg=%.*s",
             mp.from, mp.id, mp.channel, mp.to, p.payload.size, p.payload.bytes);
#endif

    // Guardar último mensaje para pantalla
    devicestate.rx_text_message = mp;
    devicestate.has_rx_text_message = true;

    if (shouldWakeOnReceivedMessage()) {
        powerFSM.trigger(EVENT_RECEIVED_MSG);
    }

    // Notificar observadores
    notifyObservers(&mp);

    // Extraer texto
    std::string text;
    if (mp.decoded.payload.size > 0 && mp.decoded.payload.bytes) {
        text.assign(reinterpret_cast<const char *>(mp.decoded.payload.bytes),
                    static_cast<size_t>(mp.decoded.payload.size));
    } else {
        text.clear();
    }

    // Timestamp y canal
    const uint32_t ts = mp.rx_time;
    const uint8_t channelIndex = static_cast<uint8_t>(mp.channel);

    // DM si 'to' NO es broadcast
    const bool isDirect = !isBroadcast(mp.to);

    if (isDirect) {
        chat::ChatHistoryStore::instance().addDM(
            static_cast<uint32_t>(mp.from),
            /*outgoing=*/false,
            text,
            ts);
    } else {
        chat::ChatHistoryStore::instance().addCHAN(
            channelIndex,
            static_cast<uint32_t>(mp.from),   // remitente real
            /*outgoing=*/false,
            text,
            ts);
    }

    return ProcessMessage::CONTINUE;
}

bool TextMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

// === Nuevo: enviar texto y guardarlo en el historial ===
bool TextMessageModule::sendText(uint32_t to, uint8_t channel, const std::string &text)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) return false;

    p->to = to;
    p->channel = channel;

    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.payload.size = text.size();
    memcpy(p->decoded.payload.bytes, text.data(), text.size());

    // Enviar a la malla (con puntero global 'service')
    if (service) {
        service->sendToMesh(p);
    }

    // Guardar en historial como saliente
    if (isBroadcast(to)) {
        chat::ChatHistoryStore::instance().addCHAN(
            channel,
            nodeDB ? nodeDB->getNodeNum() : 0, // yo mismo
            /*outgoing=*/true,
            text,
            millis()/1000);
    } else {
        chat::ChatHistoryStore::instance().addDM(
            to,
            /*outgoing=*/true,
            text,
            millis()/1000);
    }

    return true;
}
