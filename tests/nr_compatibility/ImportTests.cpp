#define NOMINMAX
#include "../../OptiScaler/dlssnr/DlssNr_RuntimeImports.h"
#include <cassert>
#include <cstdio>

using DlssNr::RuntimeImports::Slot;
int main()
{
    // Two layouts deliberately place the IAT somewhere other than the old hardcoded RVA.
    for (DWORD shift : { 0u, 0x300u })
    {
        std::vector<unsigned char> image(0x4000);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 0x80);
        nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        directory = { 0x400 + shift, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR) };
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data() + directory.VirtualAddress);
        descriptor->Name = 0x600 + shift;
        descriptor->OriginalFirstThunk = 0x800 + shift;
        descriptor->FirstThunk = 0xa00 + shift;
        memcpy(image.data() + descriptor->Name, "KERNEL32.dll", 13);
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + descriptor->OriginalFirstThunk);
        names[0].u1.AddressOfData = 0xc00 + shift;
        names[1].u1.AddressOfData = 0xc40 + shift;
        names[2].u1.Ordinal = IMAGE_ORDINAL_FLAG64 | 123;
        memcpy(image.data() + names[0].u1.AddressOfData + 2, "GetModuleFileNameW", 18);
        memcpy(image.data() + names[1].u1.AddressOfData + 2, "GetModuleFileNameA", 18);
        std::vector<Slot> slots;
        assert(DlssNr::RuntimeImports::Find(image, slots) && slots.size() == 2);
        assert(slots[0].address == reinterpret_cast<void**>(image.data() + descriptor->FirstThunk) && slots[0].wide);
        assert(slots[1].address == reinterpret_cast<void**>(image.data() + descriptor->FirstThunk + 8) && !slots[1].wide);
        // Malformed tables must fail without returning partial patch targets.
        const auto saved = names[1].u1.AddressOfData;
        names[1].u1.AddressOfData = image.size() - 1;
        assert(!DlssNr::RuntimeImports::Find(image, slots) && slots.empty());
        names[1].u1.AddressOfData = saved;
        directory.Size = sizeof(IMAGE_IMPORT_DESCRIPTOR);
        assert(!DlssNr::RuntimeImports::Find(image, slots) && slots.empty());
        directory.Size *= 2;
        descriptor->FirstThunk = (DWORD)image.size() - 4;
        assert(!DlssNr::RuntimeImports::Find(image, slots) && slots.empty());
        dos->e_lfanew = -1;
        assert(!DlssNr::RuntimeImports::Find(image, slots) && slots.empty());
    }
    puts("PASS: moved ANSI/Unicode imports, ordinal imports, and invalid table bounds");
}
