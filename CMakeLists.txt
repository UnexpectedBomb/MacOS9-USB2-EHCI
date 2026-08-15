cmake_minimum_required(VERSION 3.13)
project(EHCIUIM C CXX)   # CXX needed: the CONSOLE runtime (RetroConsole) is C++

add_library(EHCIUIM SHARED src/ehci_uim.c src/ehci_hw.c src/ehci_os.c src/ehci_xfer.c src/ehci_vhub.c)
set_target_properties(EHCIUIM PROPERTIES
    LINK_FLAGS -Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/src/ehci_uim.exp)

# CFM import libraries the OS glue calls into (all present in the toolchain).
target_link_libraries(EHCIUIM
    # DriverLoaderLib supplies InstallDriverFromMemory: the driver installs its own block driver.
    PCILib DriverServicesLib NameRegistryLib InterfaceLib DriverLoaderLib)

add_custom_command(OUTPUT EHCIUIM.pef
    COMMAND ${MAKE_PEF} libEHCIUIM.so -o EHCIUIM.pef
    # ROM path: point the fragment main at DoDriverIO so the Device Manager loads us from the
    # ROM parcel (Retro68 leaves main unset). patch-pef-main.py only rewrites the loader main
    # offset; DoDriverIO / TheDriverDescription / ThePluginDispatchTable stay exported, so the
    # USL / LoadUIMForEntry path is unaffected.
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py EHCIUIM.pef DoDriverIO
    DEPENDS EHCIUIM ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py)

if (REZ_INCLUDE_PATH)
    set(REZ_INCLUDE_FLAG -I${REZ_INCLUDE_PATH})
endif()

# Wrap as a native driver: file type 'ndrv'.
add_custom_command(OUTPUT EHCIUIM.ndrv
    COMMAND ${REZ} ${REZ_FLAGS} ${CMAKE_CURRENT_SOURCE_DIR}/ehci_uim.r
            ${REZ_INCLUDE_FLAG}
            -o EHCIUIM.ndrv --cc EHCIUIM.bin
            -t "ndrv" -c "????"
            --data EHCIUIM.pef
    DEPENDS EHCIUIM.pef)

add_custom_target(ndrv ALL DEPENDS EHCIUIM.ndrv)

# ---- BYPASS m2: standalone USB block-storage driver PEF. Installed by EHCITrigger via
#      InstallDriverFromMemory after the UIM publishes the 'Eusb' block-read service. It
#      scans the MBR, AddDrive's the FAT partition, and serves File-Mgr reads through the
#      service. main == DoDriverIO (patch-pef-main.py), like a real native driver. ----
add_library(USBDisk SHARED src/usb_disk.c)
set_target_properties(USBDisk PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/src/usb_disk.exp")
target_link_libraries(USBDisk DriverServicesLib NameRegistryLib InterfaceLib)
add_custom_command(OUTPUT USBDisk.pef
    COMMAND ${MAKE_PEF} libUSBDisk.so -o USBDisk.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py USBDisk.pef DoDriverIO
    # Regenerate the embedded-blob header IN THE SAME RULE, so an edit to usb_disk.c can never ship a
    # stale blob inside the UIM (the UIM #includes usb_disk_blob.h; a manual regeneration step was
    # forgotten twice and cost a hardware run each time). usb_disk.c -> USBDisk.pef -> usb_disk_blob.h
    # -> EHCIUIM now propagates automatically via the add_dependencies below.
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pef-to-blob.py USBDisk.pef ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_disk_blob.h gUsbDiskPef
    DEPENDS USBDisk ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pef-to-blob.py)
add_custom_target(blockdrv ALL DEPENDS USBDisk.pef)   # NB: target name must differ from lib 'USBDisk' case-insensitively (macOS FS)
add_dependencies(EHCIUIM blockdrv)   # the UIM embeds the block driver: build order matters


# ---- USL-4.2: resident-driver trigger — call the Expert's LoadUIMForEntry on the EHCI node
#      so it loads EHCIUIM.ndrv (in Extensions) as the UIM plugin + builds the Name Registry. ----
add_application(EHCITrigger probe/ehci_trigger.c CONSOLE)
target_include_directories(EHCITrigger PRIVATE src)
target_link_libraries(EHCITrigger
    NameRegistryLib PCILib DriverServicesLib DriverLoaderLib InterfaceLib
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBServicesLibFull.a
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBFamilyExpertFull.a)

# ---- The shippable app: a single UNIVERSAL, prompt-driven launcher. Claims the EHCI ports, then
#      prompts "INSERT USB DRIVE NOW" and mounts the freshly inserted drive at USB 2.0 (with the
#      custom volume icon). One binary serves BOTH machine types: the driver's per-port claim
#      self-adapts — on a PCI card every port is empty at claim time so it claims them all; on an
#      on-board controller (Mac Mini G4) it releases the ports holding the keyboard/mouse back to
#      the 1.1 companion and claims only the free ones, and the ISR then chains the companion
#      handler (the runtime sharedCompanion discriminator in ehci_vhub.c). The wording is neutral
#      ("any free port"), so no per-machine build variant is needed. Same load path as EHCITrigger,
#      without the diagnostic harness. ----
add_application(EHCILauncher probe/ehci_launcher.c CONSOLE)
target_include_directories(EHCILauncher PRIVATE src)
target_link_libraries(EHCILauncher
    NameRegistryLib PCILib DriverServicesLib DriverLoaderLib InterfaceLib
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBServicesLibFull.a
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBFamilyExpertFull.a)

# ---- EHCIActivate: THE SHIPPING HELPER. Faceless: it activates the ROM driver with
#      LoadUIMForEntry, hands the front back to the Finder (so the Finder, not us, receives the
#      disk-inserted event), hides its window and then just pumps the polled service slot that
#      device discovery runs on. Mounting is done by the driver's own block driver plus the OS.
#      ⚠ The .r supplies a SIZE resource with canBackground set. Without it the Process Manager
#      stops scheduling this app once it hands the front to the Finder, which silently kills the
#      discovery pump — and the symptom is misleading, because selecting the app from the
#      Application menu resumes it and it then looks perfectly responsive. ----
add_application(EHCIActivate probe/ehci_activate.c probe/ehci_activate.r CONSOLE)
target_include_directories(EHCIActivate PRIVATE src)
target_link_libraries(EHCIActivate
    NameRegistryLib PCILib DriverServicesLib DriverLoaderLib InterfaceLib
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBServicesLibFull.a
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBFamilyExpertFull.a)
