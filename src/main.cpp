#include "pch.hpp"

#include "app.hpp"

int main(int argc, char** argv)
{
    static_cast<void>(argc);
    static_cast<void>(argv);

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
