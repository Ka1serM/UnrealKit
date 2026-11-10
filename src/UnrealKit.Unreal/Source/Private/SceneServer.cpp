#include "SceneServer.h"

#include <atomic>
#include <vector>

#include "Async/Async.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeCounter.h"
#include "ImporterProduct.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"
#include "SceneImport.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
constexpr TCHAR ServerPath[] = TEXT("/scene");
constexpr ANSICHAR WebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr uint64 MaxMessage = 0x7fffffffull;
constexpr int32 MaxQueued = 64;
constexpr double TickBudget = 0.02;
constexpr double ProgressInterval = 0.25;

constexpr uint8 OpContinuation = 0x0;
constexpr uint8 OpBinary = 0x2;
constexpr uint8 OpClose = 0x8;
constexpr uint8 OpPing = 0x9;
constexpr uint8 OpPong = 0xA;

namespace stream = scene::stream;
using FSceneReply = TArray<uint8>;
using FMessage = TSharedPtr<TArray64<uint8>, ESPMode::ThreadSafe>;

FString HeaderValue(const FString& Headers, const FString& Name)
{
	TArray<FString> Lines;
	Headers.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		FString Left, Right;
		if (Line.Split(TEXT(":"), &Left, &Right) && Left.Equals(Name, ESearchCase::IgnoreCase))
			return Right.TrimStartAndEnd();
	}
	return {};
}

template <class T>
FSceneReply Envelope(flatbuffers::FlatBufferBuilder& Builder, const stream::Record Type, const flatbuffers::Offset<T> Record)
{
	stream::FinishEnvelopeBuffer(Builder, stream::CreateEnvelope(Builder, Type, Record.Union()));
	return FSceneReply(Builder.GetBufferPointer(), static_cast<int32>(Builder.GetSize()));
}

flatbuffers::Offset<stream::Diagnostic> DiagnosticOf(flatbuffers::FlatBufferBuilder& Builder, const FSceneDiagnostic& Diagnostic)
{
	const FTCHARToUTF8 Code(*Diagnostic.Code), Message(*Diagnostic.Message), Subject(*Diagnostic.Subject);
	const auto CodeText = Builder.CreateString(Code.Get(), Code.Length());
	const auto MessageText = Builder.CreateString(Message.Get(), Message.Length());
	const auto SubjectText = Builder.CreateString(Subject.Get(), Subject.Length());
	return stream::CreateDiagnostic(Builder, Diagnostic.Severity, CodeText, MessageText, SubjectText);
}

FSceneReply HelloReply()
{
	flatbuffers::FlatBufferBuilder Builder(64);
	const FTCHARToUTF8 Version(*IPluginManager::Get().FindPlugin(ImporterPlugin)->GetDescriptor().VersionName);
	const auto VersionText = Builder.CreateString(Version.Get(), Version.Length());
	return Envelope(Builder, stream::Record::Hello, stream::CreateHello(Builder, stream::Version::Current, VersionText));
}

FSceneReply ReadyReply()
{
	flatbuffers::FlatBufferBuilder Builder(64);
	return Envelope(Builder, stream::Record::Ready, stream::CreateReady(Builder));
}

FSceneReply ProgressReply(const uint32 Done, const uint32 Total)
{
	flatbuffers::FlatBufferBuilder Builder(64);
	return Envelope(Builder, stream::Record::Progress, stream::CreateProgress(Builder, Done, Total));
}

FSceneReply FailedReply(const FString& Code, const FString& Message)
{
	flatbuffers::FlatBufferBuilder Builder(256);
	const auto Diagnostic = DiagnosticOf(Builder, {stream::Severity::Error, Code, Message, {}});
	return Envelope(Builder, stream::Record::Failed, stream::CreateFailed(Builder, Diagnostic));
}

FSceneReply FinishedReply(const TArray<FSceneDiagnostic>& Diagnostics)
{
	flatbuffers::FlatBufferBuilder Builder(1024);
	std::vector<flatbuffers::Offset<stream::Diagnostic>> Offsets;
	for (const FSceneDiagnostic& Diagnostic : Diagnostics)
		Offsets.push_back(DiagnosticOf(Builder, Diagnostic));
	const auto Vector = Builder.CreateVector(Offsets);
	return Envelope(Builder, stream::Record::Finished, stream::CreateFinished(Builder, Vector));
}

const stream::Envelope* Verified(const TArray64<uint8>& Message)
{
	flatbuffers::Verifier::Options Options;
	Options.max_tables = 1u << 30;
	Options.max_size = MaxMessage;
	flatbuffers::Verifier Verifier(Message.GetData(), static_cast<size_t>(Message.Num()), Options);
	return stream::VerifyEnvelopeBuffer(Verifier) ? stream::GetEnvelope(Message.GetData()) : nullptr;
}

