#pragma once

#include <queue>          // std::priority_queue (Min-Heap 核心)
#include <unordered_map>  // 字元頻率表、編碼字典（O(1) 平均查找）
#include <vector>         // 動態陣列，用於位元緩衝
#include <string>         // std::string，密鑰與路徑處理
#include <fstream>        // std::ifstream / ofstream，檔案 I/O
#include <cstdint>        // uint8_t / uint32_t，精確位元寬度控制
#include <memory>         // std::unique_ptr，RAII 管理 Tree 節點記憶體


// =============================================================================
// Section 1：Huffman Tree 節點結構
// 設計考量：使用 unique_ptr 管理子節點，確保樹狀結構在 HuffGuardEngine
//           解構時自動釋放，避免 IoT 環境下的記憶體洩漏。
// =============================================================================

struct HuffNode {
    uint8_t  ch;        // 儲存字元（僅葉節點有效；內部節點設為 0）
    uint32_t freq;      // 該字元或子樹的累計出現頻率
    bool     is_leaf;   // 快速判斷是否為葉節點，避免額外的 null 判斷開銷

    std::unique_ptr<HuffNode> left;   // 左子節點（代表編碼位元 '0'）
    std::unique_ptr<HuffNode> right;  // 右子節點（代表編碼位元 '1'）

    // 葉節點建構子
    HuffNode(uint8_t c, uint32_t f)
        : ch(c), freq(f), is_leaf(true), left(nullptr), right(nullptr) {}

    // 內部節點建構子（合併兩子節點，頻率相加）
    HuffNode(uint32_t f,
             std::unique_ptr<HuffNode> l,
             std::unique_ptr<HuffNode> r)
        : ch(0), freq(f), is_leaf(false),
          left(std::move(l)), right(std::move(r)) {}
};


// =============================================================================
// Section 2：Min-Heap 自訂比較器
// 設計考量：std::priority_queue 預設為 Max-Heap，
//           透過自訂 Comparator 使頻率「最小」的節點優先被取出，
//           符合 Huffman 建樹演算法的貪婪策略。
// =============================================================================

struct NodeMinComparator {
    // 回傳 true 代表 lhs 的「優先級低於」rhs（即頻率較大的節點沉底）
    bool operator()(const std::unique_ptr<HuffNode>& lhs,
                    const std::unique_ptr<HuffNode>& rhs) const {
        return lhs->freq > rhs->freq; // 大頻率 = 低優先級 → 形成 Min-Heap
    }
};

// Min-Heap 型別別名，提升可讀性
using MinHeap = std::priority_queue<
    std::unique_ptr<HuffNode>,   // 元素型別
    std::vector<std::unique_ptr<HuffNode>>, // 底層容器
    NodeMinComparator            // 比較器
>;


// =============================================================================
// Section 3：HuffGuardEngine 主類別宣告
// =============================================================================

class HuffGuardEngine {
public:
    // =========================================================================
    // 公開介面（對外 API）
    // =========================================================================

    /**
     * @brief 壓縮 + 加密入口
     *
     * 完整流程：讀檔 → 頻率統計 → 建樹 → 生成字典 →
     *           XOR 加密標頭 → Bit-level 寫出壓縮檔
     *
     * @param input_path  輸入純文字檔路徑（ASCII）
     * @param output_path 輸出壓縮二進位檔路徑（.hgd）
     * @param key         XOR 加密密鑰字串（任意長度）
     */
    void compress(const std::string& input_path,
                  const std::string& output_path,
                  const std::string& key);

    /**
     * @brief 解壓縮 + 解密入口（無回饋防禦機制）
     *
     * 完整流程：讀取標頭 → XOR 解密（無論密鑰對錯強制執行）→
     *           重建 Huffman Tree → 位元流解碼 → 寫出還原檔
     *
     * 【安全設計】：密鑰錯誤時，程式不會 Throw / Crash，
     *              而是用錯誤的樹解碼，輸出亂碼檔案。
     *
     * @param input_path  輸入壓縮檔路徑（.hgd）
     * @param output_path 輸出還原（或亂碼）檔路徑
     * @param key         XOR 解密密鑰字串（任意長度）
     */
    void decompress(const std::string& input_path,
                    const std::string& output_path,
                    const std::string& key);


private:
    // =========================================================================
    // 私有成員變數（引擎狀態）
    // =========================================================================

