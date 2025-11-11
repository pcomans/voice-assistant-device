# WebSocket vs WebRTC for Continuous Conversation

**Decision Context**: Choosing protocol for device ↔ proxy bidirectional audio streaming

---

## Quick Answer

**Recommendation: WebSocket**
- Simpler, proven, sufficient for our use case
- Official ESP-IDF support built-in
- OpenAI Realtime API supports both WebSocket and WebRTC equally

**WebRTC only needed if**: Direct device ↔ OpenAI (no proxy) OR P2P device-to-device

---

## Technical Comparison

### WebSocket

**What it is**: Bidirectional TCP-based protocol over HTTP

**ESP32 Support**:
- ✅ Built into ESP-IDF (v5.5.1 has `esp_websocket_client` component)
- ✅ Official examples: `esp-idf/examples/protocols/http_server/ws_echo_server`
- ✅ For v5.x: Add via `idf.py add-dependency "espressif/esp_websocket_client"`
- ✅ Mature, well-documented, battle-tested

**Architecture**:
```
Device (ws://proxy:8000/ws) ←→ Proxy (wss://api.openai.com/v1/realtime)
       WebSocket (TCP)              WebSocket (TLS)
```

**Pros**:
- Simple implementation (~200 lines device code)
- Reliable delivery (TCP)
- Works through firewalls/NAT (HTTP upgrade)
- TLS encryption standard
- Event-driven API (callbacks for data/connect/disconnect)
- Low overhead for our use case

**Cons**:
- Slightly higher latency than WebRTC (TCP overhead)
- Not optimized for packet loss (TCP retransmits)

**Latency**: ~200-300ms on LAN (TCP + processing)

**Complexity**: Low (3/10)

---

### WebRTC

**What it is**: P2P real-time communication protocol, typically UDP-based