struct FConnection
{
	TQueue<FMessage, EQueueMode::Spsc> Messages;
	FThreadSafeCounter Queued;
	std::atomic<int32> Received{0};
	TQueue<FSceneReply, EQueueMode::Spsc> Replies;
	std::atomic<bool> bInputEnded{false};
	std::atomic<bool> bDone{false};
	std::atomic<bool> bAbandoned{false};
};
using FConnectionRef = TSharedRef<FConnection, ESPMode::ThreadSafe>;

class FImportTicker
{
public:
	explicit FImportTicker(FConnectionRef InConnection) : Connection(MoveTemp(InConnection)) {}

	bool Tick()
	{
		if (Connection->bAbandoned)
			return false;
		const double Deadline = FPlatformTime::Seconds() + TickBudget;
		FMessage Message;
		while (!Import.IsComplete() && FPlatformTime::Seconds() < Deadline && Connection->Messages.Dequeue(Message))
		{
			Connection->Queued.Decrement();
			++Built;
			FString Error;
			if (!Import.Accept(*stream::GetEnvelope(Message->GetData()), Error))
			{
				UE_LOG(LogSceneImport, Error, TEXT("The app broke the protocol: %s"), *Error);
				Connection->Replies.Enqueue(FailedReply(TEXT("import.protocol"), Error));
				return Done();
			}
		}
		if (const double Now = FPlatformTime::Seconds(); Now - LastProgress >= ProgressInterval)
		{
			LastProgress = Now;
			Connection->Replies.Enqueue(ProgressReply(static_cast<uint32>(Built), static_cast<uint32>(Connection->Received.load())));
		}
		if (Import.IsComplete())
		{
			Connection->Replies.Enqueue(ProgressReply(static_cast<uint32>(Built), static_cast<uint32>(Connection->Received.load())));
			Connection->Replies.Enqueue(FinishedReply(Import.Finish()));
			return Done();
		}
		if (Connection->bInputEnded && Connection->Messages.IsEmpty())
		{
			UE_LOG(LogSceneImport, Warning, TEXT("The export ended early. What arrived stays in the project, unsaved."));
			return Done();
		}
		return true;
	}

private:
	bool Done()
	{
		Connection->bDone = true;
		return false;
	}

	FConnectionRef Connection;
	FSceneImport Import;
	int32 Built = 0;
	double LastProgress = 0.0;
};
}

FSceneServer::~FSceneServer()
{
	Stop();
}

void FSceneServer::Start()
{
	if (Thread)
		return;
	Running = true;
	Thread = FRunnableThread::Create(this, TEXT("SceneServer"));
}

void FSceneServer::Stop()
{
	Running = false;
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (ListenSocket)
	{
		ListenSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
	}
}

uint32 FSceneServer::Run()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	ListenSocket = Sockets->CreateSocket(NAME_Stream, TEXT("SceneServer"), false);
	if (!ListenSocket)
		return 1;
	ListenSocket->SetReuseAddr(true);
	ListenSocket->SetNoDelay(true);
	TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
	bool bValid = false;
	Address->SetIp(TEXT("127.0.0.1"), bValid);
	Address->SetPort(ImporterPort);
	if (!bValid || !ListenSocket->Bind(*Address) || !ListenSocket->Listen(4))
	{
		UE_LOG(LogSceneImport, Error, TEXT("Cannot listen on ws://127.0.0.1:%d%s"), ImporterPort, ServerPath);
		return 1;
	}
	UE_LOG(LogSceneImport, Display, TEXT("Listening on ws://127.0.0.1:%d%s"), ImporterPort, ServerPath);
	while (Running)
	{
		bool bPending = false;
		if (!ListenSocket->WaitForPendingConnection(bPending, FTimespan::FromMilliseconds(100)) || !bPending)
			continue;
		FSocket* Client = ListenSocket->Accept(TEXT("SceneServerClient"));
		if (!Client)
			continue;
		Client->SetNoDelay(true);
		HandleClient(*Client);
		Client->Close();
		Sockets->DestroySocket(Client);
	}
	return 0;
}

bool FSceneServer::SendAll(FSocket& Client, const uint8* Data, int64 Size) const
{
	while (Size > 0)
	{
		int32 Sent = 0;
		if (!Client.Send(Data, static_cast<int32>(FMath::Min<int64>(Size, 1 << 20)), Sent) || Sent <= 0)
			return false;
		Data += Sent;
		Size -= Sent;
	}
	return true;
}

bool FSceneServer::RecvExact(FSocket& Client, uint8* Data, int64 Size) const
{
	while (Size > 0)
	{
		if (!Running)
			return false;
		if (!Client.Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(100)))
		{
			if (Client.GetConnectionState() != SCS_Connected)
				return false;
			continue;
		}
		int32 Read = 0;
		if (!Client.Recv(Data, static_cast<int32>(FMath::Min<int64>(Size, 1 << 20)), Read) || Read <= 0)
			return false;
		Data += Read;
		Size -= Read;
	}
	return true;
}

