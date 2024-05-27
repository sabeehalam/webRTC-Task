#include "helpers.hpp"
#include "nlohmann/json.hpp"
#include "opusfileparser.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <rtc/websocket.hpp>
#include <thread>
#include <unordered_map>
#include <variant>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

/// Directory for Opus samples
const string opusSamplesDirectory =
    "C:/Users/sabee/source/repos/libdatachannel-2/libdatachannel/examples/streamer/samples/opus/";

/// Audio stream
optional<shared_ptr<Stream>> audioStream = nullopt;

/// Incoming message handler for websocket
void wsOnMessage(json message, shared_ptr<WebSocket> janusWs);

/// Create stream
shared_ptr<Stream> createStream(const string opusSamples, weak_ptr<WebSocket> wsWeak) {
	// audio source
	auto audio = make_shared<OPUSFileParser>(opusSamples, true);

	auto stream = make_shared<Stream>(nullptr, audio); // Only audio stream
	// set callback responsible for sample sending
	stream->onSample(
	    [wsWeak](Stream::StreamSourceType type, uint64_t sampleTime, rtc::binary sample) {
		    if (type == Stream::StreamSourceType::Audio) {
			    // Convert rtc::binary (which might be std::vector<std::byte>) to
			    // std::vector<uint8_t>
			    std::vector<uint8_t> audioSample(sample.size());
			    std::transform(sample.begin(), sample.end(), audioSample.begin(),
			                   [](std::byte b) { return static_cast<uint8_t>(b); });

			    // Send audio sample via WebSocket
			    json message = {
			        {"type", "audio_sample"}, {"sample_time", sampleTime}, {"sample", audioSample}};
			    if (auto ws = wsWeak.lock()) {
				    ws->send(message.dump());
			    }
		    }
	    });
	return stream;
}

void handleJanusMessage(json message, shared_ptr<WebSocket> janusWs) {
	if (message.contains("janus")) {
		string janusMessageType = message["janus"];

		if (janusMessageType == "event" &&
		    message["plugindata"]["plugin"] == "janus.plugin.audiobridge") {
			// Handle events from the AudioBridge plugin
			auto data = message["plugindata"]["data"];
			if (data.contains("audiobridge") && data["audiobridge"] == "joined") {
				// Handle joined event
				cout << "Joined audio bridge room" << endl;
			}
		}
	}
}

void wsOnMessage(json message, shared_ptr<WebSocket> janusWs) {
	if (message.contains("type") && message["type"] == "audio_sample") {
		// Relay audio sample to Janus
		json janusMessage = {{"janus", "message"},
		                     {"transaction", "audio-sample"},
		                     {"body",
		                      {{"request", "audio_sample"},
		                       {"sample_time", message["sample_time"]},
		                       {"sample", message["sample"]}}}};
		janusWs->send(janusMessage.dump());
	} else {
		// Handle other client messages
	}
}

int main() try {
	InitLogger(LogLevel::Debug);

	// Janus WebSocket configuration
	rtc::WebSocket::Configuration janusConfig;
	janusConfig.protocols = {"janus-protocol"};

	auto janusWs = make_shared<WebSocket>(janusConfig);

	janusWs->onOpen([&]() {
		cout << "Janus WebSocket connected, creating session" << endl;

		// Create a session
		json createSession = {{"janus", "create"}, {"transaction", "create-session"}};
		janusWs->send(createSession.dump());
	});

	janusWs->onClosed([]() { cout << "Janus WebSocket closed" << endl; });

	janusWs->onError(
	    [](const string &error) { cout << "Janus WebSocket failed: " << error << endl; });

	janusWs->onMessage([&](variant<binary, string> data) {
		if (!holds_alternative<string>(data))
			return;

		json message = json::parse(get<string>(data));
		handleJanusMessage(message, janusWs);
	});

	const string janusUrl = "ws://127.0.0.1:8188/janus";
	cout << "Janus URL is " << janusUrl << endl;
	janusWs->open(janusUrl);

	cout << "Waiting for signaling to be connected..." << endl;
	while (!janusWs->isOpen()) {
		if (janusWs->isClosed())
			return 1;
		this_thread::sleep_for(100ms);
	}

	// Create the audio stream after WebSocket connection is open
	audioStream = createStream(opusSamplesDirectory, weak_ptr<WebSocket>(janusWs));

	cout << "Connected. Enter any key to exit..." << endl;
	string input;
	cin >> input;

	if (audioStream.has_value()) {
		audioStream.value()->stop();
	}

	cout << "Exiting..." << endl;
	return 0;

} catch (const std::exception &e) {
	std::cout << "Error: " << e.what() << std::endl;
	return -1;
}
