#include "helpers.hpp"
#include "nlohmann/json.hpp"
#include "opusfileparser.hpp"
#include "portaudio.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <rtc/websocket.hpp>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

const int SAMPLE_RATE = 48000;
const int FRAMES_PER_BUFFER = 960;

bool initializePortAudio() {
	PaError err = Pa_Initialize();
	if (err != paNoError) {
		std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
		return false;
	}
	return true;
}

void terminatePortAudio() { Pa_Terminate(); }

std::vector<uint8_t> captureAudioSample() {
	static PaStream *stream;
	static bool isInitialized = false;

	if (!isInitialized) {
		initializePortAudio();

		Pa_OpenDefaultStream(&stream,
		                     1,       // number of input channels
		                     0,       // number of output channels
		                     paInt16, // sample format
		                     SAMPLE_RATE, FRAMES_PER_BUFFER,
		                     nullptr,  // no callback, use blocking API
		                     nullptr); // no callback, so no user data

		Pa_StartStream(stream);
		isInitialized = true;
	}

	std::vector<int16_t> buffer(FRAMES_PER_BUFFER);
	Pa_ReadStream(stream, buffer.data(), FRAMES_PER_BUFFER);

	// Convert int16_t buffer to uint8_t
	std::vector<uint8_t> audioSample(buffer.size() * sizeof(int16_t));
	std::memcpy(audioSample.data(), buffer.data(), audioSample.size());

	return audioSample;
}

void playAudioSample(const std::vector<uint8_t> &sample) {
	static PaStream *stream;
	static bool isInitialized = false;

	if (!isInitialized) {
		initializePortAudio();

		Pa_OpenDefaultStream(&stream,
		                     0,       // number of input channels
		                     1,       // number of output channels
		                     paInt16, // sample format
		                     SAMPLE_RATE, FRAMES_PER_BUFFER,
		                     nullptr,  // no callback, use blocking API
		                     nullptr); // no callback, so no user data

		Pa_StartStream(stream);
		isInitialized = true;
	}

	// Convert uint8_t sample to int16_t
	std::vector<int16_t> buffer(sample.size() / sizeof(int16_t));
	std::memcpy(buffer.data(), sample.data(), sample.size());

	Pa_WriteStream(stream, buffer.data(), FRAMES_PER_BUFFER);
}

const int roomId = 1234;        // AudioBridge room ID
const int participantId = 2222; // Unique participant ID

optional<shared_ptr<Stream>> audioStream = nullopt;

void wsOnMessage(json message, shared_ptr<WebSocket> janusWs);

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

int64_t handleId = 0;  // Store the handle ID after attaching the plugin
int64_t sessionId = 0; // Store the session ID after attaching the plugin

void handleJanusMessage(json message, shared_ptr<rtc::WebSocket> janusWs) {
	if (message.contains("janus")) {
		string janusMessageType = message["janus"];

		if (janusMessageType == "success" && message.contains("transaction")) {
			string transaction = message["transaction"];
			if (transaction == "create-session-1") {
				sessionId = message["data"]["id"];
				// Session created, now attach the audiobridge plugin
				json attachPlugin = {{"janus", "attach"},
				                     {"transaction", "attach-plugin-1"},
				                     {"plugin", "janus.plugin.audiobridge"},
				                     {"session_id", sessionId}};
				janusWs->send(attachPlugin.dump());
				cout << "Sent attach plugin request: " << attachPlugin.dump() << endl;
			} else if (transaction == "attach-plugin-1") {
				// Plugin attached, save handle_id and join the existing room
				handleId = message["data"]["id"];
				json joinRoom = {
				    {"janus", "message"},
				    {"transaction", "join-room-1234"},
				    {"body",
				     {
				         {"request", "join"},
				         {"room", 1234},
				         {"id", 2222}, // Unique participant ID
				         {"display", "Participant1"},
				         {"codec", "opus"},
				         {"secret", "adminpwd"} // Replace with actual secret
				     }},
				    {"session_id", sessionId}, // Replace sessionId with the actual session_id
				    {"handle_id", handleId}    // Replace handleId with the actual handle_id
				};
				janusWs->send(joinRoom.dump());
				cout << "Sent join room request: " << joinRoom.dump() << endl;
			}
		} else if (janusMessageType == "event" &&
		           message["plugindata"]["plugin"] == "janus.plugin.audiobridge") {
			auto data = message["plugindata"]["data"];
			if (data.contains("audiobridge") && data["audiobridge"] == "joined") {
				cout << "Joined audio bridge room" << endl;
			} else if (data.contains("audiobridge") && data["audiobridge"] == "event") {
				// Handle incoming audio samples
				if (data.contains("participants")) {
					for (auto &participant : data["participants"]) {
						if (participant["id"] != 2222) {
							cout << "Participant " << participant["id"] << " is in the room"
							     << endl;
						}
					}
				}
			}
		} else if (janusMessageType == "message") {
			auto data = message["plugindata"]["data"];
			if (data.contains("result") && data["result"]["event"] == "created") {
				cout << "AudioBridge room created" << endl;
			}
		} else {
			cout << "Unhandled Janus message: " << message.dump() << endl;
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
		                       {"sample", message["sample"]}}},
		                     {"plugin", "janus.plugin.audiobridge"}};
		janusWs->send(janusMessage.dump());
		cout << "Sent audio sample message: " << janusMessage.dump() << endl;
	} else {
		// Handle other client messages
		cout << "Unhandled client message: " << message.dump() << endl;
	}
}

int main() try {
	rtc::InitLogger(rtc::LogLevel::Debug);

	// Initialize PortAudio
	if (!initializePortAudio()) {
		return -1;
	}

	// Janus WebSocket configuration
	rtc::WebSocket::Configuration janusConfig;
	janusConfig.protocols = {"janus-protocol"};

	auto janusWs = make_shared<rtc::WebSocket>(janusConfig);

	janusWs->onOpen([&]() {
		cout << "Janus WebSocket connected, creating session" << endl;

		// Create a session
		json createSession = {{"janus", "create"}, {"transaction", "create-session-1"}};
		janusWs->send(createSession.dump());
		cout << "Sent create session request: " << createSession.dump() << endl;
	});

	janusWs->onClosed([]() { cout << "Janus WebSocket closed" << endl; });

	janusWs->onError(
	    [](const string &error) { cout << "Janus WebSocket failed: " << error << endl; });

    janusWs->onMessage([&](variant<rtc::binary, rtc::string> data) {
		if (!holds_alternative<rtc::string>(data))
			return;

	    json message = json::parse(get<rtc::string>(data));
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

	// Wait for the user to end the program
	cout << "Connected. Enter any key to exit..." << endl;
	string input;
	cin >> input;

	// Cleanup
	janusWs->close();
	terminatePortAudio();
	cout << "Exiting..." << endl;
	return 0;

} catch (const std::exception &e) {
	std::cout << "Error: " << e.what() << std::endl;
	return -1;
}