bool FSceneServer::ReadHeaders(FSocket& Client, FString& Headers) const
{
	TArray<uint8> Bytes;
	Bytes.Reserve(4096);
	while (Bytes.Num() < 16384)
	{
		uint8 Byte = 0;
		if (!RecvExact(Client, &Byte, 1))
			return false;
		Bytes.Add(Byte);
		const int32 N = Bytes.Num();
		if (N >= 4 && Bytes[N - 4] == '\r' && Bytes[N - 3] == '\n' && Bytes[N - 2] == '\r' && Bytes[N - 1] == '\n')
		{
			Bytes.Add(0);
			Headers = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
			return true;
		}
	}
	return false;
}

bool FSceneServer::ReadMessage(FSocket& Client, uint8& Opcode, TArray64<uint8>& Payload) const
{
	Payload.Reset();
	Opcode = 0xff;
	while (true)
	{
		uint8 Header[2]{};
		if (!RecvExact(Client, Header, 2))
			return false;
		const bool bFinal = (Header[0] & 0x80) != 0;
		const uint8 FrameOpcode = Header[0] & 0x0f;
		if ((Header[0] & 0x70) != 0)
			return false;
		uint64 Length = Header[1] & 0x7f;
		if (Length == 126)
		{
			uint8 N[2];
			if (!RecvExact(Client, N, 2))
				return false;
			Length = (uint64(N[0]) << 8) | N[1];
		}
		else if (Length == 127)
		{
			uint8 N[8];
			if (!RecvExact(Client, N, 8))
				return false;
			Length = 0;
			for (const uint8 B : N)
				Length = (Length << 8) | B;
		}
		const bool bControl = FrameOpcode >= 0x8;
		if ((bControl && (!bFinal || Length > 125)) || static_cast<uint64>(Payload.Num()) + Length > MaxMessage)
			return false;
		uint8 Mask[4]{};
		const bool bMasked = (Header[1] & 0x80) != 0;
		if (bMasked && !RecvExact(Client, Mask, 4))
			return false;

		TArray64<uint8> Control;
		TArray64<uint8>& Target = bControl ? Control : Payload;
		const int64 Start = Target.Num();
		Target.AddUninitialized(static_cast<int64>(Length));
		if (Length && !RecvExact(Client, Target.GetData() + Start, static_cast<int64>(Length)))
			return false;
		if (bMasked)
			for (int64 I = 0; I < static_cast<int64>(Length); ++I)
				Target[Start + I] ^= Mask[I & 3];

		if (bControl)
		{
			if (FrameOpcode == OpPing)
			{
				if (!SendFrame(Client, OpPong, Control.GetData(), Control.Num()))
					return false;
				continue;
			}
			if (FrameOpcode == OpPong)
				continue;
			Opcode = FrameOpcode;
			Payload = MoveTemp(Control);
			return true;
		}
		if (FrameOpcode != OpContinuation)
		{
			if (Opcode != 0xff)
				return false;
			Opcode = FrameOpcode;
		}
		else if (Opcode == 0xff)
		{
			return false;
		}
		if (bFinal)
			return true;
	}
}

bool FSceneServer::SendFrame(FSocket& Client, const uint8 Opcode, const uint8* Data, const int64 Size) const
{
	TArray<uint8> Header;
	Header.Add(static_cast<uint8>(0x80 | Opcode));
	if (Size < 126)
	{
		Header.Add(static_cast<uint8>(Size));
	}
	else if (Size <= 0xffff)
	{
		Header.Add(126);
		Header.Add(static_cast<uint8>(Size >> 8));
		Header.Add(static_cast<uint8>(Size));
	}
	else
	{
		Header.Add(127);
		for (int Shift = 56; Shift >= 0; Shift -= 8)
			Header.Add(static_cast<uint8>(static_cast<uint64>(Size) >> Shift));
	}
	return SendAll(Client, Header.GetData(), Header.Num()) && (Size == 0 || SendAll(Client, Data, Size));
}

