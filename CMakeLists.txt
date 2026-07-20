cmake_minimum_required(VERSION 3.13)
project(EHCIUIM C CXX)   # CXX needed: the CONSOLE runtime (RetroConsole) is C++

add_library(EHCIUIM SHARED src/ehci_uim.c src/ehci_hw.c src/ehci_os.c src/ehci_xfer.c src/ehci_vhub.c)
set_target_properties(EHCIUIM PROPERTIES
    LINK_FLAGS -Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/src/ehci_uim.exp)

# CFM import libraries the OS glue calls into (all present in the toolchain).
target_link_libraries(EHCIUIM
    PCILib DriverServicesLib NameRegistryLib InterfaceLib)

add_custom_command(OUTPUT EHCIUIM.pef
    COMMAND ${MAKE_PEF} libEHCIUIM.so -o EHCIUIM.pef
    DEPENDS EHCIUIM)

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
    DEPENDS USBDisk ${CMAKE_CURRENT_SOURCE_DIR}/patch-pef-main.py)
add_custom_target(blockdrv ALL DEPENDS USBDisk.pef)   # NB: target name must differ from lib 'USBDisk' case-insensitively (macOS FS)


# ---- USL-4.2: resident-driver trigger — call the Expert's LoadUIMForEntry on the EHCI node
#      so it loads EHCIUIM.ndrv (in Extensions) as the UIM plugin + builds the Name Registry. ----
add_application(EHCITrigger probe/ehci_trigger.c CONSOLE)
target_include_directories(EHCITrigger PRIVATE src)
target_link_libraries(EHCITrigger
    NameRegistryLib PCILib DriverServicesLib DriverLoaderLib InterfaceLib
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBServicesLibFull.a
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBFamilyExpertFull.a)

# ---- The shippable app: a visible, prompt-driven launcher. Claims the EHCI ports, then prompts
#      "INSERT USB DRIVE NOW" and mounts the freshly inserted drive at USB 2.0 (with the custom
#      volume icon). Same load path as EHCITrigger, without the diagnostic harness. ----
add_application(EHCILauncher probe/ehci_launcher.c CONSOLE)
target_include_directories(EHCILauncher PRIVATE src)
target_link_libraries(EHCILauncher
    NameRegistryLib PCILib DriverServicesLib DriverLoaderLib InterfaceLib
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBServicesLibFull.a
    ${CMAKE_CURRENT_SOURCE_DIR}/usl_import/libUSBFamilyExpertFull.a)
