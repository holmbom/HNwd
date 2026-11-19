// Gemensam ingång för verktygen: argumenten som UTF-8 på alla plattformar.
//
// På Windows ger main() argumenten i den aktiva ANSI-kodsidan, och en sökväg
// med tecken utanför den går då inte att öppna. wmain() ger dem som UTF-16, och
// de görs om till UTF-8 här. Verktyget skriver bara tool_run().

#pragma once

#include <string>
#include <vector>

#include "path_utf8.h"

// Verktygets egentliga main. args[0] är programnamnet.
int tool_run(const std::vector<std::string>& args);

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        args.push_back(HNwd::detail::wide_to_utf8(argv[i]));
    return tool_run(args);
}
#else
int main(int argc, char** argv) {
    return tool_run(std::vector<std::string>(argv, argv + argc));
}
#endif
