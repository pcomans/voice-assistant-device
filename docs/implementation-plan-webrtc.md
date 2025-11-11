# Implementation Plan: WebRTC Real-Time Audio Streaming (Option D2)

**Date:** 2025-01-08
**Status:** Planning
**Target:** ESP32 → Proxy (WebRTC) → OpenAI Realtime API

---

## Executive Summary

This document outlines the implementation plan for migrating from HTTP-based batch processing to WebRTC-based real-time streaming. The architecture uses a proxy to maintain API key security while achieving low latency (<200ms after button release) and resilient audio streaming over UDP.

**Target Architecture:**
```
ESP32 (WebRTC) → Local Proxy (WebRTC relay) → OpenAI (WebRTC)
        ↓                    ↓                        ↓
    Device Token      API Key Secure          Realtime API
```

**Expected Latency:** 150-350ms after button release (vs current 2000-3500ms)

---

## Table of Contents

1. [Open Questions & Decisions](#open-questions--decisions)
2. [Architecture Overview](#architecture-overview)
3. [Component Breakdown](#component-breakdown)
4. [Implementation Phases](#implementation-phases)
5. [Risk Assessment](#risk-assessment)
6. [Dependencies & Prerequisites](#dependencies--prerequisites)
7. [Testing Strategy](#testing-strategy)
8. [Rollback Plan](#rollback-plan)

---

## Open Questions & Decisions

### Critical Decisions (Must Answer Before Starting)

#### Q1: Proxy Technology Stack

**Question:** Which stack should we use for the WebRTC proxy?

**Options:**

| Stack | Pros | Cons | Effort |
|-------|------|------|--------|
| **A) Python/FastAPI + aiortc** | Familiar, existing code reusable, same deployment | aiortc less mature, limited examples | 3-4 days |
| **B) Node.js + mediasoup** | Excellent WebRTC support, many examples, production-ready | New stack, different deployment | 4-5 days |
| **C) Deno/TypeScript (ElatoAI)** | Modern, edge-ready, built-in WebRTC | New stack, Supabase dependency | 3-4 days |
| **D) Go + pion/webrtc** | High performance, official OpenAI examples | New language, learning curve | 5-6 days |

**Recommendation:** Python/FastAPI (familiar, reuse existing proxy code)

**Decision:** ✅ **Option A: Python/FastAPI + aiortc**

**Rationale:** Familiar stack, can reuse existing proxy infrastructure, same deployment process

**Impact:** Affects all proxy development, deployment, and maintenance

---

#### Q2: ESP32 WebRTC Implementation Approach

**Question:** Should we start from Espressif's demo or build custom?

**Options:**

| Approach | Pros | Cons | Effort |
|----------|------|------|--------|
| **A) Use Espressif demo as-is** | Working example, tested, official support | May not match our hardware, less control | 1-2 days |
| **B) Adapt Espressif demo** | Proven WebRTC stack, keep our audio code | Need to understand their code, integration work | 3-4 days |
| **C) Use esp_peer component directly** | Maximum control, clean integration | More complex, need to learn WebRTC details | 5-7 days |
| **D) Use ElatoAI firmware** | Working example with proxy pattern | Arduino framework (not ESP-IDF), different patterns | 2-3 days |

**Recommendation:** Adapt Espressif demo (proven WebRTC, customize for hardware)

**Decision:** ✅ **Option B: Adapt Espressif demo**

**Rationale:** Use proven esp_peer WebRTC stack, adapt audio I/O for Waveshare board

**Impact:** Affects device development timeline and code maintainability

---

#### Q3: Hardware Compatibility

**Question:** What hardware are we targeting?

**Current Setup:**
- Board: ✅ **Waveshare ESP32-S3-Touch-LCD-1.85C**
- Microphone: ✅ Built-in microphone
- Speaker: ✅ PCM5101 audio decoder + external speaker header
- Display: ✅ 1.85" LCD 360×360 touch screen
- Memory: ✅ 8MB PSRAM, 16MB Flash
- Button: ✅ Boot button (can use for push-to-talk)
- Audio: ✅ Volume adjustment knob, amplifier chip

**Espressif Demo Requirements:**
- ESP32-S3-Korvo-2 (has built-in I2S microphone and speaker)
- Specific audio board pinout

