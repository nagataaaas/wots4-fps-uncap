#pragma once
// Maps a copy of the decrypted exe into the test process (sections and base
// relocations only) and records what mod.dll asks of a fake ModLoader.
// No PE entry point, TLS, Steam imports, save, or game assets are executed.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <wots4/mod_api.h>

namespace mapped_exe {

inline unsigned imageBase;
inline unsigned va(unsigned address) { return imageBase+address-0x400000; }

inline unsigned tests, failures;
inline void check(const char* label, bool passed) {
    ++tests; if(!passed) ++failures;
    printf("%s %s\n",passed?"PASS":"FAIL",label);
}

inline bool mapImage(const char* path) {
    std::ifstream f(path,std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),{});
    if(bytes.size()<4096) return false;
    auto dos=(IMAGE_DOS_HEADER*)bytes.data();
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || (unsigned)dos->e_lfanew>bytes.size()-sizeof(IMAGE_NT_HEADERS32)) return false;
    auto nt=(IMAGE_NT_HEADERS32*)(bytes.data()+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.ImageBase!=0x400000) return false;
    auto base=(unsigned char*)VirtualAlloc(nullptr,nt->OptionalHeader.SizeOfImage,
        MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    if(!base) return false;
    imageBase=(unsigned)base;
    memcpy(base,bytes.data(),nt->OptionalHeader.SizeOfHeaders);
    auto sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        auto& s=sections[i];
        if((size_t)s.PointerToRawData+s.SizeOfRawData>bytes.size() ||
            (size_t)s.VirtualAddress+s.SizeOfRawData>nt->OptionalHeader.SizeOfImage) return false;
        memcpy(base+s.VirtualAddress,bytes.data()+s.PointerToRawData,s.SizeOfRawData);
    }
    auto reloc=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    for(unsigned pos=reloc.VirtualAddress;pos<reloc.VirtualAddress+reloc.Size;) {
        auto block=(IMAGE_BASE_RELOCATION*)(base+pos);
        if(block->SizeOfBlock<8 || pos+block->SizeOfBlock>nt->OptionalHeader.SizeOfImage) return false;
        auto entries=(WORD*)(block+1);
        for(unsigned i=0;i<(block->SizeOfBlock-8)/2;++i) {
            if((entries[i]>>12)==IMAGE_REL_BASED_HIGHLOW)
                *(unsigned*)(base+block->VirtualAddress+(entries[i]&0xfff))+=imageBase-0x400000;
            else if(entries[i]>>12) return false;
        }
        pos+=block->SizeOfBlock;
    }
    return true;
}

// What mod.dll asked of the fake ModLoader.
struct Claim { unsigned address, length; };
inline std::vector<Claim> claims;
inline unsigned releases;
inline std::string logged;
inline void WINAPI hostLog(HMODULE, const char* message) { logged+=message; logged+="\n"; printf("  log: %s\n",message); }
inline BOOL WINAPI hostClaim(HMODULE, uintptr_t address, uint32_t length) {
    claims.push_back({(unsigned)address,length}); return TRUE;
}
inline void WINAPI hostRelease(HMODULE) { ++releases; claims.clear(); }
inline bool claimed(unsigned address, unsigned length) {
    for(auto& c:claims)
        if(c.address==va(address)&&c.length==length) return true;
    return false;
}
inline bool loggedText(const char* text) { return logged.find(text)!=std::string::npos; }

}  // namespace mapped_exe