    // 字元頻率表：key = uint8_t 字元，value = 出現次數
    std::unordered_map<uint8_t, uint32_t> freq_table_;

    // 編碼字典：key = uint8_t 字元，value = 對應的 Huffman 編碼字串（"0101..."）
    // 注意：此字串僅用於建構期間，實際寫入時透過 bitwise 操作轉換為位元流
    std::unordered_map<uint8_t, std::string> code_table_;

    // Huffman Tree 根節點（unique_ptr 確保 RAII 自動釋放）
    std::unique_ptr<HuffNode> tree_root_;

    // 位元緩衝：累積 8 個位元後才寫入一個 Byte，減少 I/O 次數
    uint8_t  bit_buffer_;      // 當前緩衝的位元組（最多 8 bits）
    uint8_t  bit_count_;       // bit_buffer_ 中已填入的位元數量（0~7）


    // =========================================================================
    // 私有方法 - 第一階段：頻率統計與建樹
    // =========================================================================

    /**
     * @brief 讀取輸入檔案，統計每個 ASCII 字元的出現頻率
     *
     * 運作邏輯：
     *   - 以二進位模式讀取（避免 Windows 換行符轉換問題）
     *   - 逐 Byte 計數，結果寫入 freq_table_
     *   - 複雜度：O(n)，n 為檔案位元組數
     *
     * @param input_path 輸入檔案路徑
     * @return 是否成功讀取（false = 檔案無法開啟）
     */
    bool buildFrequencyTable(const std::string& input_path);

    /**
     * @brief 以 freq_table_ 建構 Huffman Tree
     *
     * 運作邏輯：
     *   1. 將 freq_table_ 中所有字元各建立一個葉節點，推入 Min-Heap
     *   2. 重複取出頻率最小的兩個節點，合併成內部節點後推回 Heap
     *   3. 直到 Heap 只剩一個節點，即為 tree_root_
     *   - 複雜度：O(k log k)，k 為相異字元數量（最多 256）
     */
    void buildHuffmanTree();

    /**
     * @brief 遞迴走訪 Huffman Tree，產生每個字元的編碼並填入 code_table_
     *
     * 運作邏輯：
     *   - 向左走 → 路徑字串附加 '0'
     *   - 向右走 → 路徑字串附加 '1'
     *   - 抵達葉節點 → 將 (字元, 路徑字串) 存入 code_table_
     *
     * @param node    當前走訪的節點指標（裸指標，不轉移所有權）
     * @param current 從根到當前節點的編碼路徑字串
     */
    void generateCodes(const HuffNode* node, const std::string& current);


    // =========================================================================
    // 私有方法 - 第二階段：Bit-level I/O
    // =========================================================================

    /**
     * @brief 將一個位元（0 或 1）寫入輸出檔案
     *
     * 運作邏輯：
     *   - 將 bit 移位後 OR 進 bit_buffer_（MSB first）
     *   - bit_count_ 達到 8 時，呼叫 flushBitBuffer() 實際寫出一個 Byte
     *   - 複雜度：O(1)
     *
     * @param bit 要寫入的位元值（0 或 1）
     * @param out 輸出檔案串流
     */
    void writeBit(uint8_t bit, std::ofstream& out);

    /**
     * @brief 將 bit_buffer_ 中剩餘的不足 8 bits 補零後強制寫出
     *
     * 運作邏輯：
     *   - 在壓縮流結尾呼叫，避免最後不足一個 Byte 的位元遺失
     *   - 解壓縮時需知道最後一個 Byte 的有效位元數，
     *     此資訊應記錄於檔案標頭的 padding_bits 欄位
     */
    void flushBitBuffer(std::ofstream& out);

