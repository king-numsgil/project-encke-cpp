#include "core/pch.hpp"

#include "app.hpp"
#include "core/log.hpp"
#include "platform/input_probe.hpp"
#include "terrain/benchmark.hpp"

#include <filesystem>
#include <system_error>

int main(int argc, char** argv)
{
    encke::AppOptions options;

    // --headless and --sweep run the benchmarks without a window or a
    // Vulkan device.
    for (int i = 1; i < argc; ++i)
    {
        if (string_view{argv[i]} == "--scenario" && i + 1 < argc)
        {
            options.scenarios.emplace_back(argv[++i]);
            continue;
        }
        if (string_view{argv[i]} == "--out" && i + 1 < argc)
        {
            options.output = std::filesystem::path{argv[++i]};
            continue;
        }
        // Before anything reads it: every baked surface map goes, and this
        // run bakes its own.
        if (string_view{argv[i]} == "--clear-cache")
        {
            std::error_code  error;
            std::uintmax_t const removed = std::filesystem::remove_all(ENCKE_CACHE_DIR, error);
            if (error)
            {
                encke::log::error("could not clear %s: %s", ENCKE_CACHE_DIR, error.message().c_str());
            }
            else
            {
                encke::log::info("cleared %s, %llu entries", ENCKE_CACHE_DIR, static_cast<unsigned long long>(removed));
            }
            continue;
        }
        if (string_view{argv[i]} == "--input-probe")
        {
            return encke::run_input_probe();
        }
        if (string_view{argv[i]} == "--headless")
        {
            return encke::terrain::run_benchmark();
        }
        if (string_view{argv[i]} == "--sweep")
        {
            u32 chunks  = 100'000;
            u32 threads = 0;
            for (int j = i + 1; j < argc; ++j)
            {
                if (string_view{argv[j]} == "--threads" && j + 1 < argc)
                {
                    threads = static_cast<u32>(std::strtoul(argv[++j], nullptr, 10));
                }
                else
                {
                    chunks = static_cast<u32>(std::strtoul(argv[j], nullptr, 10));
                }
            }
            return encke::terrain::run_sweep(chunks, threads);
        }
    }

    // Destructors tear down Vulkan then SDL; init failure is handled by the
    // same path, since every shutdown checks its handle first.
    encke::App app;

    if (!app.init(options))
    {
        return EXIT_FAILURE;
    }

    return app.run();
}
