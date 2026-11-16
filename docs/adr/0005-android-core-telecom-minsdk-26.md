# 0005. Android system calling on Jetpack core-telecom, minSdk 26

- **Status**: Accepted
- **Date**: 2026-11-15

The Android SDK integrates with system call UI through Jetpack `androidx.core:core-telecom` (`CallsManager`, stable 1.0.x, wrapped by our own `SipTelecomManagerCoordinator`) instead of a raw self-managed `ConnectionService`, and `sipsdk` raises `minSdk` from 21 to 26 because `CallsManager` requires API 26. The cost is dropping Android 5.0–7.1 devices and depending on a young library; the wrapper interface keeps a switch to raw `ConnectionService` possible.
