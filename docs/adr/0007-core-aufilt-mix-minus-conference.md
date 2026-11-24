# 0007. Build mix-minus audio conferencing as a `core/` `aufilt` plugin

- **Status**: Accepted
- **Date**: 2026-11-20

`sip_core` implements local audio conferencing (2..`max_calls` remote legs) as a portable C++20 Baresip `aufilt` plugin registered via `aufilt_register(baresip_aufiltl(), ...)` instead of enabling Baresip's optional `mixminus` module. This extends ADR 0006 from one audio-active call to one audio-active call or conference group while avoiding a full rebuild and release of `BaresipCore.xcframework` and the Android/Desktop/Windows static prebuilts, at the cost of owning PCM resampling and mix-minus buffering inside `core/` (unit-tested with Goertzel tone isolation).
