#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
# Forwards to espOS's build wrapper (machine-wide lock, capped parallelism).
exec "$(dirname "$0")/../espos/scripts/build.sh" "$@"
