#ifndef PRIVATEER_HANDLER_TEXT_HPP
#define PRIVATEER_HANDLER_TEXT_HPP

// Marks a function as fault-handler text. All handler-text functions are
// placed in one linker section so install_fault_handler can mlock the whole
// range: a reclaimed text page touched while the fault signal is masked
// would kill the process. noinline keeps the code inside the section.

#ifdef __APPLE__
#define PRIVATEER_HANDLER_TEXT __attribute__((section("__TEXT,__pv_handler,regular,pure_instructions"), noinline))
#else
#define PRIVATEER_HANDLER_TEXT __attribute__((section("pv_handler_text"), noinline))
#endif

#endif  // PRIVATEER_HANDLER_TEXT_HPP
