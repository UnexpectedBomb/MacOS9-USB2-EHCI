#!/bin/sh
# regen.sh — regenerate libUSBServicesLibFull.a, a CFM import stub exposing the
# FULL USBServicesLib API (77 exports), including the UIM-registration calls
# USBAddBus / USBRemoveBus / StartRootHub / USBExpertSetStatusLevel / uslRefToBus
# that Apple never published (so the stock toolchain libUSBServicesLib.a only has
# the public USBGetVersion).
#
# Source of truth = the real USBServicesLib.pef carved out of your machine's Mac OS ROM
# (all 77 exports). We wrap it with a 'cfrg' resource naming the fragment
# "USBServicesLib" (so the imports bind to the live USL at runtime), then MakeImport
# turns the exports into linkable import stubs.
#
# NOTE: only the exported SYMBOL NAMES end up in the .a — MakeImport emits linkage stubs,
# not Apple's code. Carve the PEF yourself with Elliot Nunn's tbxi tools; it is not
# redistributed here.
#
# Prereq: Retro68 toolchain on PATH. Run from this directory.
# Set $USBSERVICESLIB_PEF to your carved USBServicesLib.pef.
set -e
TC="${RETRO68:-$HOME/Retro68-build/toolchain}"
PEF="${USBSERVICESLIB_PEF:?set USBSERVICESLIB_PEF to your carved USBServicesLib.pef}"
export PATH="$TC/bin:$PATH"

cp "$PEF" USBServicesLib
Rez usl_cfrg.r -I "$TC/RIncludes" --data USBServicesLib -o USBServicesLib.shlb -t 'shlb' -c '????'
MakeImport USBServicesLib.shlb libUSBServicesLibFull.a
rm -f USBServicesLib USBServicesLib.shlb

echo "wrote libUSBServicesLibFull.a:"
powerpc-apple-macos-nm libUSBServicesLibFull.a | grep -iE 'AddBus|RemoveBus|RootHub|ExpertSetStatus|RefToBus' || true
