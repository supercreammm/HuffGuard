// =============================================================================
// 第一部分：壓縮相關實作
// =============================================================================

#include "HuffGuardEngine.h"
#include <iostream>
#include <stdexcept>
#include <cmath>    // ceil()

// =============================================================================
// Section 1：頻率統計
// =============================================================================

/**
 * 讀取輸入檔案，統計每個 ASCII 字元的出現頻率。
 * 以二進位模式開啟，避免作業系統對 '\n' 做換行轉換（Windows CRLF 問題）。
 */
bool HuffGuardEngine::buildFrequencyTable(const std::string& input_path) {
    // --- 確認檔案可以開啟 ---
    std::ifstream in(input_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "[錯誤] 無法開啟輸入檔案：" << input_path << "\n";
        return false;
    }

    freq_table_.clear();

    // 每次讀取 1 個 Byte，強制轉型為 uint8_t 以防止 char 帶符號擴展問題
    // 例如：char c = 0xFF → int 值可能變成 -1（signed char），
    //        轉成 uint8_t 後固定為 255，確保作為 map key 時不越界
    char raw_byte;
    while (in.get(raw_byte)) {
        uint8_t byte = static_cast<uint8_t>(raw_byte);
        freq_table_[byte]++;   // 不存在的 key 會被零初始化後 +1
    }

    in.close();

    if (freq_table_.empty()) {
        std::cerr << "[錯誤] 輸入檔案為空，無法建立頻率表。\n";
        return false;
    }

    return true;
}


// =============================================================================
// Section 2：建構 Huffman Tree（Min-Heap）
// =============================================================================

/**
 *
 * 演算法步驟：
 *   ① 為每個字元建立葉節點，全部推入 Min-Heap
 *   ② 重複：取出頻率最小的兩個節點 (left, right)
 *            建立內部節點（freq = left->freq + right->freq）
 *            將內部節點推回 Heap
 *   ③ Heap 剩一個節點時，即為 tree_root_
 *
 * 特殊情況：只有一種字元時，Heap 僅剩根節點（葉節點），
 *           此時該字元編碼為 "0"（特殊處理見 generateCodes）
 */
void HuffGuardEngine::buildHuffmanTree() {
    // 清除舊樹，避免多次呼叫 compress() 時殘留舊狀態
    tree_root_.reset();

    MinHeap heap;

    // ① 將所有字元各建一個葉節點推入 Min-Heap
    for (const auto& pair : freq_table_) {
        heap.push(std::make_unique<HuffNode>(pair.first, pair.second));
    }

    // ② Heap 只剩一個節點時停止合併（此時即為根節點）
    while (heap.size() > 1) {
        // priority_queue 不支援 move top()，需先 const_cast 取得非常數引用
        // 再 move 出來釋放所有權，最後 pop() 移除空殼
        // C++11 標準作法：top() 回傳 const ref，需配合 const_cast + move

        // 取出頻率最小的節點（左子節點）
        auto left = std::move(const_cast<std::unique_ptr<HuffNode>&>(heap.top()));
        heap.pop();

        // 取出第二小的節點（右子節點）
        auto right = std::move(const_cast<std::unique_ptr<HuffNode>&>(heap.top()));
        heap.pop();

        // 合併：建立內部節點，頻率為兩子節點之和
        uint32_t merged_freq = left->freq + right->freq;
        auto parent = std::make_unique<HuffNode>(
            merged_freq,
            std::move(left),
            std::move(right)
        );

        heap.push(std::move(parent));
    }

    // ③ Heap 最後剩下的就是根節點
    if (!heap.empty()) {
        tree_root_ = std::move(
            const_cast<std::unique_ptr<HuffNode>&>(heap.top())
        );
        heap.pop();
    }
}


// =============================================================================
// Section 3：遞迴走訪產生 Huffman 編碼字典
// =============================================================================

/**
 * 遞迴後序走訪 Huffman Tree，產生每個字元的可變長度二進位編碼。
 *
 * 路徑規則：
 *   - 走向左子節點 → 路徑附加 '0'
 *   - 走向右子節點 → 路徑附加 '1'
 *   - 抵達葉節點   → 將 (字元, 完整路徑) 寫入 code_table_
 *
 * 特殊情況：若整棵樹只有根節點（單一字元），強制給予編碼 "0"
 */
