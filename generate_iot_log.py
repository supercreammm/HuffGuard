import random
import datetime
import os
import sys

# 強制讓 Python 輸出純文字時不要因為編碼問題崩潰
sys.stdout.reconfigure(encoding='utf-8')

# ================= 設定區 =================
# 你可以隨時修改這裡的數字來產生不同大小的檔案 (例如 1, 5, 20)
TARGET_MB = 1028
filename = f"iot_telemetry_{TARGET_MB}MB.log"
target_bytes = TARGET_MB * 1024 * 1024

print(f">>> Start generating IoT test data ({TARGET_MB}MB)...")

# 模擬三台在不同區域的邊緣感測器
devices = ["ESP32_Zone_A", "STM32_Zone_B", "CortexM0_Zone_C"]
current_time = datetime.datetime(2026, 5, 17, 8, 0, 0)
bytes_written = 0

with open(filename, "w", encoding="utf-8") as f:
    while bytes_written < target_bytes:
        # 1. 模擬真實的數據波動
        dev = random.choice(devices)
        temp = round(random.uniform(22.0, 28.5), 2)  
        hum = round(random.uniform(50.0, 65.0), 1)   
        status = "WARNING" if temp > 27.5 else "OK"  
        
        # 2. 組裝成業界標準的 JSON 格式
        log_entry = f'{{"timestamp": "{current_time.isoformat()}Z", "device_id": "{dev}", "sensor_type": "environment", "metrics": {{"temperature_c": {temp}, "humidity_rh": {hum}}}, "system_status": "{status}"}}\n'
        
        # 3. 寫入檔案並計算精確大小
        f.write(log_entry)
        bytes_written += len(log_entry.encode('utf-8'))
        
        # 每次回傳時間增加 1~3 秒
        current_time += datetime.timedelta(seconds=random.randint(1, 3))

print(f">>> Success! Filename: {filename}")
print(f">>> Actual Size: {os.path.getsize(filename):,} Bytes")