module;

#ifdef USE_STD_MODULE
#include <logging/logging_macros.hpp>
#else
#include <logging/logging.hpp>
#endif

module lib_example;

#ifdef USE_STD_MODULE
import std;
import logiface;
#endif

namespace modern_cmake_template {

    namespace {
        int doSomethingPrivate(int x) {
            return x * x;
        }
    }

    int example::exampleFunction(int value) {
        LOGIFACE_LOG(debug, "exampleFunction invoked");
        return value * 2;
    }

    int example::anotherExampleFunction(int value) {
        LOGIFACE_LOG(debug, "anotherExampleFunction invoked");
        int result = doSomethingPrivate(value);
        return result + 10;
    }

}