void HuffGuardEngine::generateCodes(const HuffNode* node,
                                    const std::string& current) {
    if (node == nullptr) return;

    // 抵達葉節點：記錄編碼
    if (node->is_leaf) {
        // 特殊情況：樹只有一個節點（單一字元），路徑為空 → 給予 "0"
        code_table_[node->ch] = current.empty() ? "0" : current;
        return;
    }

    // 向左走：位元 '0'
    generateCodes(node->left.get(),  current + "0");

    // 向右走：位元 '1'
    generateCodes(node->right.get(), current + "1");
}


// =============================================================================
// Section 4：Bit-level I/O
// =============================================================================

/**
 * 將單一位元（0 或 1）寫入輸出串流。
 *
 * 位元緩衝邏輯（MSB First）：
 *
 *   bit_buffer_ 初始為 0b00000000，bit_count_ = 0
 *
 *   每次呼叫 writeBit(b)：
 *     ① 將 bit_buffer_ 左移一位：騰出最低位給新的位元
 *        bit_buffer_ <<= 1
 *        例：0b00000000 << 1 = 0b00000000（初始）
 *            0b01000000 << 1 = 0b10000000（累積中）
 *
 *     ② 將新位元 OR 進最低位：
 *        bit_buffer_ |= (b & 0x01)
 *        &0x01 確保只取 b 的最低位，防止傳入非 0/1 的雜訊值
 *
 *     ③ bit_count_++ → 當達到 8 時，呼叫 flushBitBuffer() 寫出
 *
 *   範例：依序寫入 1,0,1,1,0,0,1,0
 *     步驟 1: buffer = 0b00000001, count=1
 *     步驟 2: buffer = 0b00000010, count=2
 *     步驟 3: buffer = 0b00000101, count=3
 *     步驟 4: buffer = 0b00001011, count=4
 *     步驟 5: buffer = 0b00010110, count=5
 *     步驟 6: buffer = 0b00101100, count=6
 *     步驟 7: buffer = 0b01011001, count=7
 *     步驟 8: buffer = 0b10110010, count=8 → flush → 寫出 0xB2
 */
void HuffGuardEngine::writeBit(uint8_t bit, std::ofstream& out) {
    // ① 左移騰位
    bit_buffer_ <<= 1;

    // ② OR 進新位元（& 0x01 防止雜訊污染高位）
    bit_buffer_ |= (bit & 0x01);

    // ③ 計數並在累積滿 8 bits 後寫出
    bit_count_++;
    if (bit_count_ == 8) {
        out.put(static_cast<char>(bit_buffer_));
        bit_buffer_ = 0;
        bit_count_  = 0;
    }
}

/**
 * 將 bit_buffer_ 中尚未滿 8 bits 的剩餘位元補零後強制寫出。
 *
 * 補零（Padding）邏輯：
 *   若已累積 bit_count_ = 3 bits（例如 0b00000101）
 *   需左移 (8 - 3) = 5 位，變成 0b10100000
 *   補了 5 個 0 在低位 → padding_bits = 5
 *
 * 此 padding_bits 值必須寫入檔案標頭，解壓縮時用來截斷最後一個 Byte 的無效位元。
 *
 * @return 實際填充的位元數（0 代表無需填充，buffer 恰好為空）
 */
void HuffGuardEngine::flushBitBuffer(std::ofstream& out) {
    if (bit_count_ > 0) {
        // 將剩餘位元推到最高位（MSB side），低位自動補 0
        // 例：bit_count_=3, buffer=0b00000101
        //     << (8-3) = << 5 → 0b10100000
        bit_buffer_ <<= (8 - bit_count_);
        out.put(static_cast<char>(bit_buffer_));

        bit_buffer_ = 0;
        bit_count_  = 0;
    }
}


// =============================================================================
// Section 5：XOR 靶向混淆（僅作用於標頭字元欄位）
// =============================================================================

/**
 * 對單一字元執行 XOR 加密/解密。
 *
 * 數學性質：XOR 為對稱運算
 *   加密：plaintext  ^ key_byte = ciphertext
 *   解密：ciphertext ^ key_byte = plaintext  (完全相同的運算)
 *
 * 密鑰循環：key_idx % key.size() 使密鑰可無限循環使用
 *   例：key = "AB"（0x41, 0x42），加密 5 個字元時：
 *   ch[0]^0x41, ch[1]^0x42, ch[2]^0x41, ch[3]^0x42, ch[4]^0x41
 *
 * 注意：此函式刻意不對壓縮位元流進行任何操作，
 *       確保 IoT 設備的 CPU 資源只用在最必要的解密上。
 */
