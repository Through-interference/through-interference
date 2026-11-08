# CAN telemetry implementation plan

> Implement inline in the current chat. User explicitly said to proceed after authorizing preparation, flashing and hardware checks.

Goal: reliable bidirectional transparent CAN transport between the computer and airborne ESP32.

Spec: ../specs/2026-11-08-can-telemetry-design.md

Architecture: shared pure C++ wire codec and reliable batch endpoint, shared ESP32 controller with ground polling, TWAI only on air, line-framed USB on ground. Existing AES-GCM and NVS pairing retained.

Global constraints: one pair, GPIO4/17, Classical CAN, no external antenna switch, no key disclosure, no automatic actuator commands. Work in branch codex/can-telemetry in the user checkout; no extra worktree requested.

## Tasks

- [x] 1. Native protocol/endpoint: RED observed for absent implementation, GREEN native C++ codecs/queues/retries/session/stream checks. Tests run through unittest, 1 runner covering 500 frames in each direction and malformed inputs.
- [x] 2. ESP32 integration: both pinned PlatformIO builds passed, shared controller and real nonblocking TWAI/USB polling active. NVS layout/pairing retained, external secret header eliminated.
- [x] 3. Computer integration: 8 unittest runners passed including native C++, PTY serial and regression cases. CAN client/panel now handle actual CAN frames and CANable bench peer; fake flight commands rejected. Binary MAVLink v1/v2 verified on hardware in both directions.
- [x] 4. Both private backups verified, both images written and hash-verified, full CANable transfer and failure/restart scenarios passed. Final evidence in /Users/timurkh/Desktop/BAS/diagnostics/VALIDATION.md. Panel launched and owns the ground port; native UI button automation denied by Computer Use app permissions, so no claim of that visual check.

Review focus: malformed/truncated CAN/USB input; queue overflow without silent success; lost ACK and reboot without duplicate CAN delivery; missing CAN ACK/bus-off without radio stalls; real USB receive readiness and absence of feedback loops. Test these at their owning layer.

## Progress

Baseline: source tree clean, f454f47; no runnable firmware test suite and missing crypto_secrets.h. Prior hardware diagnostic proves CANable on air RX GPIO4/17 and one packet over old firmware. This known build gap is part of task 2, not a reason to stop authorized work.

Review rulings: receiver restart must not replay an uncertain command. Added an authenticated 64-bit destination boot session, handshake before payload, and explicit UNCERTAIN reporting when an in-flight batch's destination changes. This replaces the initial RAM-only restart behavior. Consequence: at most one active batch may be abandoned as uncertain; application-level command results remain the autopilot's responsibility.

Review fixes complete: reject duplicate crypto frames, avoid per-poll RX flash persistence using destination boot binding, reserve TX nonce blocks of 65536, and quarantine USB sends after an uncertain local queue-ACK timeout. New regression tests were observed failing for reboot replay and reused command channel before implementation.

Review fixes verified: 8/8 unittest runners green after RED cases. Both private 4 MiB flash backups verified by size, image headers and SHA-256. Both new firmware images written with esptool 4.5.1 at 115200, all written regions hash verified; first air write retried after serial interruption. Both boards retain pairing and now use saved GFSK250/500000 CAN settings. Native tests cover 400 frames in each direction.

Hardware evidence: ../diagnostics/hardware-can-v2.json has 37 frames each direction, Classical/extended/RTR/zero-length/binary/MAVLink v1/v2, simultaneous 24+24 burst in 0.467 s, no errors. ../diagnostics/recovery-can-v2.json passes radio outage retention, slow USB backpressure, 64 queued frames plus explicit rejection, receiver restart after actual CAN delivery before lost ACK with UNCERTAIN=1 and no replay, and successful new-session traffic. Final hardware stats show empty queues, LINK=UP; expected injected RF_TIMEOUT=12, QUEUE_REJECT=1, UNCERTAIN=1, CAN_FAIL/DROP/BUS_OFF=0.

Review minor rulings: CAN bitrate on RESTORE and separate software/hardware drop counters are deferred until hardware testing indicates relevance; not hidden by claims that all CAN configuration/overflow cases passed.

Finish ruling: retain the local feature branch and the user's checkout, with one coherent implementation commit after full bench validation. No remote push, merge or workspace deletion was requested. Review matters set aside (electrical RF conditions, actual autopilot protocol and forced bus-off) are explicitly listed in the validation report; the bench provides CAN/IRQ/link evidence only for the documented cases.
