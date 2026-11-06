# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""The wire version and fingerprint every Greet and Welcome opens with, read
from the header that defines them, so a fixture speaks the build it tests
instead of a number it retyped."""

import pathlib
import re

_HEADER = pathlib.Path(__file__).resolve().parents[2] / 'Source/Core/Protocol/Sync/Envelope.h'


def wire_version():
    """Ubiquitty::Protocol::Sync::wireVersion, as the source states it."""
    match = re.search(r'inline constexpr std::uint16_t wireVersion = (\d+);', _HEADER.read_text())
    if not match:
        raise RuntimeError('wireVersion is not stated in ' + str(_HEADER))
    return int(match.group(1))



def wire_fingerprint():
    """Ubiquitty::Protocol::Sync::wireFingerprint, as the source states it: the
    32 bytes a Greet or Welcome carries, which a peer of another build refuses."""
    match = re.search(r'wireFingerprint \{([^}]*)\}', _HEADER.read_text())
    values = [int(token, 16) for token in re.findall(r'0x([0-9a-fA-F]{2})', match.group(1))] if match else []
    if len(values) != 32:
        raise RuntimeError('wireFingerprint is not stated in ' + str(_HEADER))
    return bytes(values)
