#include "stdafx.h"
#include "key_grabber_thread.h"

#include <tlhelp32.h>

#define PLEASE_TERMINATE 0
#define SCAN_WORKER_THREAD_COUNT 2

#define RECV_TESTED 0x1
#define SEND_TESTED 0x2

bool key_grabber_thread::insertKey(DWORD pid, DWORD *keyblobptr, DWORD foundAddress)
{

	KEYDATA *newKey = new KEYDATA;
	//blob dwords 1 and 4 are null bytes;
	newKey->salsakey[0] = (keyblobptr[9]);
	newKey->salsakey[1] = (keyblobptr[6]);
	newKey->salsakey[2] = (keyblobptr[3]);
	newKey->salsakey[3] = (keyblobptr[0]);
	newKey->salsakey[4] = (keyblobptr[11]);
	newKey->salsakey[5] = (keyblobptr[8]);
	newKey->salsakey[6] = (keyblobptr[5]);
	newKey->salsakey[7] = (keyblobptr[2]);

	if (newKey->salsakey[0] == 0 && newKey->salsakey[3] == 0 && newKey->salsakey[7] == 0)
		return false; //probably an old zero-ed out key

	newKey->IV[0] = (keyblobptr[10]);
	newKey->IV[1] = (keyblobptr[7]);
	newKey->timeFound = GetTickCount64();
	newKey->foundAddress = foundAddress;
	newKey->sourceProcess = pid;

	WaitForSingleObject(this->keyVecMutex, INFINITE);
	
	for (auto keyIt = unclaimedKeys.begin(); keyIt != unclaimedKeys.end(); ++keyIt)
	{
		//this key already added
		if (keyIt->key->foundAddress == foundAddress)
		{
			if (!memcmp(keyIt->key->IV, newKey->IV, SALSA_IV_SIZE))
			{
				//printf("Ignoring dupe\n");
				delete newKey;
				ReleaseMutex(keyVecMutex);
				return false;
			}
		}
	}

	std::stringstream resultMsg;
	resultMsg << "Found candidate key blob at address: " <<
		foundAddress;
	UIaddLogMsg(resultMsg.str(), pid, uiMsgQueue);

	std::cout << std::setfill('0') << "KEY: ";
	//outfile << std::setfill('0') << "KEY: ";
	for (size_t i = 0; i < 8; ++i) {
		DWORD item = newKey->salsakey[i];
		std::cout << std::hex << std::setw(8) << (DWORD)item << " ";
		//outfile << std::hex << std::setw(8) << (DWORD)item << " ";

	}
	std::cout << std::endl;
	//outfile << std::endl;


	std::cout << std::setfill('0') << "IV: ";
	//outfile << std::setfill('0') << "IV: ";
	for (size_t i = 0; i < 2; ++i) {
		DWORD item = newKey->IV[i];
		std::cout << std::hex << std::setw(8) << (DWORD)item;
		//outfile << std::hex << std::setw(8) << (DWORD)item;
	}
	std::cout << std::endl;
	//outfile << std::endl;

	//pushing FIFO means that old dud keys don't have to be tested by every thread
	//to get to the fresh keys
	UNCLAIMED_KEY newUnc(newKey);
	unclaimedKeys.push_front(newUnc);
	
	ReleaseMutex(this->keyVecMutex);
	return true;
}

bool key_grabber_thread::insertKey(KEYDATA *key)
{
	WaitForSingleObject(this->keyVecMutex, INFINITE);
	UNCLAIMED_KEY newUnc(key);
	unclaimedKeys.push_front(newUnc);
	ReleaseMutex(this->keyVecMutex);
	return true;
}

