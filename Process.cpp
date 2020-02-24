#include "StdAfx.h"
#include "FileIo.hpp"
#include "PE.hpp"
#include "Moneta.hpp"
#include "Process.hpp"
#include "Blocks.hpp"
#include "Interface.hpp"
#include "MemDump.hpp"

using namespace std;
using namespace PeFile;
using namespace Moneta;

uint32_t Thread::GetTid() {
	return this->Id;
}

void* Thread::GetEntryPoint() {
	return this->StartAddress;
}

void Thread::SetEntryPoint(void* pStartAddress) {
	this->StartAddress = pStartAddress;
}
Thread::Thread(uint32_t dwTid) : Id(dwTid) {}
Thread::Thread(uint32_t dwTid, void *pStartAddress) : Id(dwTid), StartAddress(pStartAddress) {}

Process::~Process() {
	if (this->Handle != nullptr) {
		CloseHandle(this->Handle);
	}

	for (vector<Thread*>::const_iterator Itr = this->Threads.begin(); Itr != this->Threads.end(); ++Itr) {
		delete* Itr;
	}

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		if (Itr->second->Type() == EntityType::PE_FILE) {
			delete dynamic_cast<PeVm::Body*>(Itr->second); // This will call the destructors for PE, mapped file and entity all to be called in inheritted order.
		}
		else if (Itr->second->Type() == EntityType::MAPPED_FILE) {
			delete dynamic_cast<MappedFile*>(Itr->second);
		}
		else {
			delete dynamic_cast<ABlock*>(Itr->second);;
		}
	}
}

HANDLE Process::GetHandle() {
	return this->Handle;
}

BOOL Process::IsWow64() {
	return this->Wow64;
}

#define ThreadQuerySetWin32StartAddress 9

// 1 = suspicious memory
// 2 = process info regardless of whether it contains suspicious memory
// 3 = all artifacts from enumerated processes
// 4 - status output while parsing memory for processes

Process::Process(uint32_t dwPid) : Pid(dwPid) {
	//
	// Initialize a new entity for each allocation base and add it to this process address space map
	//

	this->Handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, dwPid);

	if (this->Handle != nullptr) {
		//
		//
		//

		HANDLE hThreadSnap = INVALID_HANDLE_VALUE;
		THREADENTRY32 ThreadEntry;

		if ((hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)) != INVALID_HANDLE_VALUE) {
			ThreadEntry.dwSize = sizeof(THREADENTRY32);

			if (Thread32First(hThreadSnap, &ThreadEntry)) {
				do
				{
					if (ThreadEntry.th32OwnerProcessID == this->Pid) {
						Thread* CurrentThread = new Thread(ThreadEntry.th32ThreadID);
						//Interface::Log("* TID: 0x%08x\r\n", CurrentThread.GetTid());
						HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, false, CurrentThread->GetTid()); // OpenThreadToken consistently failed even with impersonation (ERROR_NO_TOKEN). The idea was abandoned due to lack of relevance. Get-InjectedThread returns the user as SYSTEM even when it was a regular user which launched the remote thread.

						if (hThread != nullptr) {
							typedef NTSTATUS(NTAPI* NtQueryInformationThread_t) (HANDLE, THREADINFOCLASS, void*, uint32_t, uint32_t*);
							static NtQueryInformationThread_t NtQueryInformationThread = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
							void* pStartAddress = nullptr;
							HANDLE hDupThread = nullptr;

							if (DuplicateHandle(GetCurrentProcess(), hThread, GetCurrentProcess(), &hDupThread, THREAD_QUERY_INFORMATION, FALSE, 0)) { // Without duplicating this handle NtQueryInformationThread will consistently fail to query the start address.
								NTSTATUS NtStatus = NtQueryInformationThread(hDupThread, (THREADINFOCLASS)ThreadQuerySetWin32StartAddress, &pStartAddress, sizeof(pStartAddress), nullptr);

								if (NT_SUCCESS(NtStatus)) {
									CurrentThread->SetEntryPoint(pStartAddress);
									//Interface::Log("* Start address: 0x%p\r\n", CurrentThread.GetEntryPoint());
								}

								CloseHandle(hDupThread);
							}

							this->Threads.push_back(CurrentThread);
							CloseHandle(hThread);
						}
					}
				} while (Thread32Next(hThreadSnap, &ThreadEntry));
			}

			CloseHandle(hThreadSnap);
		}
		//system("pause");

		wchar_t ImageName[MAX_PATH + 1] = { 0 };

		if (GetModuleBaseNameW(this->Handle, nullptr, ImageName, MAX_PATH + 1)) {
			//if(GetProcessImageFileNameW(Handle, ImageFilePath, sizeof(ImageFilePath))) {
			this->Name = wstring(ImageName);
			Interface::Log(4, "* Mapping address space of PID %d [%ws]\r\n", this->Pid, this->Name.c_str());
			typedef BOOL(WINAPI* ISWOW64PROCESS) (HANDLE, PBOOL);
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
			MEMORY_BASIC_INFORMATION* pMbi = new MEMORY_BASIC_INFORMATION;

			if (VirtualQueryEx(this->Handle, pBaseAddr, (MEMORY_BASIC_INFORMATION*)pMbi, sizeof(MEMORY_BASIC_INFORMATION)) == sizeof(MEMORY_BASIC_INFORMATION)) {
				cbRegionSize = pMbi->RegionSize;

				if (!SBlocks.empty()) { // If the sblock list is empty then there is no ablock for comparison
					//
					// In the event that this is a new ablock, create a map pair and insert it into the entities map
					//

					Interface::Log(5, "Sblock list not empty\r\n");

					if (pMbi->AllocationBase != (*ABlock)->GetBasic()->AllocationBase) {
						Interface::Log(5, "Found a new ablock. Saving sblock list to new entity entry.\r\n");
						this->Entities.insert(make_pair((uint8_t*)(*ABlock)->GetBasic()->AllocationBase, Entity::Create(this->Handle, SBlocks)));
						SBlocks.clear();
					}
					//Interface::Log("done2\r\n");
				}

				Interface::Log(5, "Adding mew sblock to list\r\n");
				SBlocks.push_back(new MemoryBlock((MEMORY_BASIC_INFORMATION*)pMbi, nullptr, this->Threads));
				ABlock = SBlocks.begin(); // This DOES fix a bug.
			}
			else {
				Interface::Log(5, "VirtualQuery failed\r\n");
				//system("pause");
				delete pMbi;
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
		Interface::Log(4, "- Failed to open handle to PID %d\r\n", this->Pid);
		throw 1; // Not throwing a specific value crashes it
	}
}


