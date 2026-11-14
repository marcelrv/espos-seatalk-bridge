# SPDX-FileCopyrightText: 2026 Marcel Verpaalen
# SPDX-License-Identifier: Apache-2.0
# Dot-source in PowerShell to get idf.py for this project:
#     . .\tools\idf-env.ps1
# ESP-IDF v6.0.3 (espOS's pin, espos/.idf-version) lives in D:\Programming\esp,
# with its own tools folder so it doesn't mix with older IDF installs.
# Override with $env:ST_IDF_PATH / $env:ST_IDF_TOOLS_PATH on another machine.
$idf   = if ($env:ST_IDF_PATH)       { $env:ST_IDF_PATH }       else { 'D:\Programming\esp\idf-v6.0.3' }
$tools = if ($env:ST_IDF_TOOLS_PATH) { $env:ST_IDF_TOOLS_PATH } else { 'D:\Programming\esp\.espressif' }
$env:IDF_TOOLS_PATH = $tools
. (Join-Path $idf 'export.ps1')
