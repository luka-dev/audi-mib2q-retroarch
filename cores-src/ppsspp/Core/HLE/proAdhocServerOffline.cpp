// QNX/MHI2Q offline build: the in-process PRO AdHoc rendezvous server is not
// part of the appliance. These symbols keep the existing HLE lifecycle safe.

#include "Core/HLE/proAdhocServer.h"

std::atomic<bool> adhocServerRunning(false);
std::thread adhocServerThread;

void __AdhocServerInit() {
	adhocServerRunning = false;
}

int proAdhocServerThread(int) {
	adhocServerRunning = false;
	return -1;
}