**Compatibility Assessment:**
- ❌ Different board than Espressif demo
- ✅ Has all required audio peripherals (mic, speaker)
- ✅ Sufficient PSRAM/Flash for WebRTC
- ⚠️ Need to adapt I2S pinout for Waveshare board
- ✅ Can reuse existing I2S audio code

**Decision:** ✅ **Adapt Espressif demo for Waveshare board**

**Action Items:**
- Find/configure I2S pins for PCM5101 audio decoder
- Find/configure I2S pins for microphone
- Keep existing I2S capture/playback code
- Integrate esp_peer WebRTC stack from Espressif demo

**Impact:** Need to configure audio I/O pins, but hardware is fully capable

---

#### Q4: Audio Codec Strategy

**Question:** How should we handle Opus encoding/decoding?

**Current State:**
- Device: Decodes Opus (ESP Audio Codec)
- Proxy: Encodes PCM → Opus
- OpenAI: Outputs PCM16 (24kHz)

**WebRTC Standard:**
- WebRTC uses Opus natively (built-in)
- OpenAI WebRTC API uses Opus

**Options:**

| Approach | Device → Proxy | Proxy → OpenAI | Proxy → Device | Pros | Cons |
|----------|----------------|----------------|----------------|------|------|
| **A) All Opus** | Opus | Opus | Opus | Native WebRTC, lowest bandwidth | Device encodes Opus (CPU cost) |
| **B) PCM to Proxy** | PCM | Opus | Opus | Device simpler (no encode) | Higher device→proxy bandwidth |
| **C) Keep Current** | PCM | PCM→Opus | Opus→PCM | Minimal changes | Extra codec steps, latency |

**Questions:**
- [ ] Can ESP32 encode Opus efficiently? (ESP Audio Codec supports encoding?)
- [ ] Is local network bandwidth a concern? (PCM = 10x more than Opus)
- [ ] Do we want to minimize device CPU usage?

**Recommendation:** ?

**Decision:** [ ] To be decided

**Impact:** Affects device CPU usage, bandwidth, and codec complexity

---

#### Q5: Signaling Mechanism

**Question:** How do we handle WebRTC signaling (SDP exchange, ICE candidates)?

**Background:** WebRTC requires exchanging session descriptions (SDP) and network candidates (ICE) before streaming.

**Options:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| **A) HTTP POST** | Device posts SDP offer, gets SDP answer via HTTP | Simple, uses existing endpoint | Extra round-trip, not standard |
| **B) WebSocket Signaling** | Separate WebSocket for signaling, WebRTC for media | Standard pattern, clean separation | Two connections |
| **C) Custom Signaling (Espressif)** | Use Espressif's esp_signaling_get_openai_signaling | Proven, works with demo | Locked into Espressif pattern |
| **D) DataChannel** | Use WebRTC DataChannel for control messages | Single connection, standard | More complex initialization |

**Questions:**
- [ ] Do we need to support ICE/STUN/TURN? (NAT traversal)
- [ ] Is local network only acceptable? (simplifies signaling)
- [ ] Should signaling reuse existing HTTP endpoint?

**Recommendation:** ?

**Decision:** [ ] To be decided

**Impact:** Affects connection establishment, firewall compatibility, complexity

---

#### Q6: Connection Lifecycle

**Question:** How do we manage WebRTC connection lifecycle?

**Options:**

| Pattern | Description | Pros | Cons |
|---------|-------------|------|------|
| **A) Persistent Connection** | Keep WebRTC peer connection alive across requests | Lowest latency, no reconnect overhead | Connection state management, recovery complexity |
| **B) Session-Based** | New connection per conversation session | Cleaner state, easier error recovery | Reconnect overhead (~500ms) |
| **C) Connection Pool** | Pool of pre-established connections | Balance of simplicity and performance | More complex, resource overhead |

**Questions:**
- [ ] How long is a typical conversation? (affects connection duration)
- [ ] How often do network interruptions occur?
- [ ] Do we need to support multiple devices simultaneously?

**Recommendation:** ?

**Decision:** [ ] To be decided

**Impact:** Affects latency, resource usage, error handling complexity

---

#### Q7: Interaction Model

**Question:** Keep button-based or move to hands-free?

**Options:**

