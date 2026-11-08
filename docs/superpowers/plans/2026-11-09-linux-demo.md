# Linux demonstration plan

User requires Linux as the deployment platform, macOS as development host, UART from GUI to ground ESP32 and Classical CAN on air. Linux CANable must work with SocketCAN and can-utils. Review precedes publishing.

1. Add SocketCAN transport with tests for frame identity, RTR, extended ID, no local echo and adapter configuration validation. Keep macOS gs_usb backend and release USB resources on all failures.
2. Add portable ground/air/bench launchers and a dependency bootstrap. Linux CAN configuration uses iproute2, requests sudo only when changing interface configuration, and leaves normal traffic unprivileged.
3. Make automatic demo correlate fresh per-run data, with regression against stale frames. Fix recovery preconditions/cleanup and provide safe fresh-pair provisioning.
4. Rewrite README Linux-first with hardware wiring, can-utils, one/two-computer scripts, GUI/UART, reproducible fresh checkout/setup and explicit first-time pairing.
5. Run clean Linux userspace tests, both firmware builds, actual Mac CAN/radio/USB bench checks, and independent final review. Docker kernel has no AF_CAN; explicitly report hardware/Linux-kernel validation limits.
6. Commit and push reviewed changes to feature branch; main requires pull request.
