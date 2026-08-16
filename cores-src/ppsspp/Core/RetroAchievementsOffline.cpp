// QNX/MHI2Q offline build: preserve core call sites and save-state framing
// without embedding rcheevos or making network requests.

#include "Core/RetroAchievements.h"

#include "Common/Serialize/SerializeFuncs.h"

namespace Achievements {

rc_client_t *GetClient() { return nullptr; }
bool IsLoggedIn() { return false; }
bool IsActive() { return false; }
bool UnofficialEnabled() { return false; }
bool EncoreModeActive() { return false; }
bool HardcoreModeActive() { return false; }
bool WarnUserIfHardcoreModeActive(bool, std::string_view) { return false; }
bool IsBlockingExecution() { return false; }
bool RAIntegrationDirty() { return false; }

size_t GetRichPresenceMessage(char *, size_t) { return static_cast<size_t>(-1); }

void Initialize() {}
void UpdateSettings() {}
bool HasToken() { return false; }
bool LoginProblems(std::string *errorString) {
	if (errorString)
		*errorString = "RetroAchievements are unavailable in the offline QNX build";
	return false;
}
bool LoginAsync(const char *, const char *) { return false; }
void Logout() {}
bool Shutdown() { return true; }
void FrameUpdate() {}
void Idle() {}

void DoState(PointerWrap &p) {
	auto section = p.Section("Achievements", 0, 1);
	if (!section)
		return;

	uint32_t dataSize = 0;
	Do(p, dataSize);
	p.SkipBytes(dataSize);
}

bool HasAchievementsOrLeaderboards() { return false; }
void DownloadImageIfMissing(std::string_view) {}
Statistics GetStatistics() { return {}; }
std::string GetGameAchievementSummary(uint32_t) { return {}; }
void SetGame(const Path &, IdentifiedFileType, FileLoader *) {}
void ChangeUMD(const Path &, FileLoader *) {}
void UnloadGame() {}
std::set<uint32_t> GetActiveChallengeIDs() { return {}; }

}  // namespace Achievements
