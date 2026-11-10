#include <rknn_api.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <signal.h>

int main(int argc, char **argv)
{
    rknn_context context = 0;
    try
    {
        rlimit memory{1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024};
        setrlimit(RLIMIT_AS, &memory);
        rlimit cpu{20, 20};
        setrlimit(RLIMIT_CPU, &cpu);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (argc != 2)
        {
            throw std::runtime_error("Usage: rk-model-info model.rknn");
        }
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<char> model((std::istreambuf_iterator<char>(input)), {});
        // The runtime reports the conversion toolkit in its model information log.
        setenv("RKNN_LOG_LEVEL", "3", 1);
        if (model.empty() ||
            rknn_init(&context, model.data(), static_cast<uint32_t>(model.size()), 0, nullptr) != 0)
        {
            throw std::runtime_error("cannot initialize model");
        }
        rknn_input_output_num io{};
        if (rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != 0)
        {
            throw std::runtime_error("query I/O");
        }
        nlohmann::json result;
        for (int output = 0; output < 2; ++output)
        {
            auto list = nlohmann::json::array();
            for (uint32_t i = 0; i < (output ? io.n_output : io.n_input); ++i)
            {
                rknn_tensor_attr attr{};
                attr.index = i;
                if (rknn_query(context, output ? RKNN_QUERY_OUTPUT_ATTR : RKNN_QUERY_INPUT_ATTR,
                               &attr, sizeof(attr)) != 0)
                {
                    throw std::runtime_error("query tensor");
                }
                std::vector<uint32_t> dims(attr.dims, attr.dims + attr.n_dims);
                list.push_back({{"index", i},
                                {"name", attr.name},
                                {"dims", dims},
                                {"format", get_format_string(attr.fmt)},
                                {"type", get_type_string(attr.type)},
                                {"quantization", get_qnt_type_string(attr.qnt_type)},
                                {"zero_point", attr.zp},
                                {"scale", attr.scale},
                                {"bytes", attr.size},
                                {"stride_bytes", attr.size_with_stride}});
                if (output)
                {
                    rknn_tensor_attr native{};
                    native.index = i;
                    if (rknn_query(context, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &native,
                                   sizeof(native)) == 0)
                    {
                        list.back()["native_type"] = get_type_string(native.type);
                    }
                }
            }
            result[output ? "outputs" : "inputs"] = list;
        }
        std::fflush(nullptr);
        std::cout << result.dump(2) << '\n';
        rknn_destroy(context);
        return 0;
    }
    catch (const std::exception &e)
    {
        if (context)
        {
            rknn_destroy(context);
        }
        std::cerr << e.what() << '\n';
        return 1;
    }
}
