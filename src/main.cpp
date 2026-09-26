#include "core/pch.hpp"

#include "app.hpp"
#include "terrain/benchmark.hpp"

int main(int argc, char** argv)
{
    // --headless and --sweep run the benchmarks without a window or a
    // Vulkan device.
    for (int i = 1; i < argc; ++i)
    {
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

    if (!app.init())
    {
        return EXIT_FAILURE;
    }

    app.run();
    return EXIT_SUCCESS;
}