/*
the unique stream IDs make it easy to manage them but assocating multiple keys from
multiple clients with their appropriate stream is trickier

This offers an unclaimed key to streams that need one. If one half of a stream is
decrypted then we only need the IV for the other as the same salsa works 
which can be used as a hint to force returning only keyblobs with that key.
*/
KEYDATA * key_grabber_thread::getUnusedMemoryKey(unsigned int streamID, bool recvKey,
	KEYDATA *hintKey)
{
	KEYDATA *chosenCandidate = NULL;

	DWORD waitResult = WaitForSingleObject(this->keyVecMutex, 200);
	if (waitResult)
		return NULL;

	for (auto keyit = unclaimedKeys.begin(); keyit != unclaimedKeys.end(); keyit++)
	{
		//find stream/mask pairs in streamsTests where stream==streamID
		auto streamIt = std::find_if(keyit->streamsTested.begin(), 
									 keyit->streamsTested.end(), 
						[&streamID](const std::pair<unsigned int, byte>& element)
							{ return element.first == streamID; });

		bool allowTest = false;
		byte mask = recvKey ? RECV_TESTED : SEND_TESTED;
		if (streamIt == keyit->streamsTested.end())
		{
			allowTest = true;
			keyit->streamsTested.push_back(make_pair(streamID, mask));
		}
		else 
		{

			byte previousMask = streamIt->second;
			if (recvKey && (previousMask & mask) == 0)
			{
				streamIt->second |= mask;
				allowTest = true;
			}
			else if (!recvKey && (previousMask & mask) == 0)
			{
				streamIt->second |= mask;
				allowTest = true;
			}
		}

		if(allowTest)
		{
			//key was passed in a packet, not a login key
			if (keyit->key->foundAddress == SENT_BY_SERVER)
				continue;

			if (hintKey)
			{
				if (memcmp(hintKey->salsakey, keyit->key->salsakey, SALSA_KEY_SIZE))
					continue;
			}

			chosenCandidate = keyit->key;
			break;
		}
	}

	ReleaseMutex(keyVecMutex);
	return chosenCandidate;
}

/*
Mark a key as 'claimed' so it is no longer presented as a candidate to other streams

Arguments: Pointer to key object, ID of claiming stream
*/
void key_grabber_thread::claimKey(KEYDATA *key, unsigned int keyStreamID)
{
	DWORD waitResult = WaitForSingleObject(keyVecMutex, INFINITE);

	auto keyit = unclaimedKeys.begin();
	for (; keyit != unclaimedKeys.end(); keyit++)
	{
		if (keyit->key == key)
			break;
	}

	if (keyit != unclaimedKeys.end())
		unclaimedKeys.erase(keyit);
	else
	{
		stringstream errmsg;
		errmsg << "ERROR: Stream " << keyStreamID <<
			" tried to claim a key not present in unclaimed list";
		UIaddLogMsg(errmsg.str(), key->sourceProcess, uiMsgQueue);
	}

	ReleaseMutex(keyVecMutex);
}

/*
This searches the memory of a gameclient using addresses passed to it in the
gameClient address queue for salsa keys. Keys will be placed in this objects key vector.

Argument: gameclient object to scan for salsa keys
*/
void key_grabber_thread::memoryScanWorker(GAMECLIENTINFO *gameClient)
{
	gameClient->handlesOpen.notify();

	DWORD processID = gameClient->pid;
	std::pair<void *, size_t> memRegion;
	std::vector<DWORD> procesMemChunk;

	//UIaddLogMsg("New memory scan worker thread started", processID, uiMsgQueue);

	while (true)
	{
		DWORD waitResult = WaitForSingleObject(gameClient->addressQueueMutex, 2000);
		if (gameClient->addressQueue.empty())
		{
			ReleaseMutex(gameClient->addressQueueMutex);
			Sleep(100);
			continue;
		}

		if (waitResult)
		{
			ReleaseMutex(gameClient->addressQueueMutex);

			std::stringstream errMsg;
			errMsg << "WARNING: memoryScanWorker - Wait on AddressMutex failed. Result: " << waitResult
				<< " Error: " << GetLastError();
			UIaddLogMsg(errMsg.str(), processID, uiMsgQueue);

			Sleep(1000);
			continue;
		}

		if (gameClient->addressQueue.empty())  //another thread got it first
		{
			ReleaseMutex(gameClient->addressQueueMutex);
			Sleep(5);
			continue;
		}

		memRegion = gameClient->addressQueue.front();
		gameClient->addressQueue.pop();
		ReleaseMutex(gameClient->addressQueueMutex);

		//this is our exit signal
		if (memRegion.first == (void *)PLEASE_TERMINATE && 
			memRegion.second == PLEASE_TERMINATE)
		{
			//UIaddLogMsg("Memory scan worker thread terminating", processID, uiMsgQueue);
			break;
		}
		//printf("Scanning region %p (size %d)\n", memRegion.first, memRegion.second);

		void * memAddr = memRegion.first;
		size_t regionSize = memRegion.second;
		if (regionSize > procesMemChunk.size())
			procesMemChunk.resize(regionSize);

		SIZE_T bytesRead;

		if (ReadProcessMemory(gameClient->processhandle, memAddr, 
			&procesMemChunk[0], regionSize, &bytesRead))
		{
			//printf("Scanning chunk %lx (Size %d) (queuesize:%d)\n", memAddr, regionSize, addressQueue.size());
			
			for (size_t i = 0;
				i < (((bytesRead - KEYBLOB_SIZE) / sizeof(DWORD)) - 1); 
				i += 4) //string always starts on 16 byte boundary
			{
				if (procesMemChunk[i] != (DWORD)0x61707865) continue;//"expa" reversed
				if (memcmp(procesMemChunk.data() + i, "expand 32-byte k", 16)) continue;
				/*
				std::stringstream resultMsg;
				resultMsg << "Found candidate key blob in region size: " << 
					((float)regionSize / 1024.0) << "KB";
				UIaddLogMsg(resultMsg.str(), pid, uiMsgQueue);
				*/
				DWORD *keyBlob = procesMemChunk.data() + i + 4;
				DWORD keyFoundAddr = (DWORD)memAddr+ i;
				insertKey(processID, keyBlob, keyFoundAddr);
			}

		}
		else
		{
			DWORD lasterr = GetLastError();
			if (lasterr != ERROR_PARTIAL_COPY)
				std::cout << " readprocmem err 0x" << std::hex << lasterr << std::endl;
			//Sleep(50);
		}
	}

	gameClient->handlesOpen.wait();
}