uint32_t Process::GetPid() {
	return this->Pid;
}

bool PageExecutable(uint32_t dwProtect) {
	return (dwProtect == PAGE_EXECUTE || dwProtect == PAGE_EXECUTE_READ || dwProtect == PAGE_EXECUTE_READWRITE);
}

void AlignName(const wchar_t* pOriginalName, wchar_t* pAlignedName, int32_t nAlignTo) { // Make generic and move to interface?
	assert(nAlignTo >= 1);

	if (wcslen(pOriginalName)) {
		if (wcslen(pOriginalName) < nAlignTo) {
			wcsncpy_s(pAlignedName, (nAlignTo + 1), pOriginalName, nAlignTo);
			for (int32_t nX = wcslen(pAlignedName); nX < nAlignTo; nX++) {
				wcscat_s(pAlignedName, (nAlignTo + 1), L" ");
			}
		}
	}
	else {
		wcscpy_s(pAlignedName, (nAlignTo + 1), L" ");

		for (int32_t nX = 1; nX < nAlignTo; nX++) {
			wcscat_s(pAlignedName, (nAlignTo + 1), L" ");
		}
	}
}

/*

ArchWow64PathExpand

The purpose of this function is to receive an unformatted file path (which may
contain architecture folders or environment variables) and convert the two
ambiguous architecture directories to Wow64 if applicable.

1. Expand all environment variables.
2. Check whether the path begins with either of the ambiguous architecture
folders: C:\Windows\system32, C:\Program Files
3. If the path does not begin with an ambiguous arch folder return it as is.
4. If the path does begin with an ambiguous arch folder then convert it to
the Wow64 equivalent and return it.

Examples:

%programfiles%\example1\example.exe -> C:\Program Files (x86)\example1\example.exe
C:\Program Files (x86)\example2\example.exe -> C:\Program Files (x86)\example2\example.exe
C:\Program Files\example3\example.exe -> C:\Program Files (x86)\example3\example.exe
C:\Windows\system32\notepad.exe -> C:\Windows\syswow64\notepad.exe

*/

#define MAX_ENV_VAR_SIZE 32767