uint8_t HuffGuardEngine::xorObfuscate(uint8_t ch,
                                       const std::string& key,
                                       size_t key_idx) const {
    // 防止空密鑰導致除以零
    if (key.empty()) return ch;

    // 取得當前輪次的密鑰位元組
    uint8_t key_byte = static_cast<uint8_t>(key[key_idx % key.size()]);

    // XOR 運算：加解密對稱，同一函式同時承擔兩個方向
    return ch ^ key_byte;
}


// =============================================================================
// Section 6：標頭序列化（writeHeader）
// =============================================================================

/**
 * 將加密後的 Huffman 字典序列化寫入壓縮檔標頭。
 *
 * 標頭 Binary Layout（Big-Endian）：
 * ┌──────────────────────────────────────────────────────────────┐
 * │ Offset │ 大小    │ 欄位名稱      │ 說明                      │
 * ├──────────────────────────────────────────────────────────────┤
 * │  0     │ 4 bytes │ magic        │ 0x48 0x47 0x44 0x00 "HGD" │
 * │  4     │ 2 bytes │ entry_count  │ 字典條目數（最多 256）     │
 * │  6     │ 1 byte  │ padding_bits │ 最後一個壓縮 Byte 的填充數 │
 * │  7     │ 重複：                                              │
 * │        │ 1 byte  │ xor_ch       │ XOR 加密後的字元值         │
 * │        │ 1 byte  │ code_length  │ 編碼長度（單位：bits）     │
 * │        │ N bytes │ code_bits    │ 編碼位元，右補零至整 Byte  │
 * └──────────────────────────────────────────────────────────────┘
 *
 * code_bits 儲存方式：
 *   例：編碼 "101" (3 bits) → 儲存為 1 byte：0b10100000（右補 5 個 0）
 *   解壓縮時透過 code_length 得知有效位元數，忽略補零部分
 */
void HuffGuardEngine::writeHeader(std::ofstream& out,
                                   const std::string& key,
                                   uint8_t padding_bits) {
    // --- Magic Number："HGD\0" (4 bytes) ---
    const uint8_t magic[4] = {0x48, 0x47, 0x44, 0x00};
    out.write(reinterpret_cast<const char*>(magic), 4);

    // --- 字典條目數（2 bytes, Big-Endian） ---
    // 使用 uint16_t 支援最多 256 個條目（ASCII 範圍）
    uint16_t entry_count = static_cast<uint16_t>(code_table_.size());
    // Big-Endian 拆解：先寫高位 Byte，再寫低位 Byte
    out.put(static_cast<char>((entry_count >> 8) & 0xFF)); // 高 8 bits
    out.put(static_cast<char>( entry_count       & 0xFF)); // 低 8 bits

    // --- 填充位元數（1 byte） ---
    out.put(static_cast<char>(padding_bits));

    // --- 字典條目（每個字元一筆） ---
    size_t key_idx = 0; // 密鑰索引，跨條目持續累加，確保每個字元使用不同的密鑰位元組

    for (const auto& entry : code_table_) {
        uint8_t     original_ch  = entry.first;          // 原始字元
        const std::string& code  = entry.second;         // 編碼字串（"0110..."）
        uint8_t     code_len     = static_cast<uint8_t>(code.size()); // 編碼長度

        // XOR 加密字元欄位（資料本體不加密，僅此處處理）
        uint8_t encrypted_ch = xorObfuscate(original_ch, key, key_idx++);

        // 寫出加密後的字元
        out.put(static_cast<char>(encrypted_ch));

        // 寫出編碼長度（1 byte，最長 255 bits，IoT 場景下足夠）
        out.put(static_cast<char>(code_len));

        // 將編碼字串（"0110..."）打包成位元組序列寫出
        // 每 8 個字元為一個 Byte，不足 8 個右補零
        uint8_t  packed_byte = 0;
        uint8_t  filled_bits = 0;

        for (char bit_char : code) {
            // ① 左移騰位
            packed_byte <<= 1;

            // ② OR 進當前位元（'1' - '0' 取值為 0 或 1）
            packed_byte |= static_cast<uint8_t>(bit_char - '0');

            filled_bits++;
            if (filled_bits == 8) {
                out.put(static_cast<char>(packed_byte));
                packed_byte = 0;
                filled_bits = 0;
            }
        }

        // 寫出剩餘不足 8 bits 的部分（右補零）
        if (filled_bits > 0) {
            // 剩餘位元推到高位，低位自動補零
            packed_byte <<= (8 - filled_bits);
            out.put(static_cast<char>(packed_byte));
        }
    }
}


