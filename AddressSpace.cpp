#include "StdAfx.h"
#include "FileIo.hpp"
#include "PE.hpp"
#include "Moneta.hpp"
#include "Interface.hpp"

using namespace std;
using namespace PeFile;
using namespace Moneta;

// 1 = suspicious memory
// 2 = process info regardless of whether it contains suspicious memory
// 3 = all artifacts from enumerated processes
// 4 - status output while parsing memory for processes

AddressSpace::~AddressSpace() {
	//Interface::Log("AddressSpace destructor\r\n");
	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		if (Itr->second->Type() == EntityType::PE_FILE) {
			delete dynamic_cast<PeVm::Body*>(Itr->second); // This will call the destructors for PE, mapped file and entity all to be called in inheritted order.
		}
		else if (Itr->second->Type() == EntityType::MAPPED_FILE) {
			delete dynamic_cast<MappedFile*>(Itr->second);
		}
		else {
			delete dynamic_cast<Base*>(Itr->second);;
		}
	}
	//Interface::Log("AddressSpace destructor2\r\n");
}

Process::~Process() {
	if (this->Handle != nullptr) {
		CloseHandle(this->Handle);
	}
}

HANDLE Process::GetHandle() {
	return this->Handle;
}

BOOL Process::IsWow64() {
	return this->Wow64;
}

typedef BOOL(WINAPI* ISWOW64PROCESS) (HANDLE, PBOOL);

Process::Process(uint32_t dwPid, const wchar_t* pProcessName) : Pid(dwPid), Name(pProcessName) {
	//
	// Initialize a new entity for each allocation base and add it to this process address space map
	//

	Interface::Log(4, "* Mapping address space of PID %d\r\n", this->Pid);
	this->Handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, dwPid);

	if (this->Handle != nullptr) {
		static ISWOW64PROCESS IsWow64Process = (ISWOW64PROCESS)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "IsWow64Process");

		if (IsWow64Process != nullptr) {
			BOOL bSelfWow64 = FALSE;

			if (IsWow64Process(GetCurrentProcess(), (PBOOL)&bSelfWow64)) {
				if (IsWow64Process(this->Handle, (PBOOL)&this->Wow64)) {
					if (this->IsWow64()) {
						//CloseHandle(this->Handle);
						Interface::Log(4, "* PID %d is Wow64\r\n", this->Pid);
						//system("pause");
						//throw 2;
					}
					else {
						if (bSelfWow64) {
							Interface::Log(4, "* Cannot scan non-Wow64 process from Wow64 Moneta instance\r\n");
							throw 2;
						}
					}
				}
			}
		}

		Interface::Log(4, "* Scanning sblocks...\r\n");
		//system("pause");
		SIZE_T cbRegionSize = 0;
		vector<MemoryBlock*> SBlocks;
		vector<MemoryBlock*>::iterator ABlock;
		//Entity* CurrentEntity = nullptr;

		//Loop memory, building list of SBlocks. Once a block is found which does not match the "current" allocation base, create a new entity containing the corresponding sblock list, and insert it into the address space entities map using the ablock as the key.

		//if (this->Pid == 3272) system("pause");

		for (uint8_t* pBaseAddr = nullptr;; pBaseAddr += cbRegionSize) {
			MEMORY_BASIC_INFORMATION* pBasicInfo = new MEMORY_BASIC_INFORMATION;

			if (VirtualQueryEx(this->Handle, pBaseAddr, (MEMORY_BASIC_INFORMATION*)pBasicInfo, sizeof(MEMORY_BASIC_INFORMATION)) == sizeof(MEMORY_BASIC_INFORMATION)) {
				cbRegionSize = pBasicInfo->RegionSize;
				
				if (!SBlocks.empty()) { // If the sblock list is empty then there is no ablock for comparison
					//
					// In the event that this is a new ablock, create a map pair and insert it into the entities map
					//
					
					Interface::Log(5, "Sblock list not empty\r\n");
					
					if (pBasicInfo->AllocationBase != (*ABlock)->GetBasic()->AllocationBase) {
						Interface::Log(5, "Found a new ablock. Saving sblock list to new entity entry.");
						//CurrentEntity->SetSBlocks(SBlocks);
						this->Entities.insert(make_pair((uint8_t*)(*ABlock)->GetBasic()->AllocationBase, Entity::Create(this->Handle, SBlocks)));
						SBlocks.clear();
					}
					//Interface::Log("done2\r\n");
				}
				
				Interface::Log(5, "Adding mew sblock to list\r\n");
				SBlocks.push_back(new MemoryBlock((MEMORY_BASIC_INFORMATION*)pBasicInfo, nullptr));
				ABlock = SBlocks.begin(); // This DOES fix a bug.
			}
			else {
				Interface::Log(5, "VirtualQuery failed\r\n");
				//system("pause");
				delete pBasicInfo;
				if (!SBlocks.empty()) { // Edge case: new ablock not yet found but finished enumerating sblocks.
					this->Entities.insert(make_pair((uint8_t*)(*ABlock)->GetBasic()->AllocationBase, Entity::Create(this->Handle, SBlocks)));
				}
				//Interface::Log("done\r\n");
				break;
			}
		}

		//this->PermissionRecords = new MemoryPermissionRecord(this->Blocks); // Initialize 
		//CloseHandle(hProcess);
	}
	else {
		Interface::Log(4, "- Failed to open handle to process\r\n");
		throw 1; // Not throwing a specific value crashes it
	}
}