bool ArchWow64PathExpand(const wchar_t* pTargetFilePath, wchar_t* pOutputPath, size_t OutputPathLength) {
	bool bExpandedPath = false;
	uint64_t qwPathLength;
	wchar_t* pProgFilePath64, * pProgFilePathWow64;
	wchar_t SystemDirectory[MAX_PATH + 1] = { 0 }, SysWow64Directory[MAX_PATH + 1] = { 0 }, ExpandedTargetPath[MAX_PATH + 1] = { 0 };
	SYSTEM_INFO SystemInfo = { 0 };

	if (ExpandEnvironmentStringsW(pTargetFilePath, ExpandedTargetPath, MAX_PATH + 1)) {
		bExpandedPath = true;
		wcscpy_s(pOutputPath, OutputPathLength, ExpandedTargetPath);

		GetNativeSystemInfo(&SystemInfo); // Native version of this call works on both Wow64 and x64 as opposed to just x64 for GetSystemInfo. Works on XP+

		if (SystemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
			//
			// Resolve the 32 and 64-bit versions of the most problematic paths (Program Files, System32)
			//

			if ((qwPathLength = GetSystemWow64DirectoryW(SysWow64Directory, MAX_PATH + 1))) {
				if ((qwPathLength = GetSystemDirectoryW(SystemDirectory, MAX_PATH + 1))) {
					pProgFilePath64 = (wchar_t*)new uint8_t[MAX_ENV_VAR_SIZE]; // 32,767 is the maximum number of bytes an environment var can be, including the null terminator.

					if ((qwPathLength = GetEnvironmentVariableW(L"ProgramW6432", pProgFilePath64, MAX_ENV_VAR_SIZE))) {
						pProgFilePathWow64 = (wchar_t*)new uint8_t[MAX_ENV_VAR_SIZE]; // 32,767 is the maximum number of bytes an environment var can be, including the null terminator.

						if ((qwPathLength = GetEnvironmentVariableW(L"ProgramFiles(x86)", pProgFilePathWow64, MAX_ENV_VAR_SIZE))) {
							//
							// Is the target path within one of the two ambiguous architecture directories?
							//

							if (_wcsnicmp(pProgFilePathWow64, ExpandedTargetPath, wcslen(pProgFilePathWow64)) == 0) {
								// The target path is already within the Wow64 program files path. Do nothing.
							}
							else if (_wcsnicmp(SysWow64Directory, ExpandedTargetPath, wcslen(SysWow64Directory)) == 0) {
								// The target path is already within the Wow64 system path. Do nothing.
							}
							else if (_wcsnicmp(SystemDirectory, ExpandedTargetPath, wcslen(SystemDirectory)) == 0) {
								wcscpy_s(pOutputPath, OutputPathLength, SysWow64Directory);
								wcscat_s(pOutputPath, OutputPathLength, ExpandedTargetPath + wcslen(SystemDirectory));
							}
							else if (_wcsnicmp(pProgFilePath64, ExpandedTargetPath, wcslen(pProgFilePath64)) == 0) {
								wcscpy_s(pOutputPath, OutputPathLength, pProgFilePathWow64);
								wcscat_s(pOutputPath, OutputPathLength, ExpandedTargetPath + wcslen(pProgFilePath64));
							}
						}

						delete[] pProgFilePathWow64;
					}

					delete[] pProgFilePath64;
				}
			}
		}
	}

	return bExpandedPath;
}