// =============================================================================
// Section 7：compress 主流程
// =============================================================================

/**
 * 壓縮主流程（整合所有壓縮步驟）。
 *
 * 執行順序：
 *   ① buildFrequencyTable  → 統計字元頻率
 *   ② buildHuffmanTree     → 建構最優前綴碼樹
 *   ③ generateCodes        → 走訪樹產生字典
 *   ④ 第一遍掃描輸入檔      → 計算完整壓縮位元數（用於計算 padding_bits）
 *   ⑤ writeHeader          → 寫出加密標頭
 *   ⑥ 第二遍掃描輸入檔      → 將每個字元的編碼透過 writeBit() 寫出
 *   ⑦ flushBitBuffer       → 補零並寫出最後一個不完整的 Byte
 *
 * 為何需要兩遍掃描？
 *   padding_bits 需要在寫標頭時就確定，但其值取決於壓縮後的總位元數。
 *   第一遍只計算總位元數（不寫出），確定 padding_bits 後才寫標頭，
 *   第二遍才真正輸出壓縮位元流。
 */
void HuffGuardEngine::compress(const std::string& input_path,
                                const std::string& output_path,
                                const std::string& key) {
    // ① 建立頻率表
    if (!buildFrequencyTable(input_path)) {
        return; // 錯誤訊息已在 buildFrequencyTable 內輸出
    }

    // ② 建構 Huffman Tree
    buildHuffmanTree();

    // ③ 產生字典
    code_table_.clear();
    generateCodes(tree_root_.get(), "");

    // ④ 第一遍掃描：計算壓縮後的總位元數，推算 padding_bits
    std::ifstream first_pass(input_path, std::ios::binary);
    if (!first_pass.is_open()) {
        std::cerr << "[錯誤] 無法開啟輸入檔案（第一遍掃描）：" << input_path << "\n";
        return;
    }

    uint64_t total_bits = 0;
    {
        char raw_byte;
        while (first_pass.get(raw_byte)) {
            uint8_t byte = static_cast<uint8_t>(raw_byte);
            auto it = code_table_.find(byte);
            if (it != code_table_.end()) {
                total_bits += it->second.size();
            }
        }
    }
    first_pass.close();

    // padding_bits = 最後一個 Byte 中無效的補零位元數
    // 例：total_bits = 13 → 13 % 8 = 5 有效位 → padding = 8 - 5 = 3
    //     total_bits = 16 → 16 % 8 = 0 → 無需補零 → padding = 0
    uint8_t remainder    = static_cast<uint8_t>(total_bits % 8);
    uint8_t padding_bits = (remainder == 0) ? 0 : (8 - remainder);

    // ⑤ 開啟輸出檔案並寫出加密標頭
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[錯誤] 無法建立輸出檔案：" << output_path << "\n";
        return;
    }

    writeHeader(out, key, padding_bits);

    // ⑥ 第二遍掃描：真正輸出壓縮位元流
    // 重置 Bit 緩衝器，確保從乾淨狀態開始寫壓縮資料
    bit_buffer_ = 0;
    bit_count_  = 0;

    std::ifstream second_pass(input_path, std::ios::binary);
    if (!second_pass.is_open()) {
        std::cerr << "[錯誤] 無法開啟輸入檔案（第二遍掃描）：" << input_path << "\n";
        out.close();
        return;
    }

    {
        char raw_byte;
        while (second_pass.get(raw_byte)) {
            uint8_t byte = static_cast<uint8_t>(raw_byte);
            const std::string& code = code_table_.at(byte);
                // .at() 在此處安全：字元必定在字典中（來源與建樹相同）

            for (char bit_char : code) {
                // 將編碼字元 '0' 或 '1' 轉為位元值後寫入
                writeBit(static_cast<uint8_t>(bit_char - '0'), out);
            }
        }
    }
    second_pass.close();

    // ⑦ 清空位元緩衝（補零並寫出最後一個 Byte）
    flushBitBuffer(out);

    out.close();

    // --- 壓縮完成，輸出摘要資訊 ---
    std::cout << "[HuffGuard] 壓縮完成\n"
              << "  輸入：" << input_path  << "\n"
              << "  輸出：" << output_path << "\n"
              << "  字典大小：" << code_table_.size() << " 個字元\n"
              << "  壓縮位元數：" << total_bits
              << "（填充 " << static_cast<int>(padding_bits) << " bits）\n";
}

