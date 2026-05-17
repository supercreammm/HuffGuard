import customtkinter as ctk
from tkinter import filedialog, messagebox
import subprocess
import os
import threading  # 引入多執行緒模組
import time;

# ================= 系統外觀設定 =================
ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("blue")

ENGINE_EXE = "main.exe"

def select_file():
    filepath = filedialog.askopenfilename(title="選擇目標檔案")
    if filepath:
        entry_file.delete(0, ctk.END)
        entry_file.insert(0, filepath)

def run_engine_async(mode):
    """
    這個函式負責檢查輸入，並啟動背景執行緒，以免卡死 UI 畫面。
    """
    input_file = entry_file.get()
    key = entry_key.get()

    if not input_file or not os.path.exists(input_file):
        messagebox.showerror("錯誤", "請選擇有效的輸入檔案！")
        return
    if not key:
        messagebox.showerror("錯誤", "安全密鑰不能為空！")
        return

    # 1. 鎖定 UI 介面，避免使用者重複點擊
    btn_compress.configure(state="disabled", text="⏳ 處理中...")
    btn_decompress.configure(state="disabled", text="⏳ 處理中...")
    
    # 2. 顯示並啟動進度條動畫
    progress_bar.set(0)
    progress_bar.pack(pady=(0, 15)) # 讓進度條顯示出來
    progress_bar.start()

    # 3. 啟動背景小幫手去跑 C++ 引擎
    thread = threading.Thread(target=engine_worker, args=(mode, input_file, key))
    thread.daemon = True # 設定為守護執行緒，主程式關閉時它也會自動結束
    thread.start()

def engine_worker(mode, input_file, key):
    """
    背景執行緒，專門呼叫 C++ 引擎。
    使用強制 UTF-8 解碼，並加入 Python 精準計時器。
    """
    if mode == "compress":
        output_file = input_file + ".hgd"
    else:
        if input_file.endswith(".hgd"):
            output_file = input_file[:-4] + "_decoded.txt"
        else:
            output_file = input_file + "_decoded.txt"

    try:
        # --- 1. 按下碼錶開始計時 ---
        start_time = time.time() 

        # 呼叫 C++ 引擎 (只保留這裡呼叫一次！)
        result = subprocess.run(
            [f"./{ENGINE_EXE}", mode, input_file, output_file, key],
            capture_output=True, check=True
        )
        
        # --- 2. 按下碼錶停止計時，並算出毫秒 ---
        end_time = time.time()
        elapsed_ms = round((end_time - start_time) * 1000) 
        
        # --- 3. 把算出來的時間，加到原本的對話框文字裡 ---
        stdout_text = result.stdout.decode('utf-8', errors='replace')
        stdout_text += f"\n\n執行耗時：{elapsed_ms} 毫秒"
        
        # 任務成功，呼叫 UI 更新函式
        root.after(0, on_engine_finish, True, output_file, stdout_text, None)
        
    except subprocess.CalledProcessError as e:
        err_text = e.stderr.decode('utf-8', errors='replace')
        root.after(0, on_engine_finish, False, None, None, f"底層運算失敗：\n{err_text}")
    except FileNotFoundError:
        root.after(0, on_engine_finish, False, None, None, f"找不到 {ENGINE_EXE}！\n請確保 C++ 執行檔與此程式放在同一目錄下。")
    except Exception as e:
        root.after(0, on_engine_finish, False, None, None, f"發生未知的系統錯誤：\n{str(e)}")

def on_engine_finish(success, output_file, stdout, error_msg):
    """
    當背景任務完成時，把 UI 恢復原狀並顯示結果。
    """
    # 1. 停止並隱藏進度條
    progress_bar.stop()
    progress_bar.pack_forget()

    # 2. 恢復按鈕狀態
    btn_compress.configure(state="normal", text="🔒 執行加密壓縮")
    btn_decompress.configure(state="normal", text="🔓 執行解密還原")

    # 3. 彈出結果對話框
    if success:
        messagebox.showinfo("任務成功", f"執行完畢！\n\n[輸出檔案位置]\n{output_file}\n\n[系統日誌]\n{stdout}")
    else:
        messagebox.showerror("核心引擎錯誤", error_msg)


# ================= 建立主視窗 =================
root = ctk.CTk()
root.title("HuffGuard")
root.geometry("550x380")
root.resizable(False, False)

# 主標題
lbl_title = ctk.CTkLabel(root, text="🛡️ HuffGuard Security Console", font=ctk.CTkFont(size=24, weight="bold"))
lbl_title.pack(pady=(20, 15))

# 建立一個承載表單的卡片式框架
frame_form = ctk.CTkFrame(root, corner_radius=10)
frame_form.pack(pady=10, padx=20, fill="x")

# --- 檔案選擇區 ---
frame_file = ctk.CTkFrame(frame_form, fg_color="transparent")
frame_file.pack(pady=(15, 10), padx=15, fill="x")

lbl_file = ctk.CTkLabel(frame_file, text="目標檔案", font=ctk.CTkFont(size=14, weight="bold"), width=70, anchor="w")
lbl_file.pack(side="left", padx=(0, 10))

entry_file = ctk.CTkEntry(frame_file, placeholder_text="請選擇要處理的檔案...", width=280)
entry_file.pack(side="left", padx=(0, 10))

btn_browse = ctk.CTkButton(frame_file, text="瀏覽檔案", width=80, fg_color="#555555", hover_color="#333333", command=select_file)
btn_browse.pack(side="left")

# --- 密鑰輸入區 ---
frame_key = ctk.CTkFrame(frame_form, fg_color="transparent")
frame_key.pack(pady=(0, 15), padx=15, fill="x")

lbl_key = ctk.CTkLabel(frame_key, text="安全密鑰", font=ctk.CTkFont(size=14, weight="bold"), width=70, anchor="w")
lbl_key.pack(side="left", padx=(0, 10))

entry_key = ctk.CTkEntry(frame_key, placeholder_text="輸入 XOR 混淆密鑰...", width=280, show="*")
entry_key.pack(side="left")

# --- 動態進度條 (預設隱藏) ---
progress_bar = ctk.CTkProgressBar(root, width=400, mode="indeterminate")
# 注意：這裡我們先不 pack 它，等按鈕按下去再 pack 出來

# --- 底部操作按鈕區 ---
frame_action = ctk.CTkFrame(root, fg_color="transparent")
frame_action.pack(pady=(10, 20))

btn_compress = ctk.CTkButton(frame_action, text="🔒 執行加密壓縮", font=ctk.CTkFont(size=15, weight="bold"), 
                             width=160, height=40, fg_color="#2E7D32", hover_color="#1B5E20", 
                             command=lambda: run_engine_async("compress"))
btn_compress.pack(side="left", padx=15)

btn_decompress = ctk.CTkButton(frame_action, text="🔓 執行解密還原", font=ctk.CTkFont(size=15, weight="bold"), 
                               width=160, height=40, fg_color="#1565C0", hover_color="#0D47A1", 
                               command=lambda: run_engine_async("decompress"))
btn_decompress.pack(side="left", padx=15)

# 啟動事件迴圈
root.mainloop()