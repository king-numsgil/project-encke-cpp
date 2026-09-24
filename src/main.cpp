#include "core/pch.hpp"

#include "app.hpp"
#include "terrain/benchmark.hpp"

int main(int argc, char** argv)
{
    // --headless runs the benchmarks without a window or a Vulkan device.
    for (int i = 1; i < argc; ++i)
    {
        if (string_view{argv[i]} == "--headless")
        {
            return encke::terrain::run_benchmark();
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