| Mode | Description | Pros | Cons | Effort |
|------|-------------|------|------|--------|
| **A) Push-to-Talk (Current)** | Press button to record, release to send | Simple, clear intent, no VAD needed | Less natural, requires button | Minimal change |
| **B) Hands-Free (VAD)** | Always listening, use Voice Activity Detection | Natural conversation, no button | False triggers, privacy concerns, VAD complexity | +2-3 days |
| **C) Hybrid** | Support both modes, user configurable | Flexible, best of both | More complex UI/logic | +1-2 days |

**Questions:**
- [ ] Is push-to-talk acceptable for your use case?
- [ ] Do you want to implement VAD (Voice Activity Detection)?
- [ ] Should VAD run on device or proxy?

**Recommendation:** Start with push-to-talk, add VAD in Phase 2

**Decision:** [ ] To be decided

**Impact:** Affects UX, device complexity, and implementation timeline

---

#### Q8: Proxy Deployment

**Question:** Where and how will the proxy run?

**Current Setup:**
- Proxy runs at: _____
- Network: Local WiFi / Cloud / Other: _____
- Port forwarding: Yes / No

**WebRTC Considerations:**
- May need STUN/TURN server for NAT traversal
- If local-only, can simplify (no ICE)
- Edge deployment (Deno/Supabase) vs self-hosted

**Questions:**
- [ ] Keep current proxy deployment location?
- [ ] Need remote access (outside local network)?
- [ ] Willing to use cloud services (Supabase Edge, Cloudflare Workers)?

**Recommendation:** ?

**Decision:** [ ] To be decided

**Impact:** Affects deployment complexity, cloud costs, network configuration

---

### Secondary Decisions (Can Decide During Implementation)

#### Q9: Error Handling & Reconnection

**Questions:**
- [ ] How to handle WebRTC connection failures?
- [ ] Auto-reconnect strategy? (exponential backoff, max retries)
- [ ] Fallback to HTTP if WebRTC fails?

**Decision:** [ ] Defer to implementation

---

#### Q10: Monitoring & Debugging

**Questions:**
- [ ] What metrics to track? (latency, packet loss, jitter, CPU usage)
- [ ] WebRTC statistics logging?
- [ ] Debug mode for packet inspection?

**Decision:** [ ] Defer to implementation

---

#### Q11: Audio Quality vs Bandwidth

**Questions:**
- [ ] Opus bitrate? (6kbps = lowest, 24kbps = good, 64kbps = excellent)
- [ ] Sample rate? (16kHz = lower quality, 24kHz = OpenAI native, 48kHz = best)
- [ ] Mono vs Stereo? (mono recommended for voice)

**Recommendation:** 24kHz mono, 24kbps Opus (matches OpenAI)

**Decision:** [ ] To be decided

---

## Architecture Overview

### High-Level Architecture

```
┌─────────────────────┐         ┌──────────────────────┐         ┌─────────────────────┐
│                     │ WebRTC  │                      │ WebRTC  │                     │
│   ESP32-S3 Device   │────────>│   Proxy Server       │────────>│   OpenAI Realtime   │
│                     │  (UDP)  │   (WebRTC Relay)     │  (UDP)  │        API          │
│  - I2S Microphone   │         │   - SDP Exchange     │         │  - Speech-to-Speech │
│  - I2S Speaker      │         │   - ICE Handling     │         │  - Function Calling │
│  - Button Input     │         │   - API Key Storage  │         │  - Streaming Audio  │
│  - esp_peer         │         │   - Device Auth      │         │                     │
│  - Opus Codec       │         │   - Monitoring       │         │                     │
└─────────────────────┘         └──────────────────────┘         └─────────────────────┘
         │                                  │                              │
         │                                  │                              │
    Device Token                      OpenAI API Key                 Opus Audio
    (encrypted)                       (server-side)                  (24kHz PCM16)
```

### Data Flow

**Connection Establishment:**
```
1. Device → Proxy (HTTP): POST /webrtc/offer with SDP offer + device token
2. Proxy → OpenAI: Establish WebRTC connection (if not exists)
3. Proxy → Device (HTTP): Return SDP answer
4. Device ↔ Proxy: ICE candidate exchange
5. WebRTC PeerConnection established (UDP)
```

**Audio Streaming (During Conversation):**
```
While button pressed:
  Device: I2S capture (16ms chunks) → Opus encode → RTP → Proxy
  Proxy: RTP receive → Forward to OpenAI WebRTC connection
  OpenAI: Process audio, generate response

When response ready:
  OpenAI: Stream response audio → Proxy (RTP)
  Proxy: Forward RTP → Device
  Device: RTP receive → Opus decode → I2S playback
```

