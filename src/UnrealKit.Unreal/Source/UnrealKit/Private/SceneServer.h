#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSceneImport, Log, All);

class FSocket;
class FRunnableThread;

class FSceneServer final : public FRunnable
{
public:
	~FSceneServer() override;

	void Start();
	void Stop() override;
	uint32 Run() override;

private:
	bool HandleClient(FSocket& Client);
	bool ReadHeaders(FSocket& Client, FString& Headers) const;
	bool ReadMessage(FSocket& Client, uint8& Opcode, TArray64<uint8>& Payload) const;
	bool SendFrame(FSocket& Client, uint8 Opcode, const uint8* Data, int64 Size) const;
	bool SendAll(FSocket& Client, const uint8* Data, int64 Size) const;
	bool RecvExact(FSocket& Client, uint8* Data, int64 Size) const;

	FRunnableThread* Thread = nullptr;
	FSocket* ListenSocket = nullptr;
	FThreadSafeBool Running = false;
};
