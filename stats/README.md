# P4 RC 轉換器量測

在一台 Linux 上，用 scapy 模擬兩個 RDMA 節點（與 [scapy 測試](../README.md#scapy)相同的 veth 拓撲），量測 P4 交換器加上 ICRC controller 這套架構的正確性、延遲、吞吐量、遺失與擴展性，並輸出圖表與報告。

```
bench.py      ── node-a  ═ sw-p1  ── port 1   ┐
responder.py  ── node-b  ═ sw-p2  ── port 2   ├ simple_switch (build/main.json)
controller    ── ctl-cpu ═ sw-cpu ── port 510 ┘
```

## 比較的三條路徑

| 名稱 | 規則 | 封包路徑 | 用途 |
| --- | --- | --- | --- |
| `direct` | — | node-a ═ node-b 直接以 veth 相連，不經交換器 | 基線；也是量測工具本身的上限 |
| `fast` | `gen_rules.py passthrough` | BMv2 一次處理後直接轉出 | 兩端直接以對方真實身分定址，交換器只改 MAC、TTL、UDP checksum，ICRC 不變，不經 controller |
| `ctl` | `gen_rules.py translate` | BMv2 → CPU 埠 → controller 重算 ICRC → BMv2 → 轉出 | 現行設計（與 `controller/connections.example.txt` 相同） |

兩兩相減就能拆出成本：`fast − direct` 是 BMv2 管線，`ctl − fast` 是 controller 繞行，包含 packet_out 回到 BMv2 後的第二次處理。

## 測試計畫

| 編號 | 測試 | 方法 | 指標 | 圖表 |
| --- | --- | --- | --- | --- |
| T0 | 環境 | 記錄 CPU、kernel、BMv2／p4c／scapy 版本、git commit | — | `env.json` |
| T1 | 功能正確性 | `scapy/client.py` 的 22 項檢查，跑 translate、PSN 回繞、passthrough 三種規則，各重複 3 次 | 每項 PASS／FAIL／SKIP | 檢查項目 × 執行次數的熱圖 |
| T2 | 延遲基線 | 一次只送一個訊息的 echo（Send → ACK + echo Send → ACK），三條路徑，64 B，每點 2000 個訊息 × 3 次 | 單向延遲、Send→ACK RTT、echo RTT 的 p50／p90／p99／p99.9 | CDF、箱型圖 |
| T3 | 延遲對訊息大小 | 同 T2，payload 1、16、64、128、256 B | 各大小的 p50、p99 | 折線圖 |
| T4 | 延遲拆解 | 在 sw-p1、sw-p2、sw-cpu 三個介面兩個方向各跑一個 tcpdump（奈秒時間戳），閒置與 2000/s 負載各一次 | BMv2 第一次處理、controller 往返、BMv2 第二次處理，按封包種類分 | 堆疊長條圖、controller 服務時間 CDF |
| T5 | 吞吐量與飽和點 | 開環定速送 Send（不等 ACK），250～50000/s，每點 5 秒；responder 每個 Send 回一個 ACK | 實際送出率、送達率、遺失率、負載下的單向延遲 | 送達對送出（含 y=x）、遺失率、延遲對負載 |
| T5b | ACK 合併 | 2000/s 下 responder 每 8 個 Send 才回一個 ACK | 送達率、ACK 遺失、controller CPU | 報告表格 |
| T6 | 亂序與遺失位置 | 取自 T5：responder 追蹤每條連線的 PSN；比對 bench 送出數、veth 計數、P4 `port_rx`、responder 收到數 | 序號缺口（真實 RC 接收端會 NAK 的次數）、亂序、重複；各段剩餘比例 | 缺口／亂序對負載、遺失漏斗 |
| T7 | 連線數擴展性 | 1、16、64、256、1024 條連線（每條 4 筆規則），另以 1025 條確認表滿行為；固定 1000/s 平均分配到各連線 | 規則安裝時間與速率、表項數、TABLE_FULL 數、延遲、遺失 | 安裝時間、延遲、遺失對連線數 |
| T8 | 交換器計數器 | 每次量測前清零、量測後讀取 P4 counter | 各丟棄原因、送往 controller 數、直通數、各埠收包數 | T1 期間的計數器分組長條圖 |
| T9 | 資源使用 | T5 期間每 0.25 秒取樣 `/proc/<pid>/stat`（含子行程） | simple_switch、controller、responder、bench 的 CPU% | CPU% 對負載 |

T6、T8、T9 不另外送流量，而是分析 T1、T5 收集到的資料。

### 量測方式

- **時間戳**：送出端在 `send()` 前記錄 `time.time_ns()`，接收端用 `SO_TIMESTAMPNS` 取得 kernel 收包時間，兩者都是 CLOCK_REALTIME，同一台機器上可以直接相減。RTT 會包含 responder 從 select 喚醒的時間（Python，數十 µs），因此路徑比較以單向延遲為主。
- **組包**：封包模板由 scapy（`scapy/roce.py`）建一次，之後每個封包只改 PSN／AETH 並重算 ICRC。若每個封包都用 scapy 解析，量到的會是 Python 的瓶頸，而不是交換器。
- **responder 不 NAK**：負載產生器不重送，所以 responder 只記錄真實 RC 接收端在哪裡會 NAK。功能上的 NAK 與重送行為由 T1 檢查。
- **P4 counter**：`p4/ingress.p4` 新增兩個只用於量測的 counter，不影響轉送：

  | counter | 索引 |
  | --- | --- |
  | `MyIngress.stats` | 0 malformed、1 unsupported、2 bad ACK、3 bad packet_out、4 packet_out 轉出、5 ingress 丟棄（CPU 埠、長度、TTL）、6 查表未命中、7 出口不合法、8 送往 controller、9 直通 |
  | `MyIngress.port_rx` | 依入口埠計數（1、2、510） |

  手動讀取：`echo "counter_read MyIngress.stats 8" | simple_switch_CLI --thrift-port 9090`

## 執行

需要 root、`simple_switch`、`simple_switch_CLI`、`p4c-bm2-ss`、`tcpdump`，以及 Python 3.8+ 與 scapy（`sudo apt install python3-scapy`）。從專案根目錄執行：

```sh
sudo python3 stats/suite.py              # 全部，約 15 分鐘
sudo python3 stats/suite.py --quick      # 較少的點與較短的時間，約 4 分鐘
sudo python3 stats/suite.py --tests T1,T2 --paths fast,ctl
```

`suite.py` 會自行編譯 P4、建立與刪除 veth、啟動與停止 simple_switch、controller 和 responder；開始前請先停掉手動啟動的交換器（9090 埠必須空著）。其他參數見 `--help`，例如 `--rates 500,1000,2000`、`--duration 10`、`--scale 1,256,1024`。

### 繪圖

`plot.py` 需要 matplotlib。測試機上沒有的話，`suite.py` 只會產生 `summary/*.csv`；把結果目錄複製到有 matplotlib 的機器上再畫：

```sh
sudo apt install python3-matplotlib                # 或在測試機以外的機器上：
scp -r p4-switch:Open-Hackathon/result/stats/<run> result/stats/
python3 stats/analyze.py result/stats/<run>        # 已經彙整過可略過
python3 stats/plot.py result/stats/<run>
```

`analyze.py` 只用標準函式庫，在任何機器上都能重新彙整。

### 單獨使用工具

```sh
python3 stats/gen_rules.py translate -n 16 -o /tmp/rules.txt   # 16 條連線的規則
sudo python3 stats/responder.py --rules /tmp/rules.txt          # node-b
sudo python3 stats/bench.py latency --rules /tmp/rules.txt --out /tmp/lat --size 64
sudo python3 stats/bench.py throughput --rules /tmp/rules.txt --out /tmp/tp --rate 2000
```

規則檔中每條連線佔連續四行（client Send、server ACK、server Send、client ACK）。`gen_rules.py translate` 只產生一條連線時，輸出與 `controller/connections.example.txt` 完全相同。

## 輸出

```
result/stats/<YYYYmmdd-HHMMSS>/
  env.json                 T0
  correctness.csv, t1/     T1：每次 client.py 的輸出與計數器
  latency/                 T2/T3：<path>-s<size>-r<rep>.{json,client.csv,server.csv,side.json}
  stages/                  T4：pcap 與對應的量測
  throughput/              T5：另有 .acks.csv（ACK 收到時間）與 .cpu.csv
  scale/                   T7：install.json 與各連線數的量測
  rules/                   用到的規則檔
  logs/                    simple_switch、controller、responder、server 的輸出
  summary/                 analyze.py：latency、throughput、stages、scale、t1_counters 的 CSV
  figures/, report.md      plot.py
```

`*.side.json` 是每次量測前後的介面計數（`/sys/class/net/*/statistics`）與 P4 counter。

## 限制

- BMv2 是軟體交換器，絕對數值不代表 Tofino 或 DPU；應比較路徑之間的差距、趨勢與比例。所有行程都在同一台機器上，彼此會搶 CPU。
- 負載產生器是 Python，單核心上限約數萬 pps；`direct` 路徑的結果就是這個上限。
- `run_controller.py` 以原樣執行 `controller/controller.py`。`main` 上的 controller 無條件 `import rc_state`，但 `rc_state.py` 只在 rc-state 分支，所以缺少該檔時以空模組代替（不使用 `--rc-config` 時不會用到它）。