// =============================================================================
// HuffGuardEngine.cpp  (第二部分：解壓縮與無回饋防禦機制)
// =============================================================================

// =============================================================================
// Section 8：Bit-level 讀取
// =============================================================================

/**
 * 從輸入串流逐位元讀取，每次回傳一個位元（0 或 1）。
 *
 * 讀取邏輯（MSB First，與 writeBit 對稱）：
 *
 *   bit_buffer_ 儲存當前尚未消耗的 Byte，
 *   bit_count_  記錄 buffer 中還剩幾個有效位元。
 *
 *   每次呼叫：
 *     ① 若 bit_count_ == 0，從串流讀入下一個 Byte
 *     ② 取出最高位元（bit 7）：
 *        out_bit = (bit_buffer_ >> 7) & 0x01
 *        >> 7 將最高位移到最低位，& 0x01 遮蔽其他位
 *     ③ 左移一位，使次高位成為新的最高位，準備下次讀取
 *        bit_buffer_ <<= 1
 *     ④ bit_count_--
 *
 *   範例：讀取 0b10110010 這個 Byte
 *     第1次: out=1, buffer=0b01100100, count=7
 *     第2次: out=0, buffer=0b11001000, count=6
 *     第3次: out=1, buffer=0b10010000, count=5
 *     第4次: out=1, buffer=0b00100000, count=4
 *     ...依此類推，還原出原始位元序列 1,0,1,1,0,0,1,0
 */
bool HuffGuardEngine::readBit(std::ifstream& in, uint8_t& out_bit) {
    // ① buffer 耗盡時，從串流讀入下一個 Byte
    if (bit_count_ == 0) {
        char raw_byte;
        if (!in.get(raw_byte)) {
            return false; // EOF：無更多資料
        }
        bit_buffer_ = static_cast<uint8_t>(raw_byte);
        bit_count_  = 8;
    }

    // ② 取出最高位元（MSB）
    //    >> 7：將 bit[7] 移到 bit[0] 位置
    //    & 0x01：確保只保留最低位，其餘清零
    out_bit = (bit_buffer_ >> 7) & 0x01;

    // ③ 左移一位，使 bit[6] 成為新的 bit[7]（下次讀取的最高位）
    bit_buffer_ <<= 1;

    // ④ 消耗一個位元
    bit_count_--;

    return true;
}


// =============================================================================
// Section 9：標頭反序列化（readHeader）
//   無回饋防禦機制核心所在 
// =============================================================================

/**
 * 從壓縮檔讀取標頭，並以 XOR 解密重建 code_table_。
 *
 * 【無回饋安全設計原則】：
 *   此函式刻意設計為「永不驗證密鑰」。
 *   無論 XOR 解密後的字元值為何（包含控制字元、不可見字元、
 *   甚至重複的 key），一律直接存入 code_table_，不做任何驗證。
 *   
 *   攻擊者無論輸入什麼密鑰，系統行為完全一致：
 *     正確密鑰 → 解碼出原文
 *     錯誤密鑰 → 解碼出亂碼
 *   兩種情況的程式流程、執行時間、回傳值完全相同，
 *   攻擊者無從透過程式行為判斷密鑰是否正確。
 */
