# C3 六／八页 NVS 容量探针

此独立 ESP-IDF 项目只使用合成数据和仓外 QEMU Flash，分别验证 `base_store@0x138000` 连续六页（`0x6000`）及八页（`0x8000`）。它不接入产品固件，不读取设备备份，不修改产品分区表，也不提供刷板命令。

每次提交包含三份实际格式记录：

- Base v3 最大 **7,618 字节**规范配置，通过正式 `esp_base_remote_config_commit_verified` 提交至 `base_config/committed`，按 revision 执行 CAS 与回读。
- 当前 OTA V2 **186 字节**合成收据形态，使用 `esp_base_ota_receipt.c` 的固定字段布局，在产品键 `base_ota/operation` 上直接调用 NVS API 提交及回读；前四个 SHA-256 字节承载测试 revision 以确保每代变值。此容量测试不调用正式收据解码、OTA 注册策略、下载或签名校验，不声称该记录是完整有效的 OTA 事务。
- 精确锁定的 Container 组件用 `econtainer_slots_initialize` 的正式 ECS2 编码器生成 **288 字节**无包绑定初态；每代只改变 sequence 并重算 CRC，通过真实 IDF provider 向产品键 `base_pkg/slots` 提交，再用 `econtainer_slots_load` 的正式解码器校验。合成分区表提供 `product_pkgs` 几何供 provider 绑定；不读取或执行包。

Container 由本项目的 `main/idf_component.yml` 和 `dependencies.lock` 直接从公开源精确解析，版本与 Base 产品锁一致。不使用邻仓相对路径或复制 Container 源码。六／八页必须用不同的仓外构建目录、`sdkconfig` 和 Flash 文件，避免重用分区表。

## 复现

需要 [sdk-lock.json](../../../sdk-lock.json) 指定的 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，以及支持 `-machine esp32c3` 的 Espressif QEMU。项目配置阶段运行 `tools/check_sdk.py`。以下命令从本目录执行；将两个占位绝对路径替换为本机仓外工作目录及 QEMU 可执行文件：

```bash
probe_work_root=/absolute/path/outside/repo
qemu_bin=/absolute/path/to/qemu-system-riscv32
source "$IDF_PATH/export.sh"
python3 ../../../tools/check_sdk.py --path "$IDF_PATH"

for pages in 6 8; do
    work="$probe_work_root/$pages-pages"
    mkdir -p "$work"
    for stage in 1 2 3; do
        idf.py -C . -B "$work/build" -D "SDKCONFIG=$work/sdkconfig" \
            -D "PROBE_NVS_PAGES=$pages" -D "PROBE_STAGE=$stage" build
        python3 run-qemu.py --build-dir "$work/build" --flash "$work/flash.bin" \
            --stage "$stage" --nvs-pages "$pages" --qemu "$qemu_bin" \
            --timeout-seconds 600
    done
done
```

阶段 1 提交 revision 1–3 并验证旧 revision CAS 冲突；阶段 2 在新进程中先读回 revision 3，再提交 4–100；阶段 3 在另一新进程中读回 revision 100。runner 只替换合成 Flash 的 factory app 区，保留同一大小的 NVS。每步必须有不同配置摘要、两份旁侧 blob 回读及 ECS2 解码成功；退出码 0 才算该阶段成功。

可用固定 SDK 官方 parser 检查最终 NVS 页完整性。以下提取只写入仓外目录，不提交二进制：

```bash
for pages in 6 8; do
    work="$probe_work_root/$pages-pages"
    python3 - "$work/flash.bin" "$work/base_store-final.bin" "$pages" <<'PY'
from pathlib import Path
import sys
flash = Path(sys.argv[1]).read_bytes()
pages = int(sys.argv[3])
Path(sys.argv[2]).write_bytes(flash[0x138000:0x138000 + pages * 4096])
PY
    python3 "$IDF_PATH/components/nvs_flash/nvs_partition_tool/nvs_tool.py" \
        -i -d none "$work/base_store-final.bin"
done
```

新的六／八页结果和输入摘要见[容量记录](../../../docs/operations/c3-eight-page-nvs-capacity.md)。`evidence/` 中已提交的 118 字节 OTA／占位 Container 日志属于 2026-09-26 的历史八页测试，不能作为当前精确记录的证据。完整 QEMU 串口日志、合成 Flash、提取的 NVS 和构建产物均留在仓外。
