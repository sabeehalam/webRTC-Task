cmake_minimum_required(VERSION 3.7)
if(POLICY CMP0079)
	cmake_policy(SET CMP0079 NEW)
endif()

set(STREAMER_SOURCES
	main.cpp
	dispatchqueue.cpp
	dispatchqueue.hpp
	h264fileparser.cpp
	h264fileparser.hpp
	helpers.cpp
	helpers.hpp
	opusfileparser.cpp
	opusfileparser.hpp
	fileparser.cpp
	fileparser.hpp
	stream.cpp
	stream.hpp
	ArgParser.cpp
	ArgParser.hpp
)

set(STREAMER_UWP_RESOURCES
	uwp/Logo.png
	uwp/package.appxManifest
	uwp/SmallLogo.png
	uwp/SmallLogo44x44.png
	uwp/SplashScreen.png
	uwp/StoreLogo.png
	uwp/Windows_TemporaryKey.pfx
)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
	add_executable(task-client-2 ${STREAMER_SOURCES} ${STREAMER_UWP_RESOURCES})
else()
	add_executable(task-client-2 ${STREAMER_SOURCES})
endif()

set_target_properties(task-client-2 PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME task-client-2)

set_target_properties(task-client-2 PROPERTIES
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.github.paullouisageneau.libdatachannel.examples.streamer)

find_package(Threads REQUIRED)
find_package(portaudio CONFIG REQUIRED)
target_link_libraries(task-client-2 LibDataChannel::LibDataChannel Threads::Threads nlohmann_json::nlohmann_json portaudio)

if(MSVC)
	add_custom_command(TARGET task-client-2 POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"$<TARGET_FILE_DIR:datachannel>/datachannel.dll"
		$<TARGET_FILE_DIR:task-client-2>
	)
endif()
