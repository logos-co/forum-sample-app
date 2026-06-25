#include "broadcast_module_impl.h"

std::string BroadcastModuleImpl::greet(const std::string& name)
{
    std::string greeting = "Hello, " + name + "! Greetings from the broadcast module.";

    // The generated event body routes the typed payload to every subscriber.
    greeted(greeting);

    return greeting;
}

std::string BroadcastModuleImpl::getStatus()
{
    return "Broadcast module is running.";
}
