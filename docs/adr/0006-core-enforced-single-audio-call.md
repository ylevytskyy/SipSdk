# 0006. The core enforces one audio-active call

- **Status**: Accepted
- **Date**: 2026-11-15

`sip_core` auto-holds the active call when the app answers, dials or resumes another, so at most one call has audio on every platform; if the hold re-INVITE fails, the core still stops local audio for the old call. We chose this over app-enforced hold so six platform bindings behave the same, at the cost of apps not being able to keep two calls live at once until the Phase 3 mixer changes this rule.