---

## Component Breakdown

### ESP32 Device Components

#### 1. WebRTC Stack
- **Component:** `esp_peer` from Espressif
- **Functionality:**
  - RTCPeerConnection management
  - SDP offer/answer generation
  - ICE candidate handling
  - RTP/SRTP media transport
  - DTLS encryption
- **Files to Modify/Create:**
  - `main/webrtc_client.c` (new)
  - `main/webrtc_client.h` (new)
  - Integration with CMakeLists.txt

#### 2. Audio Capture & Encoding
- **Current:** I2S capture → PCM16 @ 16kHz
- **Target:** I2S capture → Opus encode → RTP packets
- **Options:**
  - **Option A:** Keep current I2S code, add Opus encoder
  - **Option B:** Use Espressif demo's audio code
- **Files:**
  - `main/audio_controller.c` (modify or replace)
  - `main/opus_encoder.c` (new if not using demo)

#### 3. Audio Playback & Decoding
- **Current:** I2S playback (has Opus decoder)
- **Target:** RTP receive → Opus decode → I2S playback
- **Files:**
  - `main/audio_playback.c` (modify)
  - Keep existing Opus decoder (already implemented)

#### 4. Signaling Client
- **Functionality:**
  - Exchange SDP with proxy
  - Send ICE candidates
  - Handle connection state
- **Protocol:** HTTP POST (reuse existing client) or WebSocket
- **Files:**
  - `main/signaling.c` (new)
  - Or extend `main/proxy_client.c`

#### 5. Button Handler
- **Current:** GPIO button detection
- **Target:** Trigger WebRTC audio streaming on/off
- **Files:**
  - Keep existing button code
  - Integrate with WebRTC start/stop

---

### Proxy Server Components

#### 1. WebRTC Server (Technology TBD)

**If Python/FastAPI + aiortc:**
```python
# New files:
smart_assistant_proxy/webrtc_relay.py    # WebRTC peer management
smart_assistant_proxy/signaling.py       # SDP/ICE handling
smart_assistant_proxy/openai_webrtc.py   # OpenAI WebRTC client

# Modified files:
smart_assistant_proxy/app.py             # Add WebRTC endpoints
smart_assistant_proxy/config.py          # Add WebRTC config
```

**If Node.js + mediasoup:**
```javascript
// New project structure:
proxy/
  src/
    webrtc-relay.ts      // WebRTC relay logic
    openai-client.ts     // OpenAI WebRTC connection
    signaling.ts         // SDP/ICE endpoints
    auth.ts              // Device token validation
```

**If Deno/TypeScript:**
```typescript
// Supabase Edge Functions:
supabase/functions/
  webrtc-signaling/     // SDP exchange
  webrtc-relay/         // Audio relay
  device-auth/          // Token validation
```

#### 2. OpenAI WebRTC Client
- **Functionality:**
  - Establish WebRTC connection to OpenAI
  - Persistent connection (reused across device sessions)
  - Forward audio from device to OpenAI
  - Stream OpenAI response back to device
- **Key Considerations:**
  - Connection pooling (one per device or shared?)
  - Error recovery and reconnection
  - Session management

#### 3. Signaling Endpoints
- **Endpoints:**
  - `POST /webrtc/offer` - Accept SDP offer from device
  - `POST /webrtc/ice` - Accept ICE candidates
  - `GET /webrtc/status` - Connection status
- **Authentication:** Device token validation
- **Response:** SDP answer, ICE candidates

#### 4. Device Management
- **Functionality:**
  - Device token generation/validation
  - Connection state tracking
  - Rate limiting per device
- **Storage:** In-memory or Redis
- **Files:** Reuse existing auth logic

---

## Implementation Phases

### Phase 0: Research & Setup (1 day)

**Goal:** Answer open questions and set up development environment

**Tasks:**
- [ ] **Decision:** Choose proxy stack (Q1)
- [ ] **Decision:** Choose ESP32 implementation approach (Q2)
- [ ] **Decision:** Confirm hardware compatibility (Q3)
- [ ] **Decision:** Decide audio codec strategy (Q4)
- [ ] Clone relevant repositories:
  - [ ] `git clone https://github.com/espressif/esp-webrtc-solution`
  - [ ] `git clone https://github.com/openai/openai-realtime-embedded` (reference)
