import unittest
import uuid
from unittest.mock import Mock, patch

import can
from tools.can_client import CanFrame


def link(bitrate=500000, up=True):
    return {'flags': ['UP'] if up else [], 'linkinfo': {
        'info_kind': 'can', 'info_data': {'bittiming': {'bitrate': bitrate}, 'state': 'ERROR-ACTIVE'}}}


class SocketCanTests(unittest.TestCase):
    def test_setup_clears_active_listen_only_without_setting_unsupported_fd_flags(self):
        from tools.socketcan import configure
        initial = link()
        initial['linkinfo']['info_data']['ctrlmode'] = ['LISTEN-ONLY']
        with patch('tools.socketcan.read_link', side_effect=[initial, link()]), \
                patch('tools.socketcan.os.geteuid', return_value=1000), patch('subprocess.run') as run:
            configure('can0', 500000)
            settings = run.call_args_list[1].args[0]
            self.assertIn('listen-only', settings)
            self.assertEqual(settings[settings.index('listen-only') + 1], 'off')
            self.assertNotIn('fd', settings)

    def test_configuration_only_requests_privileges_when_needed(self):
        from tools.socketcan import configure
        with patch('tools.socketcan.read_link', return_value=link()), patch('subprocess.run') as run:
            configure('can0', 500000)
            run.assert_not_called()
        with patch('tools.socketcan.read_link', side_effect=[link(up=False), link()]), \
                patch('tools.socketcan.os.geteuid', return_value=1000), patch('subprocess.run') as run:
            configure('can0', 500000)
            self.assertEqual(len(run.call_args_list), 3)
            for call in run.call_args_list:
                self.assertEqual(call.args[0][:2], ['sudo', 'ip'])
            self.assertIn('500000', run.call_args_list[1].args[0])

    def test_linux_uses_socketcan_instead_of_detaching_kernel_usb(self):
        from tools.canable import Canable
        with patch('tools.canable.sys.platform', 'linux'), patch('tools.socketcan.SocketCanable') as backend:
            Canable(500000, channel='can1')
            backend.assert_called_once_with(500000, 'can1')

    def test_bidirectional_frames_without_local_echo(self):
        from tools.socketcan import SocketCanable
        name = 'can-test-' + uuid.uuid4().hex
        local = can.Bus(interface='virtual', channel=name)
        peer = can.Bus(interface='virtual', channel=name)
        factory = Mock(return_value=local)
        driver = SocketCanable(500000, 'can0', bus_factory=factory, link_reader=lambda _: link())
        try:
            self.assertEqual(factory.call_args.kwargs['interface'], 'socketcan')
            self.assertFalse(factory.call_args.kwargs['receive_own_messages'])
            for frame in [CanFrame(0x123, b'\x00\xff'), CanFrame(0x1ABCDE01, b'', True, True, 8), CanFrame(0x321)]:
                driver.send(frame)
                message = peer.recv(.5)
                self.assertEqual(message.arbitration_id, frame.can_id)
                self.assertEqual(message.dlc, frame.dlc)
                self.assertIsNone(driver.read(.01), 'Local TX must not count as reception')
                peer.send(message)
                self.assertEqual(driver.read(.5), frame)
        finally:
            driver.close()
            peer.shutdown()

    def test_wrong_bitrate_or_down_interface_is_rejected_before_open(self):
        from tools.socketcan import SocketCanable
        for state in [link(250000), link(up=False)]:
            factory = Mock()
            with self.subTest(state=state), self.assertRaises(RuntimeError):
                SocketCanable(500000, 'can0', bus_factory=factory, link_reader=lambda _: state)
            factory.assert_not_called()

    def test_local_tx_and_error_frames_do_not_appear_as_external_reception(self):
        from tools.socketcan import SocketCanable
        bus = Mock()
        bus.recv.side_effect = [can.Message(arbitration_id=0x123, is_rx=False),
                               can.Message(is_error_frame=True),
                               can.Message(arbitration_id=0x321, is_extended_id=False, data=[0, 1])]
        driver = SocketCanable(500000, 'can0', bus_factory=lambda **_: bus, link_reader=lambda _: link())
        try:
            self.assertEqual(driver.read(.5), CanFrame(0x321, b'\x00\x01'))
        finally:
            driver.close()
        bus.shutdown.assert_called_once()