bool FSceneServer::HandleClient(FSocket& Client)
{
	FString Headers;
	if (!ReadHeaders(Client, Headers) || !Headers.StartsWith(FString::Printf(TEXT("GET %s "), ServerPath)))
		return false;
	const FString Key = HeaderValue(Headers, TEXT("Sec-WebSocket-Key"));
	if (Key.IsEmpty())
		return false;
	const FTCHARToUTF8 Combined(*(Key + UTF8_TO_TCHAR(WebSocketGuid)));
	FSHA1 Sha;
	Sha.Update(reinterpret_cast<const uint8*>(Combined.Get()), Combined.Length());
	Sha.Final();
	uint8 Hash[FSHA1::DigestSize];
	Sha.GetHash(Hash);
	const FString Upgrade = FString::Printf(TEXT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
	                                             "Sec-WebSocket-Accept: %s\r\n\r\n"),
	                                        *FBase64::Encode(Hash, UE_ARRAY_COUNT(Hash)));
	const FTCHARToUTF8 UpgradeUtf8(*Upgrade);
	if (!SendAll(Client, reinterpret_cast<const uint8*>(UpgradeUtf8.Get()), UpgradeUtf8.Length()))
		return false;

	const auto Send = [this, &Client](const FSceneReply& Reply) { return SendFrame(Client, OpBinary, Reply.GetData(), Reply.Num()); };
	const auto Close = [this, &Client]() {
		if (!SendFrame(Client, OpClose, nullptr, 0))
			return;
		const double Deadline = FPlatformTime::Seconds() + 1.0;
		uint8 Opcode = 0;
		TArray64<uint8> Ignored;
		while (true)
		{
			const double Remaining = Deadline - FPlatformTime::Seconds();
			if (Remaining <= 0.0 || !Client.Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(Remaining)) || !ReadMessage(Client, Opcode, Ignored)
			    || Opcode == OpClose)
				return;
		}
	};

	if (!Send(HelloReply()))
		return false;
	{
		uint8 Opcode = 0;
		TArray64<uint8> Message;
		if (!ReadMessage(Client, Opcode, Message) || Opcode != OpBinary)
			return false;
		const stream::Envelope* Envelope = Verified(Message);
		const stream::Hello* Hello = Envelope ? Envelope->record_as_Hello() : nullptr;
		if (!Hello || Hello->version() != stream::Version::Current)
		{
			Send(FailedReply(TEXT("import.protocol"),
			                 FString::Printf(TEXT("the app speaks scene stream version %d, the plugin %d; install the plugin of the app"),
			                                 Hello ? static_cast<int32>(Hello->version()) : -1, static_cast<int32>(stream::Version::Current))));
			Close();
			return false;
		}
		if (!Send(ReadyReply()))
			return false;
	}

	const FConnectionRef Connection = MakeShared<FConnection, ESPMode::ThreadSafe>();
	AsyncTask(ENamedThreads::GameThread, [Connection]() {
		TSharedRef<FImportTicker> Ticker = MakeShared<FImportTicker>(Connection);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Ticker](float) { return Ticker->Tick(); }));
	});
	bool bReading = true;
	bool bSocketUsable = true;
	while (Running)
	{
		FSceneReply Reply;
		while (Connection->Replies.Dequeue(Reply))
			bSocketUsable = bSocketUsable && Send(Reply);
		if (Connection->bDone)
			break;
		if (!bReading || !bSocketUsable || Connection->Queued.GetValue() >= MaxQueued)
		{
			FPlatformProcess::Sleep(0.005f);
			continue;
		}
		if (!Client.Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(10)))
		{
			if (Client.GetConnectionState() == SCS_Connected)
				continue;
			bReading = bSocketUsable = false;
			Connection->bInputEnded = true;
			continue;
		}
		uint8 Opcode = 0;
		TArray64<uint8> Message;
		if (!ReadMessage(Client, Opcode, Message) || Opcode == OpClose)
		{
			UE_LOG(LogSceneImport, Warning, TEXT("The app disconnected before the export completed."));
			bReading = bSocketUsable = false;
			Connection->bInputEnded = true;
			continue;
		}
		const stream::Envelope* Envelope = Opcode == OpBinary ? Verified(Message) : nullptr;
		if (!Envelope)
		{
			Send(FailedReply(TEXT("import.protocol"), TEXT("the message is not a scene stream record")));
			bReading = false;
			Connection->bInputEnded = true;
			continue;
		}
		if (Envelope->record_type() == stream::Record::Failed)
		{
			UE_LOG(LogSceneImport, Display, TEXT("The app stopped the export."));
			bReading = false;
			Connection->bInputEnded = true;
			continue;
		}
		const bool bLast = Envelope->record_type() == stream::Record::Complete;
		Connection->Queued.Increment();
		Connection->Received.fetch_add(1);
		Connection->Messages.Enqueue(MakeShared<TArray64<uint8>, ESPMode::ThreadSafe>(MoveTemp(Message)));
		if (bLast)
		{
			bReading = false;
			Connection->bInputEnded = true;
		}
	}
	if (!Running)
	{
		Connection->bAbandoned = true;
		return false;
	}
	FSceneReply Reply;
	while (Connection->Replies.Dequeue(Reply))
		bSocketUsable = bSocketUsable && Send(Reply);
	if (bSocketUsable)
		Close();
	return true;
}