- [ ] Set up development environment:
  - [ ] Install proxy dependencies (Python aiortc / Node.js / Deno)
  - [ ] Install ESP-IDF components (esp_peer)
- [ ] Read through example code:
  - [ ] Espressif OpenAI demo
  - [ ] ElatoAI firmware (if using as reference)

**Deliverables:**
- ✅ All critical decisions made (Q1-Q8)
- ✅ Development environment ready
- ✅ Understanding of example code structure

---

### Phase 1: Proxy WebRTC Server (2-3 days)

**Goal:** Build WebRTC relay server that can connect to OpenAI

**Tasks:**

#### Day 1: Basic WebRTC Server
- [ ] Set up project structure (based on stack decision)
- [ ] Implement signaling endpoint (`POST /webrtc/offer`)
- [ ] Create WebRTC peer connection handler
- [ ] Test signaling with browser client (simple test)

#### Day 2: OpenAI Integration
- [ ] Implement OpenAI WebRTC client connection
- [ ] Test direct audio streaming to OpenAI
- [ ] Verify audio can be sent and received
- [ ] Test with simple audio file playback

#### Day 3: Audio Relay Logic
- [ ] Implement bidirectional audio relay:
  - Device → Proxy → OpenAI
  - OpenAI → Proxy → Device
- [ ] Add device authentication (token validation)
- [ ] Add connection state management
- [ ] Test with mock RTP audio packets

**Deliverables:**
- ✅ Working WebRTC proxy server
- ✅ Successfully connects to OpenAI WebRTC API
- ✅ Can relay audio bidirectionally
- ✅ Device authentication working

**Testing:**
- Test with browser WebRTC client
- Test audio file → OpenAI → receive response
- Verify latency is acceptable (<200ms)

---

### Phase 2: ESP32 WebRTC Client (3-4 days)

**Goal:** Implement WebRTC client on ESP32 device

**Tasks:**

#### Day 1: WebRTC Stack Integration
- [ ] Add esp_peer component to project
- [ ] Implement WebRTC peer connection setup
- [ ] Implement signaling client (SDP exchange with proxy)
- [ ] Test connection establishment (no audio yet)

#### Day 2: Audio Capture Integration
- [ ] Integrate I2S audio capture with WebRTC
- [ ] Implement Opus encoding (or use existing if available)
- [ ] Create RTP packets from captured audio
- [ ] Test audio transmission to proxy

#### Day 3: Audio Playback Integration
- [ ] Implement RTP packet reception
- [ ] Integrate Opus decoder (already have this)
- [ ] Connect to I2S playback
- [ ] Test receiving audio from proxy

#### Day 4: End-to-End Testing & Refinement
- [ ] Test full conversation flow
- [ ] Measure latency (button press → audio playback)
- [ ] Fix audio quality issues
- [ ] Implement reconnection logic
- [ ] Add error handling

**Deliverables:**
- ✅ ESP32 establishes WebRTC connection to proxy
- ✅ Audio capture → RTP transmission working
- ✅ RTP reception → audio playback working
- ✅ End-to-end conversation working

**Testing:**
- Speak into device, verify OpenAI responds
- Measure latency at each stage
- Test connection recovery after network interruption
- Verify audio quality is acceptable

---

### Phase 3: Integration & Optimization (2 days)

**Goal:** Integrate with existing features and optimize performance

**Tasks:**

#### Day 1: Feature Integration
- [ ] Integrate button control (start/stop WebRTC streaming)
- [ ] Add LED status indicators (connecting, connected, error)
- [ ] Implement proper cleanup on connection close
- [ ] Add configuration options (WiFi settings, server URL)

#### Day 2: Performance Optimization
- [ ] Optimize buffer sizes (minimize latency)
- [ ] Tune Opus encoder settings (bitrate, complexity)
- [ ] Reduce CPU usage (measure and optimize hotspots)
- [ ] Test memory usage (check for leaks)
- [ ] Optimize RTP packet size

**Deliverables:**
- ✅ Full feature integration
- ✅ Optimized latency (<200ms target)
- ✅ Stable memory usage (no leaks)
- ✅ Acceptable CPU usage (<30%)

---

### Phase 4: Testing & Hardening (2-3 days)

