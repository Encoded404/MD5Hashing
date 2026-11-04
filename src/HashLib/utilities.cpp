module;

module hashlib.utilities;

import std;

// PackValues now lives in the module interface, so that callers in other translation units can inline it.
// This unit stays as the place for helpers that cannot be inlined, and to keep the module's implementation side from disappearing entirely.