/*
This starts the memory scanning worker threads, then searches through 
address space for promising memory regions to pass to the workers

Argument: gameclient object to scan for salsa keys
*/
void key_grabber_thread::grabKeys(GAMECLIENTINFO *gameClient)
{
	MEMORY_BASIC_INFORMATION info;
	char* p = 0, *nextp = 0;

	DWORD processID = gameClient->pid;

	gameClient->processhandle =
		OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, processID);
	if (!gameClient->processhandle)
		return;

	//an initial test for queryability
	if (!VirtualQueryEx(gameClient->processhandle, p, &info, sizeof(info)) == sizeof(info))
		return;

	UIaddLogMsg("Starting key scan for game process", processID, uiMsgQueue);

	WaitForSingleObject(gameClient->addressQueueMutex, INFINITE);
	while(!gameClient->addressQueue.empty())
		gameClient->addressQueue.pop();
	ReleaseMutex(gameClient->addressQueueMutex);

	//start threads to scan memory from candidate addresses found by this thread
	memWorkerParams paramStruct;
	paramStruct.thisptr = (void*)this;
	paramStruct.clientInfo = gameClient;

	for (int i = 0; i < SCAN_WORKER_THREAD_COUNT; i++)
		CreateThread(NULL, 0, scanWorkerStart, (void*)&paramStruct, 0, 0);

	p = 0;
	while (gameClient->needsLoginKey)
	{
		p = nextp;
		if (VirtualQueryEx(gameClient->processhandle, p, &info, sizeof(info)) == sizeof(info))
		{
			nextp += info.RegionSize;
			//some filters to avoid scanning memory where the key (hopefully) won't be
			//homework: narrow them down to be as restrictive as possible without missing any keys
			if (info.AllocationProtect != PAGE_READWRITE) continue;
			if (!(info.State & MEM_COMMIT)) continue;
			if (info.Type != MEM_PRIVATE) continue;
			if (info.RegionSize > 20 * 1024 * 1024 || info.RegionSize <= 1024) continue;

			//totalMem += info.RegionSize;
			WaitForSingleObject(gameClient->addressQueueMutex, INFINITE);
			gameClient->addressQueue.emplace(make_pair(info.BaseAddress, info.RegionSize));
			ReleaseMutex(gameClient->addressQueueMutex);
		}
		else
		{
			DWORD lasterr = GetLastError();
			if (lasterr != ERROR_INVALID_PARAMETER)
			{
				/*
				std::stringstream errMsg;
				errMsg << "Warning: Failed to VirtualQueryEx the client process. Err: " << lasterr;
				UIaddLogMsg(errMsg.str(), processID, uiMsgQueue);
				*/
				if (lasterr == ERROR_ACCESS_DENIED)
					break;
			}
			p = 0, nextp = 0;
			Sleep(1250);
		}

	}

	//tell the worker threads to terminate
	WaitForSingleObject(gameClient->addressQueueMutex, INFINITE);
	for (int i = 0; i < SCAN_WORKER_THREAD_COUNT; i++)
		gameClient->addressQueue.emplace((void *)PLEASE_TERMINATE, PLEASE_TERMINATE);
	ReleaseMutex(gameClient->addressQueueMutex);

	UIaddLogMsg("Ending key scan for game process", processID, uiMsgQueue);

	//wait for workers to terminate then cleanup
	while (!gameClient->handlesOpen.empty())
		Sleep(5);

	if (gameClient->processhandle)
	{
		CloseHandle(gameClient->processhandle);
		gameClient->processhandle = NULL;
	}
}