**Goal:** Ensure production-ready quality

**Tasks:**

#### Testing
- [ ] **Latency Testing:**
  - Measure end-to-end latency (10 samples)
  - Breakdown by component (capture, network, processing, playback)
  - Verify meets <200ms target
- [ ] **Stability Testing:**
  - 24-hour continuous operation test
  - 100 consecutive conversation cycles
  - Memory leak detection
- [ ] **Network Resilience:**
  - Test with WiFi disconnection/reconnection
  - Test with high packet loss (10%, 20%)
  - Test with variable latency (50ms jitter)
- [ ] **Audio Quality:**
  - Subjective listening tests
  - Compare to current implementation
  - Test in noisy environment
- [ ] **Security Testing:**
  - Verify API key never exposed
  - Test token authentication
  - Check for unencrypted audio transmission

#### Hardening
- [ ] Add comprehensive error handling
- [ ] Add logging for debugging
- [ ] Implement graceful degradation
- [ ] Add health check endpoint
- [ ] Document common issues and solutions

**Deliverables:**
- ✅ Comprehensive test results documented
- ✅ All critical bugs fixed
- ✅ Production-ready code
- ✅ Documentation complete

---

### Phase 5: Documentation & Deployment (1 day)

**Goal:** Document and deploy the solution

**Tasks:**
- [ ] Write user documentation
- [ ] Document proxy setup/deployment
- [ ] Document device configuration
- [ ] Create troubleshooting guide
- [ ] Update TDD with actual results
- [ ] Deploy proxy to production environment
- [ ] Flash device firmware
- [ ] Verify production deployment

**Deliverables:**
- ✅ Complete documentation
- ✅ Production deployment
- ✅ Updated TDD with actual metrics

---

## Risk Assessment

### High-Risk Items

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **WebRTC complexity higher than estimated** | Medium | High | Use official demos as starting point, allocate buffer time |
| **esp_peer component bugs/limitations** | Medium | High | Test early, have fallback to simpler WebSocket approach |
| **Audio codec issues (quality/CPU)** | Medium | Medium | Test early, have fallback to PCM if Opus problematic |
| **NAT/firewall blocking WebRTC** | Low (local) / High (cloud) | High | Start with local-only, add TURN if needed |
| **Latency higher than expected** | Medium | Medium | Measure early and often, optimize incrementally |

### Medium-Risk Items

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Proxy stack learning curve** | Medium | Medium | Choose familiar stack (Python), allocate learning time |
| **Integration with existing audio code** | Low | Medium | Keep existing I2S code, adapt WebRTC to it |
| **Connection stability issues** | Medium | Medium | Implement robust reconnection logic |
| **Memory leaks in WebRTC stack** | Low | Medium | Regular memory profiling, long-running tests |

### Low-Risk Items

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Button integration issues** | Low | Low | Simple GPIO, well-tested in current code |
| **Device authentication** | Low | Low | Reuse existing token mechanism |
| **OpenAI API changes** | Low | Medium | Monitor OpenAI changelog, use stable API version |

---

## Dependencies & Prerequisites

### Hardware Requirements

**ESP32 Device:**
- [ ] ESP32-S3 with >= 8MB PSRAM
- [ ] I2S microphone
- [ ] I2S speaker/amplifier
- [ ] Button connected to GPIO
- [ ] USB cable for programming
- [ ] (Optional) ESP32-S3-Korvo-2 development board

**Proxy Server:**
- [ ] Raspberry Pi 4 (4GB RAM) or equivalent
- [ ] Or cloud VM (2 vCPU, 4GB RAM minimum)
- [ ] Reliable network connection
- [ ] (Optional) Domain name for HTTPS

### Software Requirements

**Development Environment:**
- [ ] ESP-IDF v5.5.1 (already installed)
- [ ] Python 3.8+ / Node.js 18+ / Deno 1.40+ (depending on stack choice)
- [ ] Git
- [ ] (Optional) Docker for proxy deployment

**ESP32 Dependencies:**
- [ ] `esp_peer` component from Espressif
- [ ] `esp_webrtc` component
- [ ] ESP Audio Codec (already have)
- [ ] mbedTLS (already in ESP-IDF)

**Proxy Dependencies (varies by stack):**

**If Python/FastAPI:**
```bash
pip install fastapi uvicorn aiortc websockets numpy
```