    /**
     * @brief 從輸入檔案逐位元讀取，返回下一個位元（0 或 1）
     *
     * 運作邏輯：
     *   - 每次呼叫消耗 bit_buffer_ 的一個位元（MSB first）
     *   - bit_buffer_ 耗盡時，從 in 讀取下一個 Byte
     *   - 複雜度：O(1)
     *
     * @param in     輸入檔案串流
     * @param out_bit 輸出的位元值（透過參數回傳）
     * @return 是否成功讀取（false = 已到達 EOF）
     */
    bool readBit(std::ifstream& in, uint8_t& out_bit);


    // =========================================================================
    // 私有方法 - 第三階段：XOR 靶向混淆（僅作用於標頭）
    // =========================================================================

    /**
     * @brief 對字典標頭的字元欄位執行 XOR 加密/解密
     *
     * 設計說明：
     *   - XOR 的「對稱性」使得加密與解密為同一操作：byte ^ key = cipher，
     *     cipher ^ key = byte
     *   - 密鑰循環使用（key[i % key.size()]），支援任意長度密鑰
     *   - 「只加密標頭字元欄位，不加密壓縮位元流」，節省 IoT CPU 週期
     *   - 複雜度：O(k)，k 為相異字元數量
     *
     * @param ch      要加密/解密的字元值（原始字元 Byte）
     * @param key     密鑰字串
     * @param key_idx 當前消耗到的密鑰索引（加密多個字元時累加）
     * @return 加密/解密後的字元 Byte
     */
    uint8_t xorObfuscate(uint8_t ch,
                         const std::string& key,
                         size_t key_idx) const;


    // =========================================================================
    // 私有方法 - 第四階段：標頭序列化（Header I/O）
    // =========================================================================

    /**
     * @brief 將加密後的 Huffman 字典序列化寫入壓縮檔標頭
     *
     * 標頭格式（Binary Layout）：
     * ┌─────────────────────────────────────────────────────┐
     * │ [4 bytes] magic number  : 0x48474400 ("HGD\0")     │ ← 檔案識別碼
     * │ [1 byte]  entry_count   : 字典條目數量（最多 256）  │
     * │ [1 byte]  padding_bits  : 最後一個 Byte 的填充位元數│
     * │ 重複 entry_count 次：                               │
     * │   [1 byte] xor_ch      : XOR 加密後的字元值         │
     * │   [1 byte] code_length : 編碼長度（bits）           │
     * │   [N bytes] code_bits  : 編碼位元（ceil(len/8)）    │
     * └─────────────────────────────────────────────────────┘
     *
     * @param out 輸出檔案串流
     * @param key XOR 加密密鑰
     * @param padding_bits 最後一個壓縮 Byte 的無效填充位元數
     */
    void writeHeader(std::ofstream& out,
                     const std::string& key,
                     uint8_t padding_bits);

    /**
     * @brief 從壓縮檔讀取並 XOR 解密標頭，重建 code_table_ 與 Huffman Tree
     *
     * 【無回饋防禦機制的關鍵實作點】：
     *   - 無論 key 是否正確，此函式必須正常返回（不 throw、不 return false）
     *   - 若 XOR 解密後的字元值在正常範圍外，仍照常插入 code_table_
     *   - 後續解碼步驟使用可能錯誤的 tree，輸出亂碼而非錯誤訊息
     *
     * @param in          輸入壓縮檔串流
     * @param key         XOR 解密密鑰（可能正確或錯誤）
     * @param padding_bits 輸出：最後一個壓縮 Byte 的填充位元數
     */
    void readHeader(std::ifstream& in,
                    const std::string& key,
                    uint8_t& padding_bits);

    /**
     * @brief 依據 code_table_ 重建 Huffman Tree（解壓縮用）
     *
     * 運作邏輯：
     *   - 反向操作 generateCodes()：依字典中每筆 (字元, 編碼) 重新插入樹節點
     *   - 遇到已存在的路徑節點則不重複建立
     *   - 此函式在密鑰錯誤時仍會執行，建出一棵「語義錯誤」的樹
     */
    void rebuildTreeFromCodeTable();
};