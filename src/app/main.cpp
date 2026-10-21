#include <logging/logging_macros.hpp>

#ifdef USE_STD_MODULE
import std;
import logiface;
import logiface.ConsoleLogger;
#else
#include <string>
#include <vector>
#include <logging/logging.hpp>
#include <logging/ConsoleLogger.hpp>
#endif

import lib_example;

void initializeLogger();

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    initializeLogger();

    LOGIFACE_LOG(info, "Modern CMake Template CLI started");

    modern_cmake_template::example api;
    const int v1 = 21;
    const int v2 = 5;
    LOGIFACE_LOG(debug, "Calling exampleFunction");
    int result1 = api.exampleFunction(v1);
    LOGIFACE_LOG(debug, "exampleFunction(" + std::to_string(v1) + ") = " + std::to_string(result1));

    LOGIFACE_LOG(debug, "Calling anotherExampleFunction");
    int result2 = api.anotherExampleFunction(v2);
    LOGIFACE_LOG(debug, "anotherExampleFunction(" + std::to_string(v2) + ") = " + std::to_string(result2));

    if (!args.empty()) {
        std::string args_list;
        args_list = "Args:";
        for (auto a : args) args_list += " \"" + a + '"';
        LOGIFACE_LOG(debug, args_list);
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}

void initializeLogger() {
    static Logiface::ConsoleLogger app_logger;
    Logiface::SetLogger(&app_logger);
}