**If Node.js:**
```bash
npm install mediasoup wrtc ws express
```

**If Deno:**
```bash
# No installation needed, imports from URL
```

### External Services

**Required:**
- [ ] OpenAI API account with Realtime API access
- [ ] Valid OpenAI API key

**Optional:**
- [ ] STUN server (if NAT traversal needed)
- [ ] TURN server (if symmetric NAT or strict firewall)
- [ ] Monitoring service (Prometheus, Grafana)

### Network Requirements

**Local Network:**
- [ ] WiFi network with WPA2/WPA3
- [ ] Router allows UDP traffic (for WebRTC)
- [ ] No client isolation on WiFi
- [ ] (Optional) Static IP for proxy server

**Firewall Ports:**
- [ ] HTTP/HTTPS for signaling (TCP 8000 or 443)
- [ ] WebRTC media (UDP range: 10000-20000 or configured range)
- [ ] (Optional) STUN/TURN ports if using external servers

---

## Testing Strategy

### Unit Tests

**Proxy Components:**
- [ ] WebRTC peer connection initialization
- [ ] SDP offer/answer generation
- [ ] ICE candidate handling
- [ ] Audio packet relay logic
- [ ] Device authentication

**Device Components:**
- [ ] WebRTC peer connection
- [ ] Signaling client
- [ ] Audio capture → RTP conversion
- [ ] RTP → audio playback conversion

### Integration Tests

- [ ] Device → Proxy SDP exchange
- [ ] Device → Proxy → OpenAI audio flow
- [ ] OpenAI → Proxy → Device audio flow
- [ ] Connection recovery after interruption
- [ ] Multi-device scenario (if applicable)

### Performance Tests

- [ ] Latency measurement:
  - End-to-end (button press → playback start)
  - Component-level (capture, network, decode, playback)
  - 99th percentile latency
- [ ] CPU usage:
  - Device (should be <30%)
  - Proxy (should be <50% per connection)
- [ ] Memory usage:
  - Device (check for leaks)
  - Proxy (check for leaks)
- [ ] Network bandwidth:
  - Measure actual bandwidth usage
  - Verify Opus compression working

### Stress Tests

- [ ] 100 consecutive conversations
- [ ] 24-hour continuous operation
- [ ] Rapid connection/disconnection cycles
- [ ] Simulated packet loss (10%, 20%, 30%)
- [ ] Simulated network latency (50ms, 100ms, 200ms)
- [ ] Multiple devices simultaneously (if applicable)

### Acceptance Criteria

**Must Have:**
- ✅ Latency < 300ms (stretch goal: <200ms)
- ✅ Audio quality comparable to current implementation
- ✅ No crashes in 100 conversation cycles
- ✅ No memory leaks in 24-hour test
- ✅ API key never exposed (security test passes)
- ✅ Works on target hardware

**Nice to Have:**
- ✅ Latency < 200ms
- ✅ Better audio quality than current (thanks to WebRTC FEC)
- ✅ Handles 20% packet loss gracefully
- ✅ CPU usage < 20% on device

---

## Rollback Plan

### Rollback Triggers

**Rollback if:**
- [ ] Implementation exceeds 15 days (2x estimate)
- [ ] Latency consistently > 500ms (worse than Option C target)
- [ ] Critical security vulnerability found
- [ ] Stability issues can't be resolved
- [ ] Audio quality significantly worse than current

### Rollback Options

**Option 1: Revert to Current (HTTP Batch)**
- Keep existing code
- No changes needed
- Time: Immediate
- Latency: 2000-3500ms (current)

**Option 2: Implement Option C (HTTP POSTs)**
- Fall back to simpler HTTP streaming approach
- Time: 1-2 days
- Latency: 176-426ms
- Lower risk, proven technology

**Option 3: Hybrid Approach**
- Keep WebRTC proxy for OpenAI connection
- Use WebSocket for device → proxy
- Easier device implementation
- Time: +2 days
- Latency: 200-400ms

### Rollback Procedure

1. **Preserve Current Code:**
   - [ ] Create git branch `backup-before-webrtc`
   - [ ] Tag current release: `v1.0-pre-webrtc`

2. **If Rollback Needed:**
   - [ ] Stop proxy service
   - [ ] Flash device with previous firmware
   - [ ] Restart proxy service
   - [ ] Verify system working
   - [ ] Document lessons learned

