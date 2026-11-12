"""Bounded line assembly across serial read timeouts."""


class SerialLines:
    def __init__(self, limit=2048):
        self.limit = limit
        self.pending = bytearray()
        self.discarding = False

    def feed(self, data):
        lines = []
        parts = data.replace(b'\r', b'\n').split(b'\n')
        for index, part in enumerate(parts):
            if not self.discarding:
                if len(self.pending) + len(part) > self.limit:
                    self.pending.clear()
                    self.discarding = True
                else:
                    self.pending.extend(part)
            if index < len(parts) - 1:
                if not self.discarding:
                    line = self.pending.decode('ascii', errors='replace').strip()
                    if line:
                        lines.append(line)
                self.pending.clear()
                self.discarding = False
        return lines