void HuffGuardEngine::readHeader(std::ifstream& in,
                                  const std::string& key,
                                  uint8_t& padding_bits) {
    // --- 讀取並驗證 Magic Number（唯一允許報錯的地方）---
    // 此處驗證是用來識別檔案格式，與密鑰無關，不違反無回饋原則
    uint8_t magic[4] = {0};
    in.read(reinterpret_cast<char*>(magic), 4);

    if (magic[0] != 0x48 || magic[1] != 0x47 ||
        magic[2] != 0x44 || magic[3] != 0x00) {
        std::cerr << "[錯誤] 不是有效的 HGD 壓縮檔（Magic Number 不符）。\n";
        return;
    }

    // --- 讀取字典條目數（2 bytes, Big-Endian） ---
    uint8_t count_high = 0, count_low = 0;
    in.get(reinterpret_cast<char&>(count_high));
    in.get(reinterpret_cast<char&>(count_low));
    // Big-Endian 組合：高位 Byte 左移 8 位 OR 低位 Byte
    uint16_t entry_count = (static_cast<uint16_t>(count_high) << 8)
                         |  static_cast<uint16_t>(count_low);

    // --- 讀取填充位元數（1 byte） ---
    uint8_t pad = 0;
    in.get(reinterpret_cast<char&>(pad));
    padding_bits = pad;

    // --- 清空舊字典，準備重建 ---
    code_table_.clear();

    // --- 逐條讀取字典條目 ---
    size_t key_idx = 0; // 與 writeHeader 的密鑰索引同步遞增

    for (uint16_t i = 0; i < entry_count; i++) {
        // 讀取加密後的字元（1 byte）
        uint8_t encrypted_ch = 0;
        if (!in.get(reinterpret_cast<char&>(encrypted_ch))) break;

        // =====================================================================
        // 【無回饋亂碼陷阱 - 觸發點 A】
        // XOR 解密：無論 key 正確與否，強制執行，結果直接使用
        // 正確密鑰 → 還原出原始字元
        // 錯誤密鑰 → 產生任意字元（可能是控制字元、亂碼、甚至 '\0'）
        // 程式不進行任何驗證，一律接受此結果
        // =====================================================================
        uint8_t decoded_ch = xorObfuscate(encrypted_ch, key, key_idx++);

        // 讀取編碼長度（1 byte）
        uint8_t code_len = 0;
        if (!in.get(reinterpret_cast<char&>(code_len))) break;

        // 讀取打包的編碼位元組（ceil(code_len / 8) 個 Bytes）
        uint8_t bytes_to_read = (code_len + 7) / 8; // ceiling division

        std::string code_str;
        code_str.reserve(code_len);

        for (uint8_t b = 0; b < bytes_to_read; b++) {
            uint8_t packed_byte = 0;
            if (!in.get(reinterpret_cast<char&>(packed_byte))) break;

            // 從這個 Byte 中取出位元，從 MSB 開始
            // 每個 Byte 最多貢獻 8 個位元，但最後一個 Byte 可能只有部分有效
            uint8_t bits_in_this_byte = 8;
            if (b == bytes_to_read - 1) {
                // 最後一個 Byte：實際有效位元數 = code_len - 已處理位元數
                uint8_t already_processed = b * 8;
                bits_in_this_byte = code_len - already_processed;
            }

            for (uint8_t bit_pos = 0; bit_pos < bits_in_this_byte; bit_pos++) {
                // 從 MSB 逐位取出：
                // bit_pos=0 → 取 bit[7]：(packed_byte >> 7) & 0x01
                // bit_pos=1 → 取 bit[6]：(packed_byte >> 6) & 0x01
                // 通式：(packed_byte >> (7 - bit_pos)) & 0x01
                uint8_t bit_val = (packed_byte >> (7 - bit_pos)) & 0x01;
                code_str += static_cast<char>('0' + bit_val);
            }
        }

        // =====================================================================
        // 【無回饋亂碼陷阱 - 觸發點 B】
        // 將解密後的字元與編碼對應關係寫入 code_table_。
        // 不檢查：
        //   - decoded_ch 是否為可見字元
        //   - decoded_ch 是否已存在於 code_table_（重複 key）
        //   - code_str 是否為合法的前綴碼
        // 若密鑰錯誤導致多個加密字元解出相同值，後者覆蓋前者，
        // 進一步製造亂碼效果，同時不 crash。
        // =====================================================================
        code_table_[decoded_ch] = code_str;
    }
}


// =============================================================================
// Section 10：由 code_table_ 重建 Huffman Tree
// =============================================================================