**ESP32 Support**:
- ⚠️ **Not built into ESP-IDF** - requires external library
- Available libraries:
  - **libpeer**: Open-source C library for IoT ([github.com/sepfy/libpeer](https://github.com/sepfy/libpeer))
  - **esp-webrtc-solution**: Espressif official demo ([github.com/espressif/esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution))
  - **OpenAI ESP32 demo** (Jan 2025): Enhanced esp_peer implementation
- ⚠️ Much more complex than WebSocket

**Architecture**:
```
Device (WebRTC/UDP) ←STUN/TURN/ICE→ Proxy ←→ OpenAI
         P2P with signaling          WebRTC Data Channel
```

**Pros**:
- Lower latency (UDP, no retransmits)
- Better for packet loss (forward error correction)
- Native audio/video streaming
- Built for real-time media

**Cons**:
- Complex setup (STUN/TURN servers, ICE negotiation, SDP exchange)
- Requires signaling server (WebSocket or HTTP for setup)
- More code (~1000+ lines for full implementation)
- Less mature on ESP32
- Overkill for local network (device and proxy on same LAN)

**Latency**: ~100-200ms on LAN (UDP + processing)

**Complexity**: High (8/10)

---

## Use Case Analysis

### Our Scenario

**Network**: Device and proxy on **same LAN** (WiFi)
- Low latency already (~1-5ms ping)
- No packet loss (reliable WiFi)
- No NAT traversal needed (same network)
- No firewall issues (both controlled by user)

**Requirements**:
- Bidirectional audio streaming
- Handle interruption signals
- Session management
- Reliable delivery (can't lose audio chunks)

**OpenAI API**: Supports **both** WebSocket and WebRTC data channels equally

### WebSocket is Sufficient Because:

1. **LAN environment**: TCP overhead negligible (1-5ms)
2. **Reliable WiFi**: No packet loss to optimize for
3. **Audio tolerance**: 200-300ms latency acceptable (already have 176-426ms)
4. **Simplicity**: Faster to implement, easier to debug
5. **Proven**: WebSocket already working in proxy → OpenAI connection

### WebRTC Only Needed If:

1. **Direct device → OpenAI** (no proxy) - but we need proxy for API key security
2. **Poor network conditions** (high packet loss) - not our case
3. **Ultra-low latency** (<100ms required) - not needed for voice conversation
4. **P2P device-to-device** - not our use case

---

## Implementation Effort Comparison

### WebSocket Path

**Device changes** (~2-3 days):
1. Add esp_websocket_client dependency
2. Replace HTTP client with WebSocket client (~200 lines)
3. Handle WebSocket events (open/message/close)
4. Send binary audio frames
5. Receive binary audio frames

**Proxy changes** (~1 day):
1. Add WebSocket endpoint to FastAPI (using `fastapi.WebSocket`)
2. Handle device WebSocket connection
3. Multiplex: device WS ↔ OpenAI WS
4. Forward interruption signals

**Total**: ~3-4 days

---

### WebRTC Path

**Device changes** (~7-10 days):
1. Integrate libpeer or esp_peer library
2. Implement STUN client (NAT traversal)
3. Handle ICE candidate exchange
4. Implement SDP offer/answer
5. Set up WebRTC data channels
6. Audio codec negotiation
7. RTP packet handling
8. DTLS encryption setup

**Proxy changes** (~5-7 days):
1. Implement WebRTC server (aiortc/pion)
2. STUN/TURN server setup
3. Signaling server (WebSocket)
4. ICE candidate handling
5. SDP negotiation
6. WebRTC relay to OpenAI

**Total**: ~12-17 days

**Complexity multiplier**: 4-5x more code, 3-4x more time

---

## Decision Matrix

| Criteria | WebSocket | WebRTC | Winner |
|----------|-----------|--------|--------|
| **Latency (LAN)** | 200-300ms | 100-200ms | WebRTC (+) |
| **Implementation Time** | 3-4 days | 12-17 days | WebSocket (++) |
| **Code Complexity** | Low (200 lines) | High (1000+ lines) | WebSocket (+++) |
| **ESP32 Support** | Built-in, mature | External, newer | WebSocket (+++) |
| **Debugging** | Simple (text frames) | Complex (binary) | WebSocket (++) |
| **Reliability** | TCP (guaranteed) | UDP (best-effort) | WebSocket (+) |
| **NAT Traversal** | Not needed (LAN) | Complex (needed) | WebSocket (+) |
| **Maintenance** | Low | High | WebSocket (+) |
| **Sufficient for Use Case** | ✅ Yes | ✅ Yes (overkill) | WebSocket |

**Score**: WebSocket wins 7/9 criteria

---

## Recommendation

### Start with WebSocket

**Why**:
1. **80/20 rule**: Get 80% of benefits with 20% of effort
2. **Sufficient latency**: 200-300ms fine for voice conversation
3. **Fast iteration**: Can test continuous conversation in days, not weeks
4. **Lower risk**: Proven technology, less debugging
5. **Same result**: Both support interruption, turn detection, bidirectional streaming

### Future: Consider WebRTC if...

After testing WebSocket implementation, consider WebRTC **only if**:
- ✅ Latency feels too high (>500ms perceived)
- ✅ Adding remote access (device outside LAN)
- ✅ Network conditions degrade (packet loss observed)
- ✅ Direct device → OpenAI becomes viable (API key in device acceptable)

**Reality check**: WebSocket will likely be sufficient permanently. Our current HTTP implementation already achieves 176-426ms - WebSocket will be similar or better.

---

## ESP-IDF WebSocket Examples

**Official examples** (ESP-IDF v5.5.1):

1. **WebSocket echo server**: `~/esp/v5.5.1/esp-idf/examples/protocols/http_server/ws_echo_server/`
   - Shows WebSocket server basics
   - Not client, but useful for understanding protocol

2. **WebSocket client component**: Available via esp-protocols
   ```bash
   idf.py add-dependency "espressif/esp_websocket_client"
   ```

3. **Example repository**: [github.com/espressif/esp-protocols](https://github.com/espressif/esp-protocols/tree/master/components/esp_websocket_client/examples)
   - Working WebSocket client examples
   - Text and binary frame support
   - TLS/SSL examples

**Usage pattern** (from docs):
```c
// Initialize
esp_websocket_client_config_t ws_cfg = {
    .uri = "ws://proxy.local:8000/ws",
};
esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

// Connect
esp_websocket_client_start(client);

// Send binary audio
esp_websocket_client_send_bin(client, audio_data, audio_len, portMAX_DELAY);

// Receive in callback
static void websocket_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data) {
    if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
        // Handle received audio: data->data_ptr, data->data_len
    }
}
```

Simple, clean, ~100 lines total.

---

## Conclusion

**For continuous conversation**: **Use WebSocket**

Simple, fast to implement, sufficient performance, proven technology. Can always upgrade to WebRTC later if needed (which is unlikely).

**WebRTC is a trap**: Adds 4x complexity for 2x latency improvement we don't need on LAN.
