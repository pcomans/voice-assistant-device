# Continuous Conversation Implementation Tasks

**Goal**: Single button press → dynamic conversation with turn detection and interruption

**Architecture**: Device (WebSocket) ↔ Proxy (WebSocket) ↔ OpenAI Realtime API

---

## Phase 1: Device WebSocket Migration (FIRST PRIORITY)

### Task 1.1: Add WebSocket Client Dependency
- [ ] Add esp_websocket_client to project
  ```bash
  cd smart_assistant_device
  idf.py add-dependency "espressif/esp_websocket_client"
  ```
- [ ] Verify dependency added to `main/idf_component.yml`
- [ ] Build to confirm no conflicts

**Effort**: 30 minutes
**Blocker**: None

---

### Task 1.2: Study ESP-IDF WebSocket Example
- [ ] Review official example: `~/esp/v5.5.1/esp-idf/examples/protocols/http_server/ws_echo_server/`
- [ ] Study esp-protocols WebSocket client examples: [github.com/espressif/esp-protocols](https://github.com/espressif/esp-protocols/tree/master/components/esp_websocket_client/examples)
- [ ] Document key APIs:
  - `esp_websocket_client_init()`
  - `esp_websocket_client_start()`
  - `esp_websocket_client_send_bin()`
  - Event handler callbacks
- [ ] Test example on device (if time permits)

**Effort**: 1-2 hours
**Blocker**: Task 1.1 complete

---

### Task 1.3: Create WebSocket Client Module
- [ ] Create `main/websocket_client.h` and `main/websocket_client.c`
- [ ] Define interface:
  ```c
  typedef void (*ws_audio_received_cb_t)(const uint8_t *data, size_t len);

  esp_err_t ws_client_init(const char *uri, ws_audio_received_cb_t callback);
  esp_err_t ws_client_connect(void);
  esp_err_t ws_client_send_audio(const uint8_t *data, size_t len);
  esp_err_t ws_client_disconnect(void);
  ```
- [ ] Implement WebSocket event handler (connect/disconnect/data/error)
- [ ] Handle binary audio frames
- [ ] Add reconnection logic (connection lost)

**Effort**: 3-4 hours
**Blocker**: Task 1.2 complete

---

### Task 1.4: Replace HTTP with WebSocket in proxy_client.c
- [ ] Keep existing `proxy_client_init()` interface (for backward compatibility)
- [ ] Replace HTTP POST logic with WebSocket send in `proxy_stream_send_chunk()`
- [ ] Update session management (session starts on WS connect, not first chunk)
- [ ] Remove HTTP client code paths (after testing)
- [ ] Update error handling (WS disconnect vs HTTP errors)

**Effort**: 2-3 hours
**Blocker**: Task 1.3 complete

---

### Task 1.5: Update Audio Playback Integration
- [ ] Modify callback to use WebSocket-received audio
- [ ] Test: WebSocket audio → ring buffer → I2S playback
- [ ] Verify no audio glitches or gaps
- [ ] Keep pre-buffering logic (500ms)

**Effort**: 1 hour
**Blocker**: Task 1.4 complete

---

### Task 1.6: Test WebSocket Migration End-to-End
- [ ] Build and flash firmware
- [ ] Test single button press → record → receive response
- [ ] Verify: Same functionality as HTTP version
- [ ] Measure latency (should be similar: 176-426ms)
- [ ] Test edge cases: connection loss, timeout, empty responses

**Effort**: 2-3 hours
**Blocker**: Task 1.5 complete

**Milestone**: ✅ **Device now using WebSocket (functionally equivalent to HTTP)**

---

## Phase 2: Proxy WebSocket Server (PARALLEL WITH PHASE 1)

### Task 2.1: Add FastAPI WebSocket Endpoint
- [ ] Create `/ws` WebSocket route in `app.py`
- [ ] Handle WebSocket connection lifecycle
- [ ] Parse binary audio frames from device
- [ ] Forward to existing OpenAI WebSocket connection
- [ ] Keep HTTP endpoint for backward compatibility (during migration)

**Effort**: 2-3 hours
**Blocker**: None (can start immediately)

---

### Task 2.2: Implement Bidirectional Multiplexing
- [ ] Receive binary audio from device → forward to OpenAI
- [ ] Receive audio deltas from OpenAI → forward to device
- [ ] Handle multiple concurrent device connections (session mapping)
- [ ] Add session timeout (30s inactivity)

**Effort**: 3-4 hours
**Blocker**: Task 2.1 complete

---

### Task 2.3: Test Proxy WebSocket
- [ ] Unit test: WebSocket connection handling
- [ ] Integration test: Device WS → Proxy → OpenAI
- [ ] Test: Multiple sessions simultaneously
- [ ] Test: Session timeout and cleanup

**Effort**: 2 hours
**Blocker**: Task 2.2 complete

**Milestone**: ✅ **Proxy supports WebSocket (backward compatible with HTTP)**

---

## Phase 3: Enable Turn Detection

### Task 3.1: Update Proxy Session Config
- [ ] Change `turn_detection` from `None` to:
  ```python
  "turn_detection": {
      "type": "semantic_vad",
      "threshold": 0.5,
      "prefix_padding_ms": 300,
      "silence_duration_ms": 500
  }
  ```
- [ ] Test with existing device (HTTP or WebSocket)
- [ ] Verify: AI responds automatically when user stops speaking
- [ ] Tune parameters if needed (threshold, silence duration)

**Effort**: 1 hour
**Blocker**: None (works with current device)

---

### Task 3.2: Test Automatic Turn Detection
- [ ] Record → AI responds without button release
- [ ] Test: Multiple back-and-forth turns
- [ ] Measure: Time from speech stop to response start
- [ ] Document: Optimal VAD parameters

**Effort**: 1-2 hours
**Blocker**: Task 3.1 complete

**Milestone**: ✅ **Turn detection working (AI auto-responds)**

---

## Phase 4: Continuous Conversation Mode

### Task 4.1: Update Device State Machine
- [ ] Add `ASSISTANT_STATE_CONVERSATION` state
- [ ] Button press → enter CONVERSATION mode
- [ ] In CONVERSATION: Keep mic streaming, accept playback responses
- [ ] Button press again → exit CONVERSATION, return to IDLE
- [ ] Remove sequential RECORDING → SENDING → PLAYING states

**Effort**: 2-3 hours
**Blocker**: Phase 1 complete (WebSocket migration)

---

### Task 4.2: Implement Session Start/End Protocol
- [ ] Device sends "session.start" message on button press
- [ ] Device sends "session.end" message on second button press
- [ ] Proxy creates/destroys OpenAI session accordingly
- [ ] Handle session timeout (30s silence → auto-end)

**Effort**: 2 hours
**Blocker**: Task 4.1 complete

---

### Task 4.3: Test Continuous Recording + Playback
- [ ] Verify: Mic keeps recording during AI response
- [ ] Verify: I2S0 (playback) and I2S1 (capture) don't interfere
- [ ] Test: User speaks → AI responds → user speaks again (no button)
- [ ] Measure: Simultaneous record + playback performance

**Effort**: 2-3 hours
**Blocker**: Task 4.2 complete

**Milestone**: ✅ **Continuous conversation working (single button press)**

---

## Phase 5: Interruption (Barge-in)

### Task 5.1: Handle Interruption Events in Proxy
- [ ] Listen for `input_audio_buffer.speech_started` from OpenAI
- [ ] When received: Send "stop_playback" message to device WebSocket
- [ ] Forward new user audio to OpenAI (replaces interrupted response)

**Effort**: 1-2 hours
**Blocker**: Phase 4 complete

---

### Task 5.2: Implement Immediate Playback Stop on Device
- [ ] Add `audio_playback_stop_immediate()` function
  - Stop I2S immediately (don't drain)
  - Clear ring buffer (drop queued audio)
  - Notify app (playback stopped)
- [ ] Handle "stop_playback" message in WebSocket client
- [ ] Keep mic recording throughout (don't stop on interruption)

**Effort**: 2 hours
**Blocker**: Task 5.1 complete

---

### Task 5.3: Test Interruption
- [ ] Test: User interrupts AI mid-sentence
- [ ] Verify: AI stops talking within 100-200ms
- [ ] Verify: AI listens to new user input immediately
- [ ] Measure: Interruption detection latency
- [ ] Test: Multiple interruptions in succession

**Effort**: 2-3 hours
**Blocker**: Task 5.2 complete

**Milestone**: ✅ **Interruption working (barge-in functional)**

---

## Phase 6: Polish & Edge Cases

### Task 6.1: Session Timeout
- [ ] Implement 30s inactivity timeout in proxy
- [ ] Send "session.timeout" message to device
- [ ] Device returns to IDLE automatically
- [ ] Log timeout events for monitoring

**Effort**: 1-2 hours
**Blocker**: Phase 4 complete

---

### Task 6.2: Error Handling
- [ ] WebSocket disconnect during conversation → retry or return to IDLE
- [ ] OpenAI API error → send error to device, return to IDLE
- [ ] Audio glitches → log, continue (don't crash)
- [ ] Network timeout → reconnect logic

**Effort**: 2-3 hours
**Blocker**: Phase 5 complete

---

### Task 6.3: UI Feedback
- [ ] Update button label: "Start Conversation" / "End Conversation"
- [ ] Add visual indicator: "Listening" vs "Speaking"
- [ ] Optional: Show AI activity (thinking/responding)

**Effort**: 1-2 hours
**Blocker**: Phase 4 complete

---

### Task 6.4: End-to-End Testing
- [ ] Full conversation: 5+ turns without issues
- [ ] Test interruptions at various points
- [ ] Test edge cases: timeout, errors, network issues
- [ ] Performance test: latency, memory, CPU usage
- [ ] Stress test: Long conversations (100+ turns)

**Effort**: 3-4 hours
**Blocker**: All previous phases complete

**Milestone**: ✅ **Production ready**

---

## Phase 7: Documentation & Cleanup

### Task 7.1: Update Documentation
- [ ] Update `README.md` with continuous conversation mode
- [ ] Document WebSocket protocol (message format)
- [ ] Update `development-notes.md` with new architecture
- [ ] Add troubleshooting section (common issues)

**Effort**: 1-2 hours
**Blocker**: Phase 6 complete

---

### Task 7.2: Code Cleanup
- [ ] Remove old HTTP code paths (if fully migrated)
- [ ] Remove unused imports/functions
- [ ] Add code comments for complex sections
- [ ] Run linter/formatter

**Effort**: 1 hour
**Blocker**: Phase 6 complete

---

### Task 7.3: Performance Optimization (Optional)
- [ ] Profile: Identify bottlenecks
- [ ] Optimize: Ring buffer size, task priorities
- [ ] Tune: WebSocket buffer sizes, timeouts
- [ ] Measure: Final latency numbers

**Effort**: 2-3 hours
**Blocker**: Phase 6 complete

---

## Summary by Phase

| Phase | Description | Effort | Dependencies |
|-------|-------------|--------|--------------|
| **Phase 1** | Device WebSocket migration | 10-14 hours | None (START HERE) |
| **Phase 2** | Proxy WebSocket server | 7-9 hours | None (parallel) |
| **Phase 3** | Enable turn detection | 2-3 hours | None (proxy config) |
| **Phase 4** | Continuous conversation | 6-8 hours | Phase 1 + 2 |
| **Phase 5** | Interruption (barge-in) | 5-7 hours | Phase 4 |
| **Phase 6** | Polish & edge cases | 7-11 hours | Phase 5 |
| **Phase 7** | Documentation | 4-6 hours | Phase 6 |

**Total effort**: 41-58 hours (~1-1.5 weeks full-time)

---

## Critical Path

**Week 1**:
- Days 1-2: Phase 1 (Device WebSocket) + Phase 2 (Proxy WebSocket) in parallel
- Day 3: Phase 3 (Turn detection) + Testing
- Days 4-5: Phase 4 (Continuous conversation)

**Week 2**:
- Days 1-2: Phase 5 (Interruption)
- Day 3: Phase 6 (Polish)
- Day 4: Phase 7 (Documentation)
- Day 5: Buffer/testing

**Fast track**: If aggressive, can complete in 5-6 days by working phases 1+2 in parallel and skipping some polish.

---

## Success Metrics

- ✅ Single button press starts conversation
- ✅ AI detects speech completion automatically
- ✅ User can interrupt AI mid-response
- ✅ Latency: <500ms (speech stop → AI response start)
- ✅ Reliability: 95%+ success rate over 100 conversations
- ✅ No audio glitches or dropouts
- ✅ Handles 5+ turn conversations smoothly

---

## Quick Start

**To begin immediately**:
```bash
# 1. Add WebSocket dependency
cd smart_assistant_device
idf.py add-dependency "espressif/esp_websocket_client"

# 2. Study examples
cat ~/esp/v5.5.1/esp-idf/examples/protocols/http_server/ws_echo_server/main/ws_echo_server.c

# 3. Start Task 1.3: Create websocket_client.c
```

**First working prototype**: After Phase 1 + 2 (~2-3 days)
**Full continuous conversation**: After Phase 4 (~4-5 days)
**Production ready**: After Phase 6 (~1 week)