uint32_t Process::GetPid() {
	return this->Pid;
}

bool PageExecutable(uint32_t dwProtect) {
	return (dwProtect == PAGE_EXECUTE || dwProtect == PAGE_EXECUTE_READ || dwProtect == PAGE_EXECUTE_READWRITE);
}
void Process::Enumerate() {
	Interface::Log(2, "[ %ws : %d : %ws\r\n", this->Name.c_str(), this->GetPid(), this->IsWow64() ? L"Wow64" : L"x64");
	//
	// Walk ablocks (entities) and list the corresponding sblocks of each.
	//

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		if (Itr->second->Type() == EntityType::PE_FILE) {
			Interface::Log(3, "[ 0x%016x:0x%08x | Image | %ws\r\n", dynamic_cast<PeVm::Body*>(Itr->second)->GetPeBase(), dynamic_cast<PeVm::Body*>(Itr->second)->GetPe()->GetImageSize(), dynamic_cast<PeVm::Body*>(Itr->second)->GetFilePath().c_str());
			//Interface::Log("File path: %ws (%ws)\r\n", ((Moneta::PE *)Itr->second)->GetFilePath().c_str(), dynamic_cast<PeVm::Body *>(Itr->second)->GetPe()->GetPeMagic() == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? L"64-bit" : L"32-bit");

			vector<PeVm::Section*> Sections = dynamic_cast<PeVm::Body*>(Itr->second)->GetSections();
			for (vector<PeVm::Section*>::const_iterator SectItr = Sections.begin(); SectItr != Sections.end(); ++SectItr) {
				vector<MemoryBlock*> SBlocks = (*SectItr)->GetSBlocks();

				for (vector<MemoryBlock*>::iterator SBlockItr = SBlocks.begin(); SBlockItr != SBlocks.end(); ++SBlockItr) {
					Interface::Log(3, "  0x%p:0x%08x | %s | %s | 0x%08x\r\n", (*SBlockItr)->GetBasic()->BaseAddress, (*SBlockItr)->GetBasic()->RegionSize, Moneta::PermissionSymbol((*SBlockItr)->GetBasic()->Protect), (*SectItr)->GetHeader()->Name,
						Moneta::GetPrivateSize(this->GetHandle(), static_cast<uint8_t*>((*SBlockItr)->GetBasic()->BaseAddress), (uint32_t)(*SBlockItr)->GetBasic()->RegionSize)
					);
					/*
					if (((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE) && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SBlockItr)->GetBasic()->BaseAddress, (uint32_t)(*SBlockItr)->GetBasic()->RegionSize)) {
						Interface::Log("! Section %s is executable and has private pages within %ws [%ws:%d]\r\n", (*SectItr)->GetHeader()->Name, dynamic_cast<PeVm::Body*>(Itr->second)->GetFilePath().c_str(), this->Name.c_str(), this->Pid);
						//system("pause");
					}*/

					if (strcmp(reinterpret_cast<const char*>((*SectItr)->GetHeader()->Name), "Headers") == 0 && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SBlockItr)->GetBasic()->BaseAddress, (uint32_t)(*SBlockItr)->GetBasic()->RegionSize)) {
						Interface::Log("! PE headers have private pages within %ws [%ws:%d]\r\n", dynamic_cast<PeVm::Body*>(Itr->second)->GetFilePath().c_str(), this->Name.c_str(), this->Pid);
						//system("pause");
					}

					if (PageExecutable((*SBlockItr)->GetBasic()->Protect) && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SBlockItr)->GetBasic()->BaseAddress, (uint32_t)(*SBlockItr)->GetBasic()->RegionSize)) {
						Interface::Log("! Sblock in section %s is executable and has private pages within %ws - %ws PE on disk [%ws:%d]\r\n", 
							(*SectItr)->GetHeader()->Name, dynamic_cast<PeVm::Body*>(Itr->second)->GetFilePath().c_str(),
							((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE) ? L"matches" : L"does not match",
							this->Name.c_str(), this->Pid);
						//system("pause");
					}
				}
			}
		}
		else if (Itr->second->Type() == EntityType::MAPPED_FILE) {
			vector<MemoryBlock*> SBlocks = Itr->second->GetSBlocks(); // This must be done explicitly, otherwise each time GetSBlocks is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.

			Interface::Log(3, "[ 0x%016x:0x%08x | Mapped | %ws\r\n", SBlocks.front()->GetBasic()->AllocationBase, SBlocks.front()->GetBasic()->RegionSize, dynamic_cast<MappedFile*>(Itr->second)->GetFilePath().c_str());
			//Interface::Log("S-Blocks:\r\n");

			for (vector<MemoryBlock*>::iterator SBlockItr = SBlocks.begin(); SBlockItr != SBlocks.end(); ++SBlockItr) {
				//Interface::Log("  0x%p\r\n", (*SBlockItr)->GetBasic()->BaseAddress);
				Interface::Log(3, "  0x%p:0x%08x | %s\r\n", (*SBlockItr)->GetBasic()->BaseAddress, (*SBlockItr)->GetBasic()->RegionSize, Moneta::PermissionSymbol((*SBlockItr)->GetBasic()->Protect));
				if (PageExecutable((*SBlockItr)->GetBasic()->Protect)) {
					Interface::Log("! Mapped memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SBlockItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
					//system("pause");
				}
			}
		}
		else {
			//Interface::Log("S-Blocks:\r\n");

			vector<MemoryBlock*> SBlocks = Itr->second->GetSBlocks(); // This must be done explicitly, otherwise each time GetSBlocks is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.

			if (SBlocks.front()->GetBasic()->Type == MEM_PRIVATE) {
				Interface::Log(3, "[ 0x%016x:0x%08x | Private\r\n", SBlocks.front()->GetBasic()->AllocationBase, (uint32_t)((uint8_t *)SBlocks.back()->GetBasic()->BaseAddress - SBlocks.back()->GetBasic()->AllocationBase) + SBlocks.back()->GetBasic()->RegionSize);
				for (vector<MemoryBlock*>::iterator SBlockItr = SBlocks.begin(); SBlockItr != SBlocks.end(); ++SBlockItr) {
					Interface::Log(3, "  0x%p:0x%08x | %s\r\n", (*SBlockItr)->GetBasic()->BaseAddress, (*SBlockItr)->GetBasic()->RegionSize, Moneta::PermissionSymbol((*SBlockItr)->GetBasic()->Protect));

					if (PageExecutable((*SBlockItr)->GetBasic()->Protect)) {
						Interface::Log("! Private memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SBlockItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
						//system("pause");
					}
				}
			}
			else {
				Interface::Log(3, "! Unknown memory type at 0x%p\r\n", Itr->first);
			}
		}

		//Interface::Log(3, "\r\n");
	}

	Interface::Log(2, "\r\n");
	/*
	bool bFileRange = false, bImageRange = false;

	for (list<MemoryBlock*>::const_iterator RecordItr = this->SBlocks.begin(); RecordItr != this->SBlocks.end(); ++RecordItr) {
		uint32_t dwPrivateSize = 0;

		if ((*RecordItr)->GetBasic()->Type == MEM_MAPPED || (*RecordItr)->GetBasic()->Type == MEM_IMAGE) {
			dwPrivateSize = VmProcess::GetPrivateSize((uint8_t*)(*RecordItr)->GetBasic()->BaseAddress, (*RecordItr)->GetBasic()->RegionSize);
			if ((*RecordItr)->GetBasic()->AllocationBase == (*RecordItr)->GetBasic()->BaseAddress) {
				bFileRange = true;
				wchar_t FileName[MAX_PATH] = { 0 };
				HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, this->Pid);

				if (hProcess != nullptr) {
					if ((*RecordItr)->GetBasic()->Type == MEM_MAPPED) {
						if (GetMappedFileNameW(hProcess, (*RecordItr)->GetBasic()->AllocationBase, FileName, MAX_PATH)) {
							//Interface::Log("%ws\r\n", MappedFileName);
						}
						else {
							wcscpy_s(FileName, MAX_PATH, L"Page file");
						}
					}
					else {
						bImageRange = true;
						if (GetModuleFileNameExW(hProcess, (HMODULE)(*RecordItr)->GetBasic()->AllocationBase, FileName, MAX_PATH)) {
							//Interface::Log("%ws\r\n", ModuleFileName);
						}
						else {
							wcscpy_s(FileName, MAX_PATH, L"?");
							Interface::Log("!!!! FAILED to get module name\r\n");
						}
					}

					CloseHandle(hProcess);
				}
				else {
					wcscpy_s(FileName, MAX_PATH, L"Access denied");
				}

				Interface::Log("======[ 0x%p : %ws\r\n", (*RecordItr)->GetBasic()->BaseAddress, FileName);
			}
		}
		else {
			bFileRange = false;
			bImageRange = false;
			Interface::Log("======[ 0x%p\r\n", (*RecordItr)->GetBasic()->BaseAddress);
		}

		wchar_t Indent[40] = { 0 };

		wcscpy_s(Indent, 40, L"  ");

		if (bImageRange) {
			wcscat_s(Indent, 40, L"  ");
		}

		Interface::Log(
			"%wsAllocated base 0x%p\r\n"
			"%wsBase: 0x%p\r\n"
			"%wsSize: %zu\r\n",
			Indent,
			(*RecordItr)->GetBasic()->AllocationBase,
			Indent,
			(*RecordItr)->GetBasic()->BaseAddress,
			Indent,
			(*RecordItr)->GetBasic()->RegionSize);

		Interface::Log("%wsState: ", Indent);
		switch ((*RecordItr)->GetBasic()->State)
		{
		case MEM_COMMIT:
			Interface::Log("MEM_COMMIT\r\n");
			break;
		case MEM_RESERVE:
			Interface::Log("MEM_RESERVE\r\n");
			break;
		case MEM_FREE:
			Interface::Log("MEM_FREE\r\n");
			break;
		default:
			Interface::Log("?\r\n");
		}

		Interface::Log("%wsType: ", Indent);
		switch ((*RecordItr)->GetBasic()->Type)
		{
		case MEM_IMAGE:
			Interface::Log("MEM_IMAGE\r\n");
			break;
		case MEM_MAPPED:
			Interface::Log("MEM_MAPPED\r\n");
			break;
		case MEM_PRIVATE:
			Interface::Log("MEM_PRIVATE\r\n");
			break;
		default:
			Interface::Log("N/A\r\n");
		}

		Interface::Log("%wsPrivate size: %d\r\n", Indent, dwPrivateSize);
		Interface::Log("%wsCurrent permissions: 0x%08x\r\n", Indent, (*RecordItr)->GetBasic()->Protect);
		Interface::Log("%wsOriginal permissions: 0x%08x\r\n", Indent, (*RecordItr)->GetBasic()->AllocationProtect);

		if ((*RecordItr)->GetRegion() != nullptr) {
			Interface::Log("%wsRegion allocation base: 0x%p\r\n", Indent, (*RecordItr)->GetRegion()->AllocationBase);
			Interface::Log("%wsRegion size: %d\r\n", Indent, (*RecordItr)->GetRegion()->RegionSize);
			Interface::Log("%wsCommit size: %d\r\n", Indent, (*RecordItr)->GetRegion()->CommitSize);
			Interface::Log("%wsRegion type literal: 0x%08x\r\n", Indent, (*RecordItr)->GetRegion()->RegionType); // These fields do not match struct declaration, likely different across Windows versions. Ignore them for now.
		}

		Interface::Log("\r\n");
	}*/
}