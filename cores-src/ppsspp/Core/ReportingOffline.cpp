// QNX/MHI2Q offline build: local log throttling remains available, while
// compatibility telemetry and its CRC/network worker are omitted.

#include "Core/Reporting.h"

#include "Common/Serialize/SerializeFuncs.h"

namespace Reporting {

namespace {
bool OfflineMessageAllowed() { return false; }
void IgnoreMessage(const char *, const char *) {}
}

void Init() {
	ResetCounts();
	SetupCallbacks(&OfflineMessageAllowed, &IgnoreMessage);
}

void Shutdown() { Init(); }

void DoState(PointerWrap &p) {
	auto section = p.Section("Reporting", 0, 1);
	if (!section)
		return;
	bool unsupported = true;
	Do(p, unsupported);
}

void UpdateConfig() {}
void NotifyDebugger() {}
void NotifyExecModule(const char *, int, uint32_t) {}
bool IsEnabled() { return false; }
bool IsSupported() { return false; }
bool Enable(bool, const std::string &) { return false; }
void EnableDefault() {}
void ReportCompatibility(const char *, int, int, int, const std::string &) {}
std::vector<std::string> CompatibilitySuggestions() { return {}; }
void QueueCRC(const Path &) {}
bool HasCRC(const Path &) { return false; }
void CancelCRC() {}
uint32_t RetrieveCRC(const Path &) { return 0; }
ReportStatus GetStatus() { return ReportStatus::FAILING; }
std::string ServerHost() { return {}; }
std::string CurrentGameID() { return {}; }

}  // namespace Reporting
