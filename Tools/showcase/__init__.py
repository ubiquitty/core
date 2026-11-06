# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Ubiquitty feature-showcase package. Regular (non-namespace) package so that
# `from showcase import tui` resolves to this directory and is not shadowed by
# the sibling script module showcase.py (same name) when run as
# `python3 Tools/showcase/showcase.py` (which puts this dir on sys.path too).