/*
| YourPhone.exe : 4760 : x64
 \ 0x0000000090bb0000:0x00001000 | Mapped | Page File
  \  0x0000019390BB0000:0x00001000 | RW
 _|
 \ 0x0000000090bc0000:0x00004000 | Mapped | C:\Users\Developer\AppData\Local\Microsoft\Windows\Caches\cversions.3.db
  \  0x0000019390BC0000:0x00004000 | R
 _|
 \ 0x0000000090bd0000:0x0000d000 | Non-executable image | C:\Windows\System32\Windows.UI.Xaml.Resources.Common.dll | Signed
  \  0x0000019390BD0000:0x0000d000 | R     | Header   | 0x00000000
  |  0x0000019390BD0000:0x0000d000 | R     | .rdata   | 0x00000000
  |  0x0000019390BD0000:0x0000d000 | R     | .rsrc    | 0x00000000
 _|  
 \ 0x0000000061a20000:0x0023a000 | Executable image | C:\Windows\SysWOW64\msmpeg2vdec.dll | Signed
  \  0x0000000061A20000:0x00001000 | R     | Header   | 0x00000000
  |  0x0000000061A21000:0x00215000 | RX    | .text    | 0x00003000 | Modified code
  |  0x0000000061C36000:0x00001000 | RW    | .data    | 0x00001000
  |  0x0000000061C37000:0x00002000 | WC    | .data    | 0x00000000
  |  0x0000000061C39000:0x00004000 | RW    | .data    | 0x00004000
  |  0x0000000061C43000:0x00017000 | R     | .idata   | 0x00002000
  |  0x0000000061C43000:0x00017000 | R     | .didat   | 0x00002000
  |  0x0000000061C43000:0x00017000 | R     | .rsrc    | 0x00002000
  |  0x0000000061C43000:0x00017000 | R     | .reloc   | 0x00002000

| Something.exe : 314 : x64
 \ 0x0000000090be0000:0x00004000 | Mapped | C:\Windows\System32\en-US\windows.ui.xaml.dll.mui
  \  0x0000019390BE0000:0x00004000 | R
[ 0x0000000090bf0000:0x00001000 | Private
  0x0000019390BF0000:0x00001000 | RW
[ 0x0000000090c00000:0x00200000 | Private
  0x0000019390C00000:0x00049000 | RW
[ 0x00000000d7850000:0x00022000 | Executable image | C:\Windows\System32\WinMetadata\Windows.Storage.winmd | Signed | Missing PEB module
  0x000002D5D7850000:0x00022000 | R     | Header   | 0x00000000
  0x000002D5D7850000:0x00022000 | R     | .text    | 0x00000000
  0x000002D5D7850000:0x00022000 | R     | .rsrc    | 0x00000000
[ 0x00000000d7880000:0x00020000 | Private
  0x000002D5D7880000:0x00001000 | RW
  0x000002D5D7881000:0x00001000 | RSRV
  0x000002D5D7882000:0x00001000 | RW
  0x000002D5D7883000:0x00001000 | RSRV
  0x000002D5D7884000:0x00008000 | RW
  0x000002D5D788C000:0x00002000 | RSRV
  0x000002D5D788E000:0x0000c000 | RW
  0x000002D5D789A000:0x00002000 | RSRV
  0x000002D5D789C000:0x00004000 | RW
*/
void Process::Enumerate(uint64_t qwMemdmpOptFlags) {
	bool bShownProc = false;
	MemDump ProcDmp(this->Handle, this->Pid);
	wchar_t DumpFilePath[MAX_PATH + 1] = { 0 };
	wstring_convert<codecvt_utf8_utf16<wchar_t>> UnicodeConverter;

	//
	// Walk ablocks (entities) and list the corresponding sblocks of each.
	//

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		int32_t nSuspiciousObjCount = 0;

		if (Itr->second->Type() == EntityType::PE_FILE) {
			PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(Itr->second);

			if (!PeEntity->IsNonExecutableImage()) {
				if (!PeEntity->IsSigned()) {
					nSuspiciousObjCount++;
				}

				if (PeEntity->GetPe() != nullptr) {
					if (!PeEntity->GetPebModule().Exists()) {
						nSuspiciousObjCount++;
					}
					else {
						if (_wcsicmp(PeEntity->GetPebModule().GetPath().c_str(), PeEntity->GetPath().c_str()) != 0) { // Since the PEB module is queried by base address with GetModuleInfo/GetModuleFileNameExW rather than by name with GetModuleHandleEx, there may be a PEB link with a base address matching this image region but with a misleading name/path
							if (this->IsWow64()) { // This is an edge case in which in Wow64 a module may appear as C:\Windows\System32\kernel32.dll although the true path is C:\Windows\SysWOW64\kernel32.dll due to Wow64 FS redirection.
								wchar_t ReFormattedPath[MAX_PATH + 1] = { 0 };

								if (ArchWow64PathExpand(PeEntity->GetPebModule().GetPath().c_str(), ReFormattedPath, MAX_PATH + 1)) {
									if (_wcsicmp(ReFormattedPath, PeEntity->GetPath().c_str()) != 0) {
										nSuspiciousObjCount++;
									}
								}
							}
							else {
								nSuspiciousObjCount++;
							}
						}
					}

					vector<PeVm::Section*> Sections = PeEntity->GetSections();
					for (vector<PeVm::Section*>::const_iterator SectItr = Sections.begin(); SectItr != Sections.end(); ++SectItr) {
						vector<MemoryBlock*> SBlocks = (*SectItr)->GetSBlocks();

						for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
							//
							// Headers with private pages
							//

							if (strcmp(reinterpret_cast<const char*>((*SectItr)->GetHeader()->Name), "Header") == 0 && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, (uint32_t)(*SbItr)->GetBasic()->RegionSize)) {
								//Interface::Log("! PE headers have private pages within %ws [%ws:%d]\r\n", PeEntity->GetFilePath().c_str(), this->Name.c_str(), this->Pid);
								//system("pause");
								nSuspiciousObjCount++;
							}

							//
							// Executable regions within sections that are not marked as executable on disk. For example: data is +rw on disk but has +x sblock
							//

							if (PageExecutable((*SbItr)->GetBasic()->Protect) && !((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
								//Interface::Log("! Sblock in section %s has executable permissions inconsistent with its file on disk at %ws [%ws:%d]\r\n", (*SectItr)->GetHeader()->Name, PeEntity->GetFilePath().c_str(), this->Name.c_str(), this->Pid);
								//system("pause");
								nSuspiciousObjCount++;
							}

							//
							// Executable regions in memory with private pages. Whether their +x is consistent with their section on disk is examined as well.
							//

							if (PageExecutable((*SbItr)->GetBasic()->Protect) && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, (uint32_t)(*SbItr)->GetBasic()->RegionSize)) {
								//Interface::Log("! Sblock in section %s is executable and has private pages within %ws - %ws PE on disk [%ws:%d]\r\n", (*SectItr)->GetHeader()->Name, PeEntity->GetFilePath().c_str(), ((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE) ? L"matches" : L"does not match", this->Name.c_str(), this->Pid);
								//system("pause");
								nSuspiciousObjCount++;
							}
						}
					}
				}
				else {
					vector<MemoryBlock*> SBlocks = PeEntity->GetSBlocks();

					for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
						if (PageExecutable((*SbItr)->GetBasic()->Protect)) {
							//Interface::Log("! Phantom image memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SbItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
							//system("pause");
							nSuspiciousObjCount++;
						}
					}
				}
			}
		}
		else if (Itr->second->Type() == EntityType::MAPPED_FILE) {
			vector<MemoryBlock*> SBlocks = Itr->second->GetSBlocks(); // This must be done explicitly, otherwise each time GetSBlocks is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.
			for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
				if (PageExecutable((*SbItr)->GetBasic()->Protect)) {
					//Interface::Log("! Mapped memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SbItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
					//system("pause");
					nSuspiciousObjCount++;
				}
			}
		}
		else {
			//Interface::Log("S-Blocks:\r\n");

			vector<MemoryBlock*> SBlocks = Itr->second->GetSBlocks(); // This must be done explicitly, otherwise each time GetSBlocks is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.

			if (SBlocks.front()->GetBasic()->Type == MEM_PRIVATE) {
				for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
					if (PageExecutable((*SbItr)->GetBasic()->Protect)) {
						//Interface::Log("! Private memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SbItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
						//system("pause");
						nSuspiciousObjCount++;
					}
				}
			}
		}

		//
		// Retrospectively enumerate the entire entity if a suspicious sblock was found within it, or enumerate it simply based on verbosity level
		//

		if (Interface::GetVerbosity() >= 3 || (Interface::GetVerbosity() < 3 && nSuspiciousObjCount > 0)) {
			bool bTotalEntitySuspicion = false; // Indicates a full entity dump rather than an individual sblock regardless of "from-base" dump setting. For example for PEB unlinked modules.

			nSuspiciousObjCount = 0; // Suspicious object count must be re-calculated since it cannot be known if this entity enumeration is occuring due to verbosity level or genuine suspicion

			if (!bShownProc) {
				Interface::Log("[ %ws : %d : %ws\r\n", this->Name.c_str(), this->GetPid(), this->IsWow64() ? L"Wow64" : L"x64");
				bShownProc = true;
			}

			if (Itr->second->Type() == EntityType::PE_FILE) {
				PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(Itr->second);

				if (!PeEntity->IsSigned()) {
					nSuspiciousObjCount++;
					bTotalEntitySuspicion = true;
				}

				if (PeEntity->GetPe() != nullptr) {
					if (PeEntity->IsNonExecutableImage()) {
						Interface::Log("[ 0x%016x:0x%08x | Non-executable image | %ws", PeEntity->GetPeBase(), PeEntity->GetPe()->GetImageSize(), PeEntity->GetPath().c_str());
					}
					else {
						Interface::Log("[ 0x%016x:0x%08x | Executable image | %ws", PeEntity->GetPeBase(), PeEntity->GetPe()->GetImageSize(), PeEntity->GetPath().c_str());
					}

					if (!PeEntity->IsSigned()) {
						Interface::Log((WORD)FOREGROUND_RED, " | Unsigned");
					}
					else {
						Interface::Log(" | Signed");
					}

					//
					// Determine whether this image has a corresponding entry in the PEB, and whether or not this entry accurately reflects the mapped file it is associated with.
					//

					if (!PeEntity->IsNonExecutableImage()) {
						if (!PeEntity->GetPebModule().Exists()) { // The PEB module is queried by base address with GetModuleInfo/GetModuleFileNameExW rather than by name with GetModuleHandleEx
							Interface::Log((WORD)FOREGROUND_RED, " | Missing PEB module");
							nSuspiciousObjCount++;
							bTotalEntitySuspicion = true;
						}
						else {
							if (_wcsicmp(PeEntity->GetPebModule().GetPath().c_str(), PeEntity->GetPath().c_str()) != 0) { // Since the PEB module is queried by base address with GetModuleInfo/GetModuleFileNameExW rather than by name with GetModuleHandleEx, there may be a PEB link with a base address matching this image region but with a misleading name/path
								if (this->IsWow64()) { // This is an edge case in which in Wow64 a module may appear as C:\Windows\System32\kernel32.dll although the true path is C:\Windows\SysWOW64\kernel32.dll due to Wow64 FS redirection.
									wchar_t ReFormattedPath[MAX_PATH + 1] = { 0 };

									if (ArchWow64PathExpand(PeEntity->GetPebModule().GetPath().c_str(), ReFormattedPath, MAX_PATH + 1)) {
										//Interface::Log("* Translated %ws to %ws\r\n", PeEntity->GetPebModule().GetPath().c_str(), ReFormattedPath);

										if (_wcsicmp(ReFormattedPath, PeEntity->GetPath().c_str()) != 0) {
											Interface::Log((WORD)FOREGROUND_RED, " | Mismatching PEB module");
											nSuspiciousObjCount++;
											bTotalEntitySuspicion = true;
										}
									}
								}
								else {
									Interface::Log((WORD)FOREGROUND_RED, " | Mismatching PEB module");
									nSuspiciousObjCount++;
									bTotalEntitySuspicion = true;
								}
							}
						}
					}

					Interface::Log("\r\n");

					vector<PeVm::Section*> Sections = PeEntity->GetSections();
					for (vector<PeVm::Section*>::const_iterator SectItr = Sections.begin(); SectItr != Sections.end(); ++SectItr) {
						vector<MemoryBlock*> SBlocks = (*SectItr)->GetSBlocks();
						wchar_t AlignedSectName[9] = { 0 };
						char AnsiSectName[9];
						strncpy_s(AnsiSectName, 9, (char*)(*SectItr)->GetHeader()->Name, 8);
						wstring UnicodeSectName = UnicodeConverter.from_bytes(AnsiSectName);
						AlignName((const wchar_t*)UnicodeSectName.c_str(), AlignedSectName, 8);

						for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
							bool bSuspiciousSblock = false;
							wchar_t AlignedAttribDesc[6] = { 0 };

							AlignName(MemoryBlock::AttribDesc((*SbItr)->GetBasic()), AlignedAttribDesc, 5);

							Interface::Log("  0x%p:0x%08x | %ws | %ws | 0x%08x", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc, AlignedSectName,
								Moneta::GetPrivateSize(this->GetHandle(), static_cast<uint8_t*>((*SbItr)->GetBasic()->BaseAddress), (uint32_t)(*SbItr)->GetBasic()->RegionSize)
							);

							//
							// Headers with private pages
							//

							if (strcmp(reinterpret_cast<const char*>((*SectItr)->GetHeader()->Name), "Header") == 0 && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, (uint32_t)(*SbItr)->GetBasic()->RegionSize)) {
								//Interface::Log("! PE headers have private pages within %ws [%ws:%d]\r\n", PeEntity->GetPath().c_str(), this->Name.c_str(), this->Pid);
								Interface::Log((WORD)FOREGROUND_RED, " | Modified header");
								bSuspiciousSblock = true;
								nSuspiciousObjCount++;
								//system("pause");
							}

							//
							// Executable regions within sections that are not marked as executable on disk. For example: data is +rw on disk but has +x sblock
							//

							if (PageExecutable((*SbItr)->GetBasic()->Protect) && !((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
								//Interface::Log("! Sblock in section %s has executable permissions inconsistent with its file on disk at %ws [%ws:%d]\r\n",
								//	(*SectItr)->GetHeader()->Name, PeEntity->GetPath().c_str(),
								//	this->Name.c_str(), this->Pid);
								Interface::Log((WORD)FOREGROUND_RED, " | Inconsistent +x between disk and memory");
								bSuspiciousSblock = true;
								nSuspiciousObjCount++;
								//system("pause");
							}

							//
							// Executable regions in memory with private pages. Whether their +x is consistent with their section on disk is examined as well.
							//

							if (PageExecutable((*SbItr)->GetBasic()->Protect) && Moneta::GetPrivateSize(this->GetHandle(), (uint8_t*)(*SbItr)->GetBasic()->BaseAddress, (uint32_t)(*SbItr)->GetBasic()->RegionSize)) {
								//Interface::Log("! Sblock in section %s is executable and has private pages within %ws - %ws PE on disk [%ws:%d]\r\n",
								//	(*SectItr)->GetHeader()->Name, PeEntity->GetPath().c_str(),
								//	((*SectItr)->GetHeader()->Characteristics & IMAGE_SCN_MEM_EXECUTE) ? L"matches" : L"does not match",
								//	this->Name.c_str(), this->Pid);
								Interface::Log((WORD)FOREGROUND_RED, " | Modified code");
								bSuspiciousSblock = true;
								nSuspiciousObjCount++;
								//system("pause");
							}

							Interface::Log("\r\n");
							vector<Thread*> Threads = (*SbItr)->GetThreads();

							for (vector<Thread*>::iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
								Interface::Log("    Thread 0x%p [TID 0x%08x]\r\n", (*ThItr)->GetEntryPoint(), (*ThItr)->GetTid());
								if (PeEntity->IsNonExecutableImage()) {
									Interface::Log("    !! Thread in non-executable image!\r\n");
									system("pause");
								}
							}

							if (!(qwMemdmpOptFlags & MEMDMP_OPT_FLAG_FROM_BASE) && (qwMemdmpOptFlags & MEMDMP_OPT_FLAG_SUSPICIOUS) && bSuspiciousSblock) {
								if (ProcDmp.Create((*SbItr)->GetBasic(), DumpFilePath, MAX_PATH + 1)) {
									Interface::Log("      ~ Memory dumped to %ws\r\n", DumpFilePath);
								}
								else {
									Interface::Log("      ~ Memory dump failed.\r\n");
								}
							}
						}
					}
				}
				else {
					vector<MemoryBlock*> SBlocks = PeEntity->GetSBlocks();
					Interface::Log((WORD)FOREGROUND_RED, "[ 0x%016x:0x%08x | Image | %ws [Phantom]\r\n", PeEntity->GetStartVa(), PeEntity->GetEntitySize(), PeEntity->GetPath().c_str());

					for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
						bool bSuspiciousSblock = false;
						wchar_t AlignedAttribDesc[6] = { 0 };

						AlignName(MemoryBlock::AttribDesc((*SbItr)->GetBasic()), AlignedAttribDesc, 5);

						Interface::Log("  0x%p:0x%08x | %ws | 0x%08x", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc,
							Moneta::GetPrivateSize(this->GetHandle(), static_cast<uint8_t*>((*SbItr)->GetBasic()->BaseAddress), (uint32_t)(*SbItr)->GetBasic()->RegionSize));

						if (PageExecutable((*SbItr)->GetBasic()->Protect)) {
							//Interface::Log("! Phantom image memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SbItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
							Interface::Log((WORD)FOREGROUND_RED, " | Phantom +x image memory");
							bSuspiciousSblock = true;
							nSuspiciousObjCount++;
							//system("pause");
						}

						Interface::Log("\r\n");
						vector<Thread*> Threads = (*SbItr)->GetThreads();

						for (vector<Thread*>::iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
							Interface::Log("    Thread 0x%p [TID 0x%08x]\r\n", (*ThItr)->GetEntryPoint(), (*ThItr)->GetTid());
							system("pause");
						}

						if (!(qwMemdmpOptFlags & MEMDMP_OPT_FLAG_FROM_BASE) && (qwMemdmpOptFlags & MEMDMP_OPT_FLAG_SUSPICIOUS) && bSuspiciousSblock) {
							if (ProcDmp.Create((*SbItr)->GetBasic(), DumpFilePath, MAX_PATH + 1)) {
								Interface::Log("      ~ Memory dumped to %ws\r\n", DumpFilePath);
							}
							else {
								Interface::Log("      ~ Memory dump failed.\r\n");
							}
						}
					}
				}
			}
			else if (Itr->second->Type() == EntityType::MAPPED_FILE) {
				vector<MemoryBlock*> SBlocks = Itr->second->GetSBlocks(); // This must be done explicitly, otherwise each time GetSBlocks is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.

				Interface::Log("[ 0x%016x:0x%08x | Mapped | %ws\r\n", SBlocks.front()->GetBasic()->AllocationBase, SBlocks.front()->GetBasic()->RegionSize, dynamic_cast<MappedFile*>(Itr->second)->GetPath().c_str());
				//Interface::Log("S-Blocks:\r\n");

				for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
					bool bSuspiciousSblock = false;
					wchar_t AlignedAttribDesc[6] = { 0 };

					AlignName(MemoryBlock::AttribDesc((*SbItr)->GetBasic()), AlignedAttribDesc, 5);

					//Interface::Log("  0x%p\r\n", (*SbItr)->GetBasic()->BaseAddress);
					Interface::Log("  0x%p:0x%08x | %ws", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc);
					if (PageExecutable((*SbItr)->GetBasic()->Protect)) {
						//Interface::Log("! Mapped memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SbItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
						Interface::Log((WORD)FOREGROUND_RED, " | Abnormal executable mapped memory");
						//system("pause");
						bSuspiciousSblock = true;
						nSuspiciousObjCount++;
					}

					Interface::Log("\r\n");
					vector<Thread*> Threads = (*SbItr)->GetThreads();

					for (vector<Thread*>::iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
						Interface::Log("    Thread 0x%p [TID 0x%08x]\r\n", (*ThItr)->GetEntryPoint(), (*ThItr)->GetTid());
						system("pause");
					}

					if (!(qwMemdmpOptFlags & MEMDMP_OPT_FLAG_FROM_BASE) && (qwMemdmpOptFlags & MEMDMP_OPT_FLAG_SUSPICIOUS) && bSuspiciousSblock) {
						if (ProcDmp.Create((*SbItr)->GetBasic(), DumpFilePath, MAX_PATH + 1)) {
							Interface::Log("      ~ Memory dumped to %ws\r\n", DumpFilePath);
						}
						else {
							Interface::Log("      ~ Memory dump failed.\r\n");
						}
					}
				}
			}
			else {
				//Interface::Log("S-Blocks:\r\n");

				vector<MemoryBlock*> SBlocks = Itr->second->GetSBlocks(); // This must be done explicitly, otherwise each time GetSBlocks is called a temporary copy of the list is created and the begin/end iterators will become useless in identifying the end of the list, causing an exception as it loops out of bounds.

				if (SBlocks.front()->GetBasic()->Type == MEM_PRIVATE) {
					Interface::Log("[ 0x%016x:0x%08x | Private\r\n", SBlocks.front()->GetBasic()->AllocationBase, (uint32_t)((uint8_t*)SBlocks.back()->GetBasic()->BaseAddress - SBlocks.back()->GetBasic()->AllocationBase) + SBlocks.back()->GetBasic()->RegionSize);
					for (vector<MemoryBlock*>::iterator SbItr = SBlocks.begin(); SbItr != SBlocks.end(); ++SbItr) {
						bool bSuspiciousSblock = false;
						wchar_t AlignedAttribDesc[6] = { 0 };

						AlignName(MemoryBlock::AttribDesc((*SbItr)->GetBasic()), AlignedAttribDesc, 5);

						Interface::Log("  0x%p:0x%08x | %ws", (*SbItr)->GetBasic()->BaseAddress, (*SbItr)->GetBasic()->RegionSize, AlignedAttribDesc);
						if (PageExecutable((*SbItr)->GetBasic()->Protect)) {
							//Interface::Log("! Private memory at sblock 0x%p is executable [%ws:%d]\r\n", (*SbItr)->GetBasic()->BaseAddress, this->Name.c_str(), this->Pid);
							Interface::Log((WORD)FOREGROUND_RED, " | Abnormal executable private memory");
							//system("pause");
							bSuspiciousSblock = true;
							nSuspiciousObjCount++;
						}

						Interface::Log("\r\n");

						vector<Thread*> Threads = (*SbItr)->GetThreads();

						for (vector<Thread*>::iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
							Interface::Log("    Thread 0x%p [TID 0x%08x]\r\n", (*ThItr)->GetEntryPoint(), (*ThItr)->GetTid());
							system("pause");
						}

						if (!(qwMemdmpOptFlags & MEMDMP_OPT_FLAG_FROM_BASE) && (qwMemdmpOptFlags & MEMDMP_OPT_FLAG_SUSPICIOUS) && bSuspiciousSblock) {
							//printf("start va: 0x%p, size: 0x%08x\r\n", Itr->second->GetStartVa(), Itr->second->GetEntitySize());
							if (ProcDmp.Create((*SbItr)->GetBasic(), DumpFilePath, MAX_PATH + 1)) {
								Interface::Log("      ~ Memory dumped to %ws\r\n", DumpFilePath);
							}
							else {
								Interface::Log("      ~ Memory dump failed.\r\n");
							}
						}
					}
				}
				else {
					//Interface::Log("! Unknown memory type at 0x%p\r\n", Itr->first);
				}
			}

			if ((qwMemdmpOptFlags & MEMDMP_OPT_FLAG_SUSPICIOUS)) {
				if (nSuspiciousObjCount > 0 && (bTotalEntitySuspicion || (qwMemdmpOptFlags & MEMDMP_OPT_FLAG_FROM_BASE))) { // Suspicious object count must be re-calculated since it cannot be known if this entity enumeration is occuring due to verbosity level or genuine suspicion
					if (Entity::Dump(ProcDmp, *(Itr->second))) {
						Interface::Log("      ~ Generated full region dump\r\n");
					}
					else {
						Interface::Log("      ~ Failed to generate full region dump\r\n");
					}
				}
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