/*
Iterate through windows process list looking for pathofexile processes
Argument: destination vector for processIDs of running clients
Returns: number of processes found
*/
int key_grabber_thread::getClientPIDs(std::vector <DWORD>& resultsList)
{
	resultsList.clear();

	PROCESSENTRY32 entry;
	entry.dwSize = sizeof(PROCESSENTRY32);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (Process32First(snapshot, &entry) == TRUE)
	{
		while (Process32Next(snapshot, &entry) == TRUE)
		{
			std::wstring binPath = entry.szExeFile;
			if (binPath.find(L"PathOfExile.exe") != std::wstring::npos ||
				binPath.find(L"PathOfExile_x64.exe") != std::wstring::npos) 
			{
				DWORD newPID = entry.th32ProcessID;
				resultsList.push_back(newPID);
			}
		}
	}
	CloseHandle(snapshot);
	return resultsList.size();
}

/*
Argument: process ID of game client
Returns: pointer to an object with client metadata
*/
GAMECLIENTINFO* key_grabber_thread::get_process_obj(DWORD pid)
{
	GAMECLIENTINFO* result = NULL;

	processListMutex.lock();
	for (auto it = activeClients.begin(); it != activeClients.end(); it++)
	{
		if (((GAMECLIENTINFO*)*it)->pid == pid)
		{
			result = *it;
			break;
		}
	}
	processListMutex.unlock();

	return result;
}

//this stops the active scan of the process but leaves its ID in activeClients
//to restart scan (after logout/disconnect) just requires removing it from that vector
void key_grabber_thread::stopProcessScan(DWORD processID)
{
	GAMECLIENTINFO* client = get_process_obj(processID);
	if (client)
	{
		client->needsLoginKey = false;
		WaitForSingleObject(this->keyVecMutex, INFINITE);

		//cleanup any unused keys
		for (auto it = unclaimedKeys.begin(); it != unclaimedKeys.end(); it++)
		{
			if (it->key->sourceProcess == processID)
			{
				it = unclaimedKeys.erase(it);
				if (it == unclaimedKeys.end())
					break;
			}
		}

		ReleaseMutex(this->keyVecMutex);

	}
	else
	{
		UIaddLogMsg("ERROR: Tried to stop scan on non-existant client", processID, uiMsgQueue);
	}
}

/*
This loop maintains a list of pathofexile processes
When a new one is detected a set of memory scanning threads are launched for it
When a current one terminates the ui is notified
*/
void key_grabber_thread::main_loop()
{
	
	while (!terminateScanning)
	{
		//get running clients
		std::vector <DWORD> latestClientPIDs;
		int clientsFound = getClientPIDs(latestClientPIDs);
		if (!clientsFound) {
			//client will take a while to start and login
			//so doesn't need to be a short sleep
			Sleep(1000);  
			continue;
		}

		//check for new client processes
		for (auto it = latestClientPIDs.begin(); it != latestClientPIDs.end(); it++)
		{
			DWORD freshProcessID = *it;
			GAMECLIENTINFO *client = get_process_obj(freshProcessID);

			if (!client)
			{
				client = new GAMECLIENTINFO(freshProcessID);

				processListMutex.lock();
				activeClients.push_back(client);
				processListMutex.unlock();

				UInotifyClientRunning(freshProcessID, true, uiMsgQueue);
				grabKeys(client);
			}
		}

		//check if the old clients are still running
		processListMutex.lock();
		for (auto knownProcessIt = activeClients.begin(); 
			knownProcessIt != activeClients.end(); knownProcessIt++)
		{
			DWORD seenBeforePID = (*knownProcessIt)->pid;

			bool stillRunning = std::find(latestClientPIDs.begin(),
										latestClientPIDs.end(),
											seenBeforePID) != latestClientPIDs.end();
			if (!stillRunning){
				UInotifyClientRunning(seenBeforePID, false, uiMsgQueue);
				knownProcessIt = activeClients.erase(knownProcessIt);
				if (knownProcessIt == activeClients.end())
					break;
			}
		}
		processListMutex.unlock();

		Sleep(500);
	}
}

key_grabber_thread::~key_grabber_thread()
{
}