/**
 * 依據 code_table_ 中的 (字元, 編碼) 對，逐一插入節點重建 Huffman Tree。
 *
 * 建樹邏輯：
 *   對每筆 (ch, "0110...") 編碼：
 *     從根節點出發，依序讀取每個位元：
 *       '0' → 走向（或建立）左子節點
 *       '1' → 走向（或建立）右子節點
 *     路徑走完後，將當前節點標記為葉節點，儲存字元 ch
 *
 * 【無回饋設計】：
 *   此函式不驗證前綴碼的合法性。
 *   若 readHeader 因密鑰錯誤產生了衝突的編碼表（例如兩個字元共用相同路徑），
 *   後插入的字元會覆蓋先前的葉節點，建出一棵「語義錯誤」的樹。
 *   此樹仍可用於解碼（不會 crash），但輸出必為亂碼。
 */
void HuffGuardEngine::rebuildTreeFromCodeTable() {
    // 建立新的根節點（內部節點，freq 填 0 即可，解碼時不使用 freq）
    tree_root_ = std::make_unique<HuffNode>(0u,
                     std::unique_ptr<HuffNode>(nullptr),
                     std::unique_ptr<HuffNode>(nullptr));
    tree_root_->is_leaf = false;

    for (const auto& entry : code_table_) {
        uint8_t            ch   = entry.first;
        const std::string& code = entry.second;

        HuffNode* current = tree_root_.get();

        for (size_t i = 0; i < code.size(); i++) {
            bool is_last_bit = (i == code.size() - 1);
            char bit         = code[i];

            if (bit == '0') {
                // 左子節點不存在則建立
                if (!current->left) {
                    current->left = std::make_unique<HuffNode>(0u,
                        std::unique_ptr<HuffNode>(nullptr),
                        std::unique_ptr<HuffNode>(nullptr));
                    current->left->is_leaf = false;
                }

                if (is_last_bit) {
                    // =========================================================
                    // 【無回饋亂碼陷阱 - 觸發點 C】
                    // 若此路徑已存在葉節點（因密鑰錯誤導致編碼衝突），
                    // 直接覆蓋字元值，不報錯、不拒絕插入。
                    // =========================================================
                    current->left->is_leaf = true;
                    current->left->ch      = ch;
                } else {
                    current = current->left.get();
                }
            } else { // bit == '1'
                // 右子節點不存在則建立
                if (!current->right) {
                    current->right = std::make_unique<HuffNode>(0u,
                        std::unique_ptr<HuffNode>(nullptr),
                        std::unique_ptr<HuffNode>(nullptr));
                    current->right->is_leaf = false;
                }

                if (is_last_bit) {
                    // =========================================================
                    // 【無回饋亂碼陷阱 - 觸發點 D】
                    // 同上，右側路徑衝突時直接覆蓋，不中斷程式。
                    // =========================================================
                    current->right->is_leaf = true;
                    current->right->ch      = ch;
                } else {
                    current = current->right.get();
                }
            }
        }
    }
}


// =============================================================================
// Section 11：decompress 主流程
// =============================================================================

/**
 * 解壓縮主流程。
 *
 * 執行順序：
 *   ① readHeader           → 讀取並 XOR 解密標頭，重建 code_table_
 *   ② rebuildTreeFromCodeTable → 由 code_table_ 重建 Huffman Tree
 *   ③ 逐位元讀取壓縮流     → 沿樹走訪，到達葉節點時輸出字元
 *   ④ 最後一個 Byte 的處理 → 依 padding_bits 截斷無效位元
 *
 * 【無回饋防禦 - 整體保證】：
 *   無論密鑰正確與否，此函式必定完整執行並產出輸出檔案。
 *   正確密鑰 → 輸出還原的原始內容
 *   錯誤密鑰 → 輸出一個大小相近、內容充滿亂碼的假檔案
 *   攻擊者無法從程式行為（執行時間、輸出大小、有無報錯）判斷密鑰正確性。
 */
