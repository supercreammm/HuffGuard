#include <iostream>
#include <string>
#include <cstdlib>
#include <chrono> // 計時用
#include "HuffGuardEngine.h"

// 接收外部參數：執行檔名, 模式, 輸入檔, 輸出檔, 密鑰
int main(int argc, char* argv[]) {
    // 解決 Windows 中文亂碼
    system("chcp 65001 > nul");

    // 檢查參數數量是否正確
    if (argc != 5) {
        std::cerr << "[錯誤] 參數數量錯誤。\n";
        std::cerr << "用法: .\\main.exe <compress|decompress> <輸入檔> <輸出檔> <密鑰>\n";
        return 1;
    }

    std::string mode = argv[1];
    std::string input_path = argv[2];
    std::string output_path = argv[3];
    std::string key = argv[4];

    HuffGuardEngine engine;

    if (mode == "compress") {
        engine.compress(input_path, output_path, key);
    } else if (mode == "decompress") {
        engine.decompress(input_path, output_path, key);
    } else {
        std::cerr << "[錯誤] 未知的模式: " << mode << "\n";
        return 1;
    }

    return 0;
}