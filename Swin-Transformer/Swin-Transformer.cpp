#include "Swin-Transformer.h"
#include "yaml-cpp/yaml.h"
#include "common.hpp"

Swin_Transformer::Swin_Transformer(const std::string &config_file) {
    YAML::Node root = YAML::LoadFile(config_file);
    YAML::Node config = root["Swin_Transformer"];
    onnx_file = config["onnx_file"].as<std::string>();
    engine_file = config["engine_file"].as<std::string>();
    labels_file = config["labels_file"].as<std::string>();
    BATCH_SIZE = config["BATCH_SIZE"].as<int>();
    INPUT_CHANNEL = config["INPUT_CHANNEL"].as<int>();
    IMAGE_WIDTH = config["IMAGE_WIDTH"].as<int>();
    IMAGE_HEIGHT = config["IMAGE_HEIGHT"].as<int>();
    img_mean = config["img_mean"].as<std::vector<float>>();
    img_std = config["img_mean"].as<std::vector<float>>();
    imagenet_labels = readImageNetLabel(labels_file);
}

Swin_Transformer::~Swin_Transformer() = default;

void Swin_Transformer::LoadEngine() {
    // create and load engine
    std::fstream existEngine;
    existEngine.open(engine_file, std::ios::in);
    if (existEngine) {
        readTrtFile(engine_file, engine);
        assert(engine != nullptr);
    } else {
        onnxToTRTModel(onnx_file, engine_file, engine, BATCH_SIZE);
        assert(engine != nullptr);
    }
}

bool Swin_Transformer::InferenceFolder(const std::string &folder_name) {
    std::vector<std::string> sample_images = readFolder(folder_name);
    //get context
    assert(engine != nullptr);
    context = engine->createExecutionContext();
    assert(context != nullptr);

    //get buffers
    assert(engine->getNbBindings() == 2);
    void *buffers[2];
    std::vector<int64_t> bufferSize;
    int nbBindings = engine->getNbBindings();
    bufferSize.resize(nbBindings);

    for (int i = 0; i < nbBindings; ++i) {
        nvinfer1::Dims dims = engine->getBindingDimensions(i);
        nvinfer1::DataType dtype = engine->getBindingDataType(i);
        int64_t totalSize = volume(dims) * 1 * getElementSize(dtype);
        bufferSize[i] = totalSize;
        std::cout << "binding" << i << ": " << totalSize << std::endl;
        cudaMalloc(&buffers[i], totalSize);
    }

    //get stream
    cudaStream_t stream;
    cudaStreamCreate(&stream);

    int outSize = bufferSize[1] / sizeof(float) / BATCH_SIZE;

    EngineInference(sample_images, outSize, buffers, bufferSize, stream);

    // release the stream and the buffers
    cudaStreamDestroy(stream);
    cudaFree(buffers[0]);
    cudaFree(buffers[1]);

    // destroy the engine
    context->destroy();
    engine->destroy();
}

void Swin_Transformer::EngineInference(const std::vector<std::string> &image_list, const int &outSize, void **buffers,
                              const std::vector<int64_t> &bufferSize, cudaStream_t stream) {
    int index = 0;
    int batch_id = 0;
    std::vector<cv::Mat> vec_Mat(BATCH_SIZE);
    float total_time = 0;
    for (const std::string &image_name : image_list)
    {
        index++;
        std::cout << "Processing: " << image_name << std::endl;
        cv::Mat src_img = cv::imread(image_name);
        if (src_img.data)
        {
            cv::cvtColor(src_img, src_img, cv::COLOR_BGR2RGB);
            vec_Mat[batch_id] = src_img.clone();
            batch_id++;
        }
        if (batch_id == BATCH_SIZE or index == image_list.size())
        {
            auto t_start_pre = std::chrono::high_resolution_clock::now();
            std::cout << "prepareImage" << std::endl;
            std::vector<float>curInput = prepareImage(vec_Mat);
            auto t_end_pre = std::chrono::high_resolution_clock::now();
            float total_pre = std::chrono::duration<float, std::milli>(t_end_pre - t_start_pre).count();
            std::cout << "prepare image take: " << total_pre << " ms." << std::endl;
            total_time += total_pre;
            batch_id = 0;
            if (!curInput.data()) {
                std::cout << "prepare images ERROR!" << std::endl;
                continue;
            }
            // DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
            std::cout << "host2device" << std::endl;
            cudaMemcpyAsync(buffers[0], curInput.data(), bufferSize[0], cudaMemcpyHostToDevice, stream);

            // do inference
            std::cout << "execute" << std::endl;
            auto t_start = std::chrono::high_resolution_clock::now();
            context->execute(BATCH_SIZE, buffers);
            auto t_end = std::chrono::high_resolution_clock::now();
            float total_inf = std::chrono::duration<float, std::milli>(t_end - t_start).count();
            std::cout << "Inference take: " << total_inf << " ms." << std::endl;
            total_time += total_inf;
            std::cout << "execute success" << std::endl;
            std::cout << "device2host" << std::endl;
            std::cout << "post process" << std::endl;
            auto r_start = std::chrono::high_resolution_clock::now();
            float out[outSize * BATCH_SIZE];
            cudaMemcpyAsync(out, buffers[1], bufferSize[1], cudaMemcpyDeviceToHost, stream);
            cudaStreamSynchronize(stream);

            for (int i = 0; i < BATCH_SIZE; i++)
            {
                auto result = std::max_element(out + i * outSize, out + (i + 1) * outSize);
                std::string result_name = imagenet_labels[result - (out + i * outSize)];
                std::cout << "result: " << result_name << std::endl;
            }

            auto r_end = std::chrono::high_resolution_clock::now();
            float total_res = std::chrono::duration<float, std::milli>(r_end - r_start).count();
            std::cout << "Post process take: " << total_res << " ms." << std::endl;
            total_time += total_res;
            vec_Mat = std::vector<cv::Mat>(BATCH_SIZE);
        }
    }
    std::cout << "Average processing time is " << total_time / image_list.size() << "ms" << std::endl;
}

std::vector<float> Swin_Transformer::prepareImage(std::vector<cv::Mat> &vec_img) {
    std::vector<float> result(BATCH_SIZE * IMAGE_WIDTH * IMAGE_HEIGHT * INPUT_CHANNEL);
    float *data = result.data();
    for (const cv::Mat &src_img : vec_img)
    {
        if (!src_img.data)
            continue;
        cv::Mat flt_img;
        cv::resize(src_img, flt_img, cv::Size(IMAGE_WIDTH, IMAGE_HEIGHT));
        flt_img.convertTo(flt_img, CV_32FC3, 1.0 / 255);

        //HWC TO CHW
        std::vector<cv::Mat> split_img(INPUT_CHANNEL);
        cv::split(flt_img, split_img);

        int channelLength = IMAGE_WIDTH * IMAGE_HEIGHT;
        for (int i = 0; i < INPUT_CHANNEL; ++i)
        {
            split_img[i] = (split_img[i] - img_mean[i]) / img_std[i];
            memcpy(data, split_img[i].data, channelLength * sizeof(float));
            data += channelLength;
        }
    }
    return result;
}