3. **Post-Rollback:**
   - [ ] Analyze what went wrong
   - [ ] Decide on next steps
   - [ ] Update implementation plan if retrying

---

## Success Metrics

### Technical Metrics

| Metric | Current | Target | Measurement Method |
|--------|---------|--------|-------------------|
| **Latency (button release → playback)** | 2000-3500ms | <300ms | Timer in code |
| **Latency (95th percentile)** | ~3000ms | <350ms | 100 samples |
| **CPU Usage (device)** | ~15% | <30% | ESP-IDF profiler |
| **CPU Usage (proxy)** | ~10% | <50% | System monitor |
| **Memory Usage (device)** | ~200KB | <400KB | ESP-IDF heap trace |
| **Bandwidth (device→proxy)** | ~32KB/s | <5KB/s | Network monitor |
| **Packet Loss Tolerance** | 0% | <10% | Network simulator |
| **Connection Uptime** | N/A | >99% | 24-hour test |

### User Experience Metrics

| Metric | Target | Measurement |
|--------|--------|-------------|
| **Response feels instant** | >90% users | User survey |
| **Audio quality acceptable** | >95% users | User survey |
| **Connection reliability** | >95% success | Log analysis |
| **No perceptible glitches** | >90% sessions | User testing |

### Business Metrics

| Metric | Target | Measurement |
|--------|--------|-------------|
| **Development time** | <10 days | Time tracking |
| **Time to first working demo** | <5 days | Milestone |
| **Bugs found in testing** | <5 critical | Issue tracker |
| **Time to production** | <15 days | Calendar |

---

## Next Steps

### Immediate Actions (Before Starting Phase 0)

1. **Review this document** with stakeholders
2. **Answer critical questions** (Q1-Q8)
3. **Set up decision-making process** for open questions
4. **Assign resources** (who will work on proxy vs device)
5. **Schedule kickoff meeting** to review plan
6. **Create GitHub issues** for each phase
7. **Set up project tracking** (Kanban board, milestones)

### Decision Points

**After Phase 0 (Research & Setup):**
- [ ] Go/No-Go decision based on research findings
- [ ] Confirm timeline is realistic
- [ ] Adjust phases if needed

**After Phase 1 (Proxy Server):**
- [ ] Verify proxy works with test client
- [ ] Confirm OpenAI integration works
- [ ] Go/No-Go for device implementation

**After Phase 2 (ESP32 Client):**
- [ ] Measure actual latency vs target
- [ ] Assess audio quality
- [ ] Go/No-Go for production deployment

---

## Appendix: Reference Architectures

### A. Espressif OpenAI Demo Architecture

```
ESP32-S3-Korvo-2
  ├─ esp_peer (WebRTC PeerConnection)
  ├─ esp_signaling_get_openai_signaling()
  ├─ Direct connection to OpenAI
  └─ API key stored on device ⚠️

Pros: Simple, working example
Cons: API key exposure, not suitable for production
```

### B. ElatoAI Architecture

```
ESP32-S3
  ├─ WebSocket to Deno Edge Functions
  ├─ Opus codec (12kbps)
  └─ Device token authentication ✅

Deno Edge Functions (Supabase)
  ├─ WebSocket server for device
  ├─ WebRTC client to OpenAI
  └─ API key server-side ✅

Pros: Secure, edge deployment, working example
Cons: Requires Supabase, Deno learning curve
```

### C. Our Target Architecture (Option D2)

```
ESP32-S3
  ├─ esp_peer (WebRTC)
  ├─ HTTP signaling to local proxy
  ├─ Device token authentication ✅
  └─ Opus codec (24kHz)

Local Proxy (Python/Node.js/Deno)
  ├─ WebRTC server for device
  ├─ WebRTC client to OpenAI
  ├─ SDP/ICE exchange
  └─ API key server-side ✅

OpenAI Realtime API
  └─ WebRTC endpoint

Pros: Secure, low latency, self-hosted, flexible
Cons: More complex, need to build relay
```

---

## Document Status

- **Status:** DRAFT - Awaiting Decisions
- **Last Updated:** 2025-01-08
- **Next Review:** After critical decisions made
- **Owner:** Engineering Team
- **Decisions Needed:** 8 critical questions (Q1-Q8)

---

**Questions or feedback?** Please review and provide input on open questions before starting Phase 0.
