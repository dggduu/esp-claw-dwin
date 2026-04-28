import re
import struct

with open("GBK.c", "r", encoding="utf-8") as f:
    text = f.read()

# 匹配 const unsigned short uni2oem[] = { ... };
m = re.search(r'uni2oem\[\]\s*=\s*\{([^}]*)\}', text, re.DOTALL)
if not m:
    raise SystemExit("uni2oem not found")
data_str = m.group(1)
nums = [int(x, 16) for x in re.findall(r'0x[0-9A-F]+', data_str)]

# 写入 Little‑Endian unsigned short
with open("uni2oem.bin", "wb") as f:
    for v in nums:
        f.write(struct.pack("<H", v))

print(f"Extracted {len(nums)} entries -> uni2oem.bin")