Steinberg ASIO SDK (NOT redistributed in this repository)
==========================================================

The native ASIO audio backend (low-latency drivers such as RME Fireface USB or
Behringer UMC) is compiled in only when the Steinberg ASIO SDK headers are
present in THIS directory. The SDK's licensing agreement does not permit
checking the SDK into the repository, so everything here except this README is
gitignored.

How to set it up on a build machine:

  1. Download the SDK zip from Steinberg (free, no registration):
         https://www.steinberg.net/asiosdk
  2. Extract it, and copy the CONTENTS of the top-level "ASIOSDK" folder into
     this directory, so that this file exists:
         external_tools/asiosdk/common/iasiodrv.h
  3. Re-run the CMake configure step. The configure output prints either
         MiniDAWLab: ASIO backend ENABLED ...
     or an explanation of what is missing.

Without the SDK the app still builds and runs; the audio settings simply do not
offer the ASIO backend (Windows Audio / DirectSound remain available).

Notes:
  * Only SDK *headers* are used at compile time (JUCE's ASIO device type talks
    to the driver COM objects directly). No Steinberg code is linked from the
    SDK's host/ or driver/ sample folders.
  * User device drivers (RME, Behringer, ...) are never bundled; DAL uses the
    drivers already installed on the machine (HKLM\SOFTWARE\ASIO).
  * SDK version used during development: 2.3.4 (see changes.txt after download).