void HuffGuardEngine::decompress(const std::string& input_path,
                                  const std::string& output_path,
                                  const std::string& key) {
    // --- 防呆：開啟輸入檔案 ---
    std::ifstream in(input_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "[錯誤] 無法開啟壓縮檔：" << input_path << "\n";
        return;
    }

    // ① 讀取標頭（含 XOR 解密，無論密鑰對錯皆強制執行）
    uint8_t padding_bits = 0;
    readHeader(in, key, padding_bits);

    // 若標頭讀取後 code_table_ 為空（例如 Magic Number 錯誤），
    // 此處仍嘗試繼續（但因樹無法建立，輸出必為空白或亂碼）
    // 不做 early return，維持一致的程式行為
    if (code_table_.empty()) {
        std::cerr << "[警告] 字典為空，輸出將為空白。\n";
    }

    // ② 由（可能錯誤的）code_table_ 重建 Huffman Tree
    rebuildTreeFromCodeTable();

    // --- 防呆：開啟輸出檔案 ---
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[錯誤] 無法建立輸出檔案：" << output_path << "\n";
        in.close();
        return;
    }

    // ③ 逐位元讀取並沿 Huffman Tree 走訪解碼
    // 重置 Bit 讀取緩衝器（重用壓縮時的 bit_buffer_ / bit_count_ 成員）
    bit_buffer_ = 0;
    bit_count_  = 0;

    // 需要知道壓縮資料本體的總位元數，以便在最後一個 Byte 截斷 padding。
    // 做法：記錄目前在壓縮資料串流中的位元消耗量，
    //       並預先計算資料本體的 Byte 總數，
    //       在最後一個 Byte 只讀取 (8 - padding_bits) 個有效位元。
    //
    // 為了簡化實作，改採「預讀整個資料本體至 buffer」的策略：
    // 先讀取所有剩餘 Bytes 存入向量，計算總有效位元數後再解碼。
    std::vector<uint8_t> compressed_bytes;
    {
        char raw_byte;
        while (in.get(raw_byte)) {
            compressed_bytes.push_back(static_cast<uint8_t>(raw_byte));
        }
    }
    in.close();

    // 計算總有效位元數：總 Bytes * 8 - padding_bits
    if (compressed_bytes.empty()) {
        std::cerr << "[警告] 壓縮資料本體為空。\n";
        out.close();
        return;
    }

    uint64_t total_valid_bits =
        static_cast<uint64_t>(compressed_bytes.size()) * 8
        - static_cast<uint64_t>(padding_bits);

    // 逐位元解碼
    const HuffNode* current_node = tree_root_.get();
    uint64_t bits_consumed = 0;

    // 外層：位元組迴圈
    for (size_t byte_idx = 0;
         byte_idx < compressed_bytes.size() && bits_consumed < total_valid_bits;
         byte_idx++) {

        uint8_t packed_byte = compressed_bytes[byte_idx];

        // 內層：位元迴圈（MSB First）
        for (int bit_pos = 7; bit_pos >= 0 && bits_consumed < total_valid_bits; bit_pos--) {

            // 取出當前位元：右移 bit_pos 位後取最低位
            uint8_t bit = (packed_byte >> bit_pos) & 0x01;
            bits_consumed++;

            // 沿 Huffman Tree 走訪
            if (bit == 0) {
                // =============================================================
                // 【無回饋亂碼陷阱 - 觸發點 E】
                // 密鑰錯誤時，樹的結構與原本不符。
                // 若走到了一個 nullptr（路徑在錯誤的樹中不存在），
                // 不 throw、不 return，改為：
                //   ① 輸出預設亂碼字元 '?'
                //   ② 重置回根節點，繼續解碼下一段位元
                // 最終輸出一個大小相近但充滿 '?' 和錯誤字元的假檔案
                // =============================================================
                if (current_node->left == nullptr) {
                    // 路徑不存在：輸出亂碼佔位字元，重置至根節點
                    out.put('?');
                    current_node = tree_root_.get();
                    continue;
                }
                current_node = current_node->left.get();
            } else {
                // =============================================================
                // 【無回饋亂碼陷阱 - 觸發點 F】
                // 右側路徑同上，nullptr 時輸出 '?' 並重置，不中斷。
                // =============================================================
                if (current_node->right == nullptr) {
                    out.put('?');
                    current_node = tree_root_.get();
                    continue;
                }
                current_node = current_node->right.get();
            }

            // 抵達葉節點：輸出字元，重置至根節點
            if (current_node->is_leaf) {
                out.put(static_cast<char>(current_node->ch));
                current_node = tree_root_.get();
            }
        }
    }

    out.close();

    // --- 解壓縮完成，輸出摘要資訊 ---
    std::cout << "[HuffGuard] 解壓縮完成\n"
              << "  輸入：" << input_path  << "\n"
              << "  輸出：" << output_path << "\n"
              << "  處理位元數：" << total_valid_bits
              << "（忽略填充 " << static_cast<int>(padding_bits) << " bits）\n";
}