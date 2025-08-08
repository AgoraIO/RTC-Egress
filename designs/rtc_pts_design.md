# RTC Stream Recording A/V Synchronization Design

## Core Requirements
This design is based on RTC stream recording, where sometimes both audio and video are present, and other times only one of them is available. Additionally, frames may arrive with delays. How to design the time_base, PTS, and ensure smooth audio-video synchronization.

Handling various scenarios:
- Normal audio-video synchronization
- Video/audio-only recording
- Dynamic streams join/leave
- Network latency and jitter
- Frame loss and out-of-order delivery (though out-of-order is unlikely as it's handled by the underlying RTC)
