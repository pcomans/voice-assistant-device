# Implementation Plan: Continuous Conversation with Turn Detection

**Status**: Proposed
**Target**: Single button press → dynamic conversation with interruption support

---

## What We're Implementing

**Goal**: Press button once → continuous conversation where AI listens, detects when you're done speaking, responds, and can be interrupted mid-sentence.

**Current**: Button press/release controls recording. Sequential: record → send → receive → play.

**Target**: Button press starts session. Simultaneous: continuously send mic + receive responses. AI detects turn completion and handles interruptions.

---

## How It Works Architecturally

### 1. Turn Detection (Server-Side)

**Enable semantic_vad** in OpenAI session config (proxy):
- AI detects speech completion based on semantic understanding of utterance
- More natural than silence-based detection (won't cut off mid-sentence)
- Automatically generates response when user finishes speaking

**Alternative**: `server_vad` with tuned silence thresholds if semantic detection too slow.

### 2. Bidirectional Audio Streaming

**Current flow** (sequential):
```
Device: Record → Stop → Send all chunks → Proxy → OpenAI
        ↓
Device: Wait for complete response → Play
```

**New flow** (simultaneous):
```
Device: Record continuously ──→ Stream chunks ──→ Proxy ──→ OpenAI (persistent WebSocket)
                                                      ↓
Device: Play responses ←────────── Stream PCM ←───── Proxy ←─ OpenAI (streaming response)
```

**Key change**: Recording and playback happen **simultaneously**, not sequentially.

### 3. Architecture Changes by Component

#### Device (ESP32)
- **Recording**: Keep mic streaming active continuously (don't stop on button release initially)
- **Playback**: Can start playing response while still recording
- **State machine**:
  - IDLE → Button press → CONVERSATION_ACTIVE
  - CONVERSATION_ACTIVE: Both recording and playback can be active
  - Button press again → End session, return to IDLE

#### Proxy (Python)
- **Enable turn detection**: Set `turn_detection: { type: "semantic_vad" }` in session config
- **Handle OpenAI events**:
  - `input_audio_buffer.speech_started`: User started speaking
  - `input_audio_buffer.speech_stopped`: User stopped speaking (turn complete)
  - `response.audio.delta`: Stream response audio to device
  - `response.audio.done`: Response complete
- **Session management**: Track active conversations, timeout after inactivity

#### OpenAI Realtime API (Existing)
- No changes needed - already supports all required features
- WebSocket remains persistent
- Turn detection and interruption handled server-side

### 4. Interruption (Barge-in)

**How it works**:
1. Device: User speaks while AI is talking
2. Proxy: Receives `input_audio_buffer.speech_started` event from OpenAI
3. Proxy: Sends "stop playback" signal to device
4. Device: Immediately stops I2S playback, clears ring buffer
5. Device: Keeps mic recording, new chunks go to OpenAI
6. OpenAI: Cancels current response, prepares for new user input
7. Cycle repeats when user stops speaking

**Critical**: Device must be able to:
- Stop playback instantly (don't wait for buffer drain)
- Clear playback ring buffer (drop queued audio)
- Keep recording throughout (don't stop mic on interruption)

### 5. Session Management

**Start session**: Single button press
- Device enters CONVERSATION_ACTIVE state
- Proxy starts/reuses OpenAI WebSocket session
- Mic starts streaming, playback ready

**End session**:
- **Option A**: Second button press (explicit end)
- **Option B**: Timeout after X seconds of silence (e.g., 30s)
- **Option C**: User says "goodbye" / "that's all" (semantic detection)

Recommend: **Option A + B** (button or timeout)

---

## Technical Requirements

### Device Changes
1. **New state**: CONVERSATION_ACTIVE (replaces RECORDING → SENDING → PLAYING sequence)
2. **Concurrent audio**: Record and play simultaneously using different I2S channels (already have I2S0 and I2S1)
3. **Playback control**: Add `audio_playback_stop_immediate()` function (clears buffer, stops I2S)
4. **HTTP protocol**: Add interruption signal from proxy (e.g., special byte sequence or separate control stream)

### Proxy Changes
1. **Session config**: Enable `turn_detection: { type: "semantic_vad" }`
2. **Event handling**: Listen for `input_audio_buffer.speech_started` → send stop signal to device
3. **Bidirectional multiplexing**: Send audio to device while receiving chunks from device (currently works, just need to keep both active)
4. **Session lifecycle**: Track session state, implement timeout logic

### Protocol Changes
**Current**: Device sends JSON chunks with `is_final: true/false`, receives binary PCM on final

**Option A - Keep HTTP**:
- Device keeps POSTing chunks to same session_id
- Proxy streams responses as they arrive (multiple responses per session)
- Add control channel (second HTTP connection or SSE) for interruption signals

**Option B - Upgrade to WebSocket**:
- Device maintains WebSocket to proxy
- Bidirectional binary frames: audio in both directions
- Control messages (start/stop/interrupt) as JSON frames
- More natural for bidirectional streaming

Recommend: **Option B (WebSocket)** - better fit for continuous bidirectional streaming

---

## Key Risks & Mitigations

**Risk 1**: Audio feedback loop (speaker output picked up by mic)
- Mitigation: Acoustic echo cancellation (OpenAI may handle this, needs testing)
- Mitigation: Add "push to talk" hold instead of toggle (optional fallback)

**Risk 2**: Network latency causes interruption lag
- Mitigation: Tune semantic_vad sensitivity
- Mitigation: Device-side audio detection for instant local stop (before proxy confirms)

**Risk 3**: Increased OpenAI costs (continuous listening)
- Mitigation: Implement timeout (30s silence → auto-end session)
- Mitigation: Monitor costs, adjust if needed

**Risk 4**: ESP32 cannot handle simultaneous record + playback
- Mitigation: We already have separate I2S channels (I2S0 playback, I2S1 capture) - should work
- Mitigation: Test early with simultaneous tasks

---

## Success Criteria

1. ✅ User presses button once → conversation starts
2. ✅ AI detects when user finishes speaking → responds automatically
3. ✅ User can interrupt AI mid-response → AI stops, listens to new input
4. ✅ Multiple back-and-forth turns without button presses
5. ✅ Session ends cleanly (button press or timeout)

---

## Architecture Comparison

### Current (Sequential)
```
[Press Button] → Record 3s → [Release Button] → Upload → AI thinks → Download → Play → [Done]
```
Latency: 176-426ms + recording time (user-dependent, 1-10s typical)

### Proposed (Continuous)
```
[Press Button] → ┌─ Record continuously ────┐
                  │                           │
                  ├─ Stream chunks to AI ────┤ → All simultaneous
                  │                           │
                  └─ Play responses ─────────┘

                  [Press Button] → End session
```
Latency: 176-426ms (same, but starts as soon as user stops speaking, no button release delay)

**Key improvement**: No waiting for button release. More natural conversation flow. Hands-free after initial button press.

---

## Dependencies

- OpenAI Realtime API: semantic_vad turn detection (already available)
- ESP32: Simultaneous I2S record + playback (hardware supports, needs testing)
- Network: WebSocket support on ESP32 (ESP-IDF has built-in support)

---

## Next Steps

1. Test semantic_vad turn detection on proxy side (change config, test with existing device)
2. Prototype simultaneous record + playback on device (separate test, verify no interference)
3. Implement WebSocket on device (ESP-IDF has `esp_websocket_client`)
4. Update proxy to handle bidirectional WebSocket + interruption events
5. Integration test: full continuous conversation
