"""CANable gs_usb adapter, including unprivileged macOS initialization."""
import queue
import struct
import threading
from tools.can_client import CanFrame


class Canable:
    def __init__(self, bitrate=500000):
        from gs_usb.gs_usb import GsUsb
        from gs_usb.constants import GS_CAN_MODE_HW_TIMESTAMP
        devices = GsUsb.scan()
        if len(devices) != 1:
            raise RuntimeError(f'Expected one CANable, found {len(devices)}')
        self.dev = devices[0]
        self.stop = threading.Event()
        self.frames = queue.Queue(maxsize=4096)
        self.error = None
        self.dev.stop()
        if not self.dev.set_bitrate(bitrate):
            raise RuntimeError('Unsupported CAN bitrate')
        flags = GS_CAN_MODE_HW_TIMESTAMP & self.dev.device_capability.feature
        self.dev.device_flags = flags
        # Avoid the upstream macOS detach_kernel_driver path, which requires root.
        self.dev.gs_usb.ctrl_transfer(0x41, 2, 0, 0, struct.pack('<II', 1, flags))
        self.thread = threading.Thread(target=self._receive, daemon=True)
        self.thread.start()

    def _receive(self):
        from gs_usb.gs_usb_frame import GsUsbFrame
        from gs_usb.constants import GS_CAN_MODE_HW_TIMESTAMP
        import usb.core
        try:
            hw = bool(self.dev.device_flags & GS_CAN_MODE_HW_TIMESTAMP)
            while not self.stop.is_set():
                frame = GsUsbFrame()
                try:
                    data = self.dev.gs_usb.read(0x81, frame.__sizeof__(hw), 100)
                except usb.core.USBTimeoutError:
                    continue
                GsUsbFrame.unpack_into(frame, data, hw)
                if frame.echo_id != 0xFFFFFFFF or frame.is_error_frame:
                    continue
                self.frames.put_nowait(CanFrame(frame.arbitration_id,
                    b'' if frame.is_remote_frame else bytes(frame.data[:frame.can_dlc]),
                    frame.is_extended_id, frame.is_remote_frame, frame.can_dlc))
        except Exception as exc:
            self.error = exc

    def send(self, frame):
        from gs_usb.gs_usb_frame import GsUsbFrame
        from gs_usb.constants import CAN_EFF_FLAG, CAN_RTR_FLAG
        raw = GsUsbFrame(can_id=frame.can_id | (CAN_EFF_FLAG if frame.extended else 0) |
                        (CAN_RTR_FLAG if frame.remote else 0), data=list(frame.data))
        raw.can_dlc = frame.dlc
        self.dev.send(raw)

    def read(self, timeout=.1):
        if self.error:
            raise self.error
        try:
            return self.frames.get(timeout=max(0, timeout))
        except queue.Empty:
            return None

    def close(self):
        import usb.util
        self.stop.set()
        self.thread.join(2)
        self.dev.stop()
        usb.util.dispose_resources(self.dev.gs_usb)
