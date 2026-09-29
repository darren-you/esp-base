# C3 六／八／十一页与 ESP32 六页 NVS 容量探针

此独立 ESP-IDF 项目只使用合成数据和仓外 QEMU Flash，分别验证历史 C3 `base_store@0x138000` 连续六／八页、C3 产品数据布局的 `base_store@0x3f5000/0xb000`，以及 ESP32 `base_store@0x3fa000/0x6000`。ESP32 合成表保留三包槽、旧 AT 原字节归档和 FRP scratch 的候选地址，但把真实双 OTA app 换成测试专用 factory app；它不接入产品固件，不读取设备备份，不修改产品分区表，也不提供刷板命令。

每代轮次覆盖四类实际格式记录：

- Base v3 最大 **7,618 字节**规范配置，通过正式 `esp_base_remote_config_commit_verified` 提交至 `base_config/committed`，按 revision 执行 CAS 与回读。
- 当前 OTA V3 **308 字节**合成收据形态，使用 `esp_base_ota_receipt.c` 的固定字段布局，在产品键 `base_ota/operation` 上直接调用 NVS API 提交及回读；前四个 SHA-256 字节承载测试 revision 以确保每代变值。此容量测试不调用正式收据解码、OTA 注册策略、下载或签名校验，不声称该记录是完整有效的 OTA 事务。
- 精确锁定的 Container 组件用 `econtainer_slots_initialize` 的正式 ECS2 编码器生成 **288 字节**无包绑定初态；每代只改变 sequence 并重算 CRC，通过真实 IDF provider 向产品键 `base_pkg/slots` 提交，再用 `econtainer_slots_load` 的正式解码器校验。合成分区表提供 `product_pkgs` 几何供 provider 绑定；不读取或执行包。
- Base 产品操作账本候选 **910 字节**单 blob，按当前八条槽位格式逐代填满最近记录，先写 `PREPARED`，再写 `SUCCEEDED`，每次都经 `base_product/operations` 提交及逐字节读回。这里只验证同一 NVS 分区的容量与页回收；不执行产品写命令、历史幂等裁决或实板磨损验收。

Container 由本项目的 `main/idf_component.yml` 和目标专用 `dependencies.lock`／`dependencies.lock.esp32` 直接从公开源精确解析，版本与 Base 产品锁一致。不使用邻仓相对路径或复制 Container 源码。每个目标与页数必须使用不同的仓外构建目录、`sdkconfig` 和 Flash 文件，避免重用分区表。

## 复现

需要 [sdk-lock.json](../../../sdk-lock.json) 指定的 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`，以及对应目标支持 `-machine esp32c3` 或 `-machine esp32` 的 Espressif QEMU。项目配置阶段运行 `tools/check_sdk.py`。以下 C3 命令从本目录执行；将两个占位绝对路径替换为本机仓外工作目录及 QEMU 可执行文件：

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
            -D "IDF_TARGET=esp32c3" -D "PROBE_NVS_PAGES=$pages" \
            -D "PROBE_STAGE=$stage" build
        python3 run-qemu.py --build-dir "$work/build" --flash "$work/flash.bin" \
            --stage "$stage" --target esp32c3 --nvs-pages "$pages" --qemu "$qemu_bin" \
            --timeout-seconds 600
    done
done
```

C3 正式产品数据布局的 11 页变体使用同一区域的合成 factory app，包槽／scratch／NVS 地址与正式表相同；三个阶段沿同一 4 MiB 合成 Flash 续跑：

```bash
work="$probe_work_root/c3-11-pages"
mkdir -p "$work"
for stage in 1 2 3; do
    idf.py -C . -B "$work/build" -D "SDKCONFIG=$work/sdkconfig" \
        -D "IDF_TARGET=esp32c3" -D "PROBE_NVS_PAGES=11" \
        -D "PROBE_STAGE=$stage" build
    python3 run-qemu.py --build-dir "$work/build" --flash "$work/flash.bin" \
        --stage "$stage" --target esp32c3 --nvs-pages 11 --qemu "$qemu_bin" \
        --timeout-seconds 600
done
```

ESP32 六页使用独立目录和 Xtensa QEMU，不复用 C3 的构建或 Flash：

```bash
work="$probe_work_root/esp32-6-pages"
qemu_xtensa=/absolute/path/to/qemu-system-xtensa
mkdir -p "$work"
for stage in 1 2 3; do
    idf.py -C . -B "$work/build" -D "SDKCONFIG=$work/sdkconfig" \
        -D "IDF_TARGET=esp32" -D "PROBE_NVS_PAGES=6" -D "PROBE_STAGE=$stage" build
    python3 run-qemu.py --build-dir "$work/build" --flash "$work/flash.bin" \
        --stage "$stage" --target esp32 --nvs-pages 6 --qemu "$qemu_xtensa" \
        --timeout-seconds 600
done
```

阶段 1 提交 revision 1–3 并验证旧 revision CAS 冲突；阶段 2 在新进程中先读回 revision 3，再提交 4–100；阶段 3 在另一新进程中读回 revision 100。runner 只替换合成 Flash 的 factory app 区，保留同一大小的 NVS。每步必须有不同配置摘要、OTA 与 ECS2 回读、产品账本两次提交读回及 ECS2 解码成功；退出码 0 才算该阶段成功。当前 V3 长度已在 C3 十一页布局复测。新增账本后的 C3 六／八页和 ESP32 六页完整旧结果使用 V2 长度，输入摘要及边界见下方两份容量记录；旧三记录测试仍只作为历史事实。

可用固定 SDK 官方 parser 检查最终 NVS 页完整性。以下 C3 提取只写入仓外目录，不提交二进制；ESP32 则从自己的 Flash 精确提取 `0x3fa000:0x400000`：

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
python3 - "$probe_work_root/esp32-6-pages/flash.bin" \
    "$probe_work_root/esp32-6-pages/base_store-final.bin" <<'PY'
from pathlib import Path
import sys
flash = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_bytes(flash[0x3fa000:0x400000])
PY
python3 "$IDF_PATH/components/nvs_flash/nvs_partition_tool/nvs_tool.py" \
    -i -d none "$probe_work_root/esp32-6-pages/base_store-final.bin"
```

11 页新布局结果见[C3 十一页容量记录](../../../docs/operations/c3_eleven_page_nvs_capacity.md)。历史六／八页结果和输入摘要见[C3 容量记录](../../../docs/operations/c3-eight-page-nvs-capacity.md)，ESP32 结果见[ESP32 容量记录](../../../docs/operations/esp32-six-page-nvs-capacity.md)。`evidence/` 中已提交的 118 字节 OTA／占位 Container 日志属于 2026-09-26 的历史八页测试，不能作为当前精确记录的证据。完整 QEMU 串口日志、合成 Flash、提取的 NVS 和构建产物均留在仓外。
