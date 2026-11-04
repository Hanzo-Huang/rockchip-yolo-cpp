#include "worker/control.hpp"
extern "C"
{
#include <libavdevice/avdevice.h>
}
#include <iostream>

int main(int argc, char **argv)
{
    try
    {
        nvr::require(argc == 2, "Usage: rk-nvr-worker private-config.json");
        av_log_set_level(AV_LOG_QUIET);
        avformat_network_init();
        avdevice_register_all();
        std::ifstream file(argv[1]);
        nvr::Json config;
        file >> config;
        return nvr::run_worker(config);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
