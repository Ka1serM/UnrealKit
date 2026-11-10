#include "UnrealKit/Transport/SceneSession.h"

#include <format>
#include <utility>

namespace UnrealKit::Transport {

namespace {

using namespace std::chrono_literals;

constexpr auto silenceLimit = 120s;
constexpr auto handshakeLimit = 30s;
constexpr auto connectTimeout = 2s;
constexpr auto readTimeout = 1s;
constexpr auto writeTimeout = 120s;

std::span<const std::byte> bytesOf(const Scene::Frame& frame) {
    return {reinterpret_cast<const std::byte*>(frame.data()), frame.size()};
}

std::span<const std::uint8_t> bytesOf(const std::string& message) {
    return {reinterpret_cast<const std::uint8_t*>(message.data()), message.size()};
}

}

SceneSession::SceneSession(const Product product, const Importer importer, std::stop_token stop, VersionMismatch versionMismatch)
    : product(product), importer(importer), stop(std::move(stop)), versionMismatch(std::move(versionMismatch)), socket(writeTimeout) {}

std::unexpected<Core::Error> SceneSession::fail(const std::string_view message) const {
    return Core::fail("importer", std::format("{} {}", importerName(importer), message));
}

std::string SceneSession::failureText(const scene::stream::Failed& failed) const {
    const Core::Diagnostic diagnostic = Scene::diagnostic(*failed.diagnostic());
    return std::format("stopped the export: {} ({})", diagnostic.message, diagnostic.code);
}

SceneSession::~SceneSession() {
    bool open;
    {
        std::lock_guard lock(mutex);
        open = state == State::Open;
    }
    if (open && socket.isOpen()) {
        const Core::Diagnostic stopped{.severity = Core::Severity::Error, .code = "export.stopped", .message = "The app stopped the export.", .subject = {}};
        (void)socket.send(bytesOf(Scene::failed(stopped)));
    }
    socket.close();
}

std::expected<void, Core::Error> SceneSession::open(Progress progress) {
    this->progress = std::move(progress);
    const int port = importerPort(product, importer);
    if (!socket.connect(std::format("ws://127.0.0.1:{}/scene", port), connectTimeout))
        return fail(std::format("is not listening on 127.0.0.1:{}. Start it with the {} {} enabled.", port, product.name,
                                importer == Importer::Blender ? "extension" : "plugin"));
    if (std::expected<void, Core::Error> sent = socket.send(bytesOf(Scene::hello(product.version))); !sent)
        return sent;
    if (std::expected<void, Core::Error> ready = handshake(); !ready)
        return ready;
    reader = std::jthread([this] { read(); });
    return {};
}

std::expected<void, Core::Error> SceneSession::handshake() {
    const auto deadline = std::chrono::steady_clock::now() + handshakeLimit;
    bool greeted = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop.stop_requested())
            return Core::cancelled();
        std::string message;
        const std::expected<WebSocket::Received, Core::Error> received = socket.receive(message, readTimeout);
        if (!received)
            return std::unexpected(received.error());
        if (*received == WebSocket::Received::Timeout)
            continue;
        if (*received == WebSocket::Received::Closed)
            return fail("closed the connection before it was ready.");
        const scene::stream::Envelope* envelope = Scene::verified(bytesOf(message));
        if (envelope == nullptr)
            return fail("sent a message that is not a scene stream record.");
        if (const auto* hello = envelope->record_as_Hello()) {
            const std::string_view version = hello->product_version() ? hello->product_version()->string_view() : std::string_view();
            if (version != product.version && versionMismatch)
                versionMismatch(version);
            if (hello->version() != scene::stream::Version::Current)
                return fail(std::format("speaks scene stream version {}, the app {}. Install the importer of this app.", static_cast<int>(hello->version()),
                                        static_cast<int>(scene::stream::Version::Current)));
            greeted = true;
            continue;
        }
        if (envelope->record_as_Ready() && greeted)
            return {};
        if (const auto* failed = envelope->record_as_Failed())
            return fail(failureText(*failed));
        return fail(std::format("sent the record {} before it was ready.", scene::stream::EnumNameRecord(envelope->record_type())));
    }
    return fail("did not get ready in time. Its main thread is possibly busy.");
}

void SceneSession::read() {
    const auto end = [this](const State next, std::string text, std::vector<Core::Diagnostic> diagnostics) {
        std::lock_guard lock(mutex);
        if (state != State::Open)
            return;
        state = next;
        failure = std::move(text);
        finished = std::move(diagnostics);
        changed.notify_all();
    };
    while (socket.isOpen()) {
        std::string message;
        const std::expected<WebSocket::Received, Core::Error> received = socket.receive(message, readTimeout);
        if (!received)
            return end(State::Failed, std::format("lost the connection: {}", received.error().message), {});
        if (*received == WebSocket::Received::Timeout)
            continue;
        if (*received == WebSocket::Received::Closed)
            return end(State::Failed, "closed the connection before it built every record.", {});
        const scene::stream::Envelope* envelope = Scene::verified(bytesOf(message));
        if (envelope == nullptr)
            return end(State::Failed, "sent a message that is not a scene stream record.", {});
        {
            std::lock_guard lock(mutex);
            lastActivity = std::chrono::steady_clock::now();
        }
        if (const auto* report = envelope->record_as_Progress()) {
            if (progress)
                progress(report->done(), report->total());
            continue;
        }
        if (const auto* done = envelope->record_as_Finished())
            return end(State::Finished, {}, Scene::diagnostics(done->diagnostics()));
        if (const auto* failed = envelope->record_as_Failed())
            return end(State::Failed, failureText(*failed), {});
        return end(State::Failed, std::format("sent the record {} during the session.", scene::stream::EnumNameRecord(envelope->record_type())), {});
    }
}

std::expected<void, Core::Error> SceneSession::check() {
    if (stop.stop_requested())
        return Core::cancelled();
    std::lock_guard lock(mutex);
    if (state == State::Failed)
        return fail(failure);
    return {};
}

std::expected<void, Core::Error> SceneSession::send(const Scene::Frame frame) {
    if (std::expected<void, Core::Error> open = check(); !open)
        return open;
    return socket.send(bytesOf(frame));
}

std::expected<std::vector<Core::Diagnostic>, Core::Error> SceneSession::complete(const std::span<const Core::Diagnostic> diagnostics) {
    if (std::expected<void, Core::Error> sent = send(Scene::complete(diagnostics)); !sent)
        return std::unexpected(sent.error());
    std::unique_lock lock(mutex);
    lastActivity = std::chrono::steady_clock::now();
    while (state == State::Open) {
        if (stop.stop_requested())
            return Core::cancelled();
        if (std::chrono::steady_clock::now() - lastActivity > silenceLimit)
            return fail("stopped responding before it built every record.");
        changed.wait_for(lock, 250ms);
    }
    if (state == State::Failed)
        return fail(failure);
    return std::move(finished);
}

}
