#include "mitm/modes.hpp"

#include <string>

namespace mitm {

AppArgs parseArgs(int argc, char** argv) {
    AppArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--controller") {
            args.controller = true;
            continue;
        }
        if (arg == "--subordinate") {
            args.subordinate = true;
            continue;
        }

        auto eq = arg.find('=');
        if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
            std::string key = arg.substr(2, eq - 2);
            std::string value = arg.substr(eq + 1);
            if (key == "count") {
                args.count = std::stoi(value);
            } else if (key == "socket") {
                args.socketPath = value;
            } else if (key == "id") {
                args.id = std::stoi(value);
            } else if (key == "device") {
                args.deviceNameHint = value;
            } else if (key == "visual") {
                args.visualMode = value;
            } else if (key == "color") {
                args.colorName = value;
            } else if (key == "spawn-token") {
                args.spawnToken = value;
            } else if (key == "web-port") {
                args.webPort = std::stoi(value);
            }
            continue;
        }

        if (arg.rfind("--", 0) != 0) {
            // Positional argument: device-name substring, only meaningful
            // in standalone mode.
            args.deviceNameHint = arg;
        }
    }

    return args;
}

} // namespace mitm
