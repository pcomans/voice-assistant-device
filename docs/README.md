# Device Firmware Documentation

This folder contains technical documentation for the ESP32-S3 voice assistant device firmware.

## Documents

### `development-notes.md`
Hardware setup, I2S configuration, and audio implementation details.

**Contents:**
- ESP32-S3-Touch-LCD-1.85 hardware overview
- I2S audio capture (MEMS microphone) configuration
- I2S playback (PCM5101 DAC) setup
- Critical GPIO pins and settings
- Audio data conversion requirements
- Working code examples and references

**Status:** Active reference - update as implementation evolves

### `implementation-plan-webrtc.md`
Future architecture plan for WebRTC-based real-time streaming.

**Contents:**
- WebRTC architecture design
- Latency projections (<200ms)
- Component breakdown
- Implementation phases
- Risk assessment

**Status:** Future consideration - not currently implemented. Current system uses HTTP streaming (Option C) which provides excellent latency (176-426ms) with simpler implementation.

### `opus_implementation.md`
Comprehensive guide for implementing Opus audio codec on ESP32.

**Contents:**
- Self-delimited packet format (RFC 6716 Appendix B)
- ESP32 decoder implementation with ESP Audio Codec library
- Python encoder implementation
- Common pitfalls and solutions
- Sample rate matching
- Memory management
- Complete working examples

**Status:** Reference - preserved as Opus codec implementation knowledge. Currently not in use as we stream raw PCM for simplicity, but this hard-won knowledge is valuable if we need compression in the future.

## Related Documentation

- **Proxy API**: See `smart_assistant_proxy/README.md`
- **Archived Decisions**: See `../docs/archived-decisions/`
