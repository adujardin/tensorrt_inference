#include "YOLOv6.h"

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cout << "Please design config file and folder name!" << std::endl;
        return -1;
    }
    std::string config_file = argv[1];
    std::string folder_name = argv[2];
    YOLOv6 YOLOv6(config_file);
    YOLOv6.LoadEngine();
    YOLOv6.InferenceFolder(folder_name);
    return 0;
}