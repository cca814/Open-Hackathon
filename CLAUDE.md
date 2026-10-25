# CLAUDE.md — DPU baseline 測試

這個 worktree 的任務:在真實的 BlueField-3 上跑 **baseline**(DPU 當 RDMA relay,見 [rdma/RELAY.md](rdma/RELAY.md))並記錄數據。
標「未確認」的事實不要當成已知;跑過、看過輸出的才算確認。

## 結構圖

- [docs/environment.png](docs/environment.png):實體環境。兩台工作站 NVAITC-WS#1 / WS#2,各一張 BlueField-3,兩個 100Gb port 以 active fiber 一對一直連;10Gb ETH 走 L3 switch 只供管理 / VPN,**不是資料面**。
- [docs/baseline-architecture.png](docs/baseline-architecture.png):baseline 的資料路徑(原始設計)。

```
WS#1 host                 WS#1 BF3        WS#2 BF3        WS#2 DPU ARM
source ──────────────────▶ Port 1 ═══════▶ Port 3 ──────▶ relay: QP_in ─ buffer pool
sink A/B/C (QP1..3) ◀──── Port 2 ◀═══════ Port 4 ◀────── relay: QP_out[0..2]
 (port 10011/12/13, 同一個 GID)
```

- ① source 從 Port 1 以 RoCEv2 SEND 送到 relay 的 `QP_in`。ACK 由 relay 這側的 NIC 回,不是 sink 回。
- ② relay 把每則訊息收進同一個 buffer pool 的 slot,對 3 個 target 各 `post_send` 一次(同一個 slot,不 memcpy),3 個 send 都完成才把 slot 還給 `QP_in`。
- ③ 3 份 SEND 從 Port 4 出去,到 Port 2 上的 3 個 sink(各一個 QP、一個 process,各自的 TCP 控制埠)。每個 sink 的 NIC 各回一個 ACK。
- 控制面(只在 setup):TCP 交換 QPN + GID,走同樣的鏈路。資料面只有 RoCEv2 SEND 和 ACK。
- 圖上 Port 1/2 = WS#1 的 IB0/IB1,Port 3/4 = WS#2 的 IB0/IB1。接線:`p0`↔`p0`(Port 1↔3)與 `p1`↔`p1`(Port 2↔4)已用 ping、RoCE 流量和 OVS FDB 確認;圖上 IB0/IB1 的編號仍是推得。鏈路必須是 Ethernet 模式(RoCEv2)。

### 目前實際配置:source 與 sinks 在 DPU1 的 ARM 上

原設計把 source / sinks 放在 WS#1 的 **host**,但 host 上 `a2-07` 不在 sudoers,設不了 IP。所以改成 source 和 3 個 sink 都跑在 DPU1 的 ARM 上,relay 在 DPU2 的 ARM 上。**這測的是「ARM 對 ARM」**,source / sink 的 CPU 和 relay 一樣弱,數字不能代表 host 端對 DPU relay 的情況。若之後 host1 能 sudo,可以改回原設計,DPU2 這側不用重做。

| 行程 | 機器 | ibdev / SF | IP | GID index |
|---|---|---|---|---|
| source | DPU1 | `mlx5_2` / `enp3s0f0s0` | 10.0.1.1 | 1 |
| sink A(10011) | DPU1 | `mlx5_3` / `enp3s0f1s0` | 10.0.2.1 | 1 |
| sink B(10012) | DPU1 | `mlx5_4` / `enp3s0f1s1`(額外建的 SF) | 10.0.2.3 | 1 |
| sink C(10013) | DPU1 | `mlx5_5` / `enp3s0f1s2`(額外建的 SF) | 10.0.2.4 | 1 |
| relay | DPU2 | 進 `mlx5_2` / `enp3s0f0s0`,出 `mlx5_3` / `enp3s0f1s0` | 10.0.1.2、10.0.2.2 | 1、1 |

## 機器與存取

| 角色 | ssh 別名 | 位址 | 備註 |
|---|---|---|---|
| WS#1 host | `DPU-host1` | 192.168.3.7 | BF3 的 host 側:`mlx5_0`=`enp116s0f0np0`、`mlx5_1`=`enp116s0f1np1` |
| WS#2 host | `DPU-host2` | 192.168.3.8 | BF3 的 host 側:`mlx5_0`=`enp195s0f0np0`、`mlx5_1`=`enp195s0f1np1` |
| WS#1 DPU ARM | `DPU1` | 192.168.100.2,ProxyJump `DPU-host1` | aarch64,16 core,**source 與 3 個 sink 跑這裡** |
| WS#2 DPU ARM | `DPU2` | 192.168.100.2,ProxyJump `DPU-host2` | **relay 跑這裡** |

- 使用者都是 `a2-07`。四台都有 gcc / make,`/dev/infiniband/uverbs*` 是 0666,跑 verbs 程式不需要 root。
- **sudo**:DPU1 / DPU2 免密碼。兩台 host 上 `a2-07` **不在 sudoers**(sudo 群組只有其他管理員帳號),要請 host 管理員處理。
- `DPU1` 與 `DPU2` 的 HostName 都是 192.168.100.2,所以 `~/.ssh/config` 的兩個 Host 區塊各有 `HostKeyAlias DPU1` / `HostKeyAlias DPU2`,直接 `ssh DPU1` / `ssh DPU2` 即可。不要用 `ssh-keygen -R` 或 `StrictHostKeyChecking=no` 硬過。兩台 DPU 的 `hostname` 都是 `localhost.localdomain`,要分辨是哪一台,看 `p0` 的 MAC(DPU1 `9c:63:c0:50:96:64`、DPU2 `9c:63:c0:50:85:c4`)。
- 程式碼放在四台機器的 `~/baseline/rdma/`(從本 repo 的 `rdma/` 以 tar 傳過去,已編譯)。改程式後要重新傳、重新 `make relay sink source`。
- 從腳本迴圈裡呼叫 `ssh` 要加 `< /dev/null`,否則 `ssh` 會吃掉迴圈的標準輸入。`pkill` 一次只能給一個 `-x` 名稱;不要用 `pkill -f`(它會殺到自己的 ssh 指令)。

## 網路設定:`rdma/dpu-net.sh`

[rdma/dpu-net.sh](rdma/dpu-net.sh) 在各 DPU 上跑,可重複執行。IP 和額外的 SF 重開機會消失,**重開機後各跑一次 `up`**;OVS 的 port 存在 OVS 資料庫,會保留。

```sh
~/baseline/rdma/dpu-net.sh dpu1 up      # DPU1:建 SF 1、2,設 IP,設 ARP 參數
~/baseline/rdma/dpu-net.sh dpu2 up      # DPU2:p0/p1 加進 OVS,IP 設在 SF 上
~/baseline/rdma/dpu-net.sh dpu1 status  # 看 IP、OVS port、IPv4 的 RoCE v2 GID
~/baseline/rdma/dpu-net.sh dpu1 down    # 還原(刪額外的 SF、拿掉 IP 與 ARP 參數)
```

子網:`10.0.1.0/24` = Port 1↔3(`p0`),`10.0.2.0/24` = Port 2↔4(`p1`)。DPU1 的 `ovsbr1`/`ovsbr2` 原本就把 SF 的 representor 和 `p0`/`p1` 橋在一起;DPU2 的原本沒有 `p0`/`p1`,`up` 會加進去。

### 實測查到、會踩到的坑(都是這台機器上跑出來的)

- **`p0`/`p1` 不能直接跑 RoCE**:`mlx5_0`/`mlx5_1` 的 port 83(`p0`/`p1`)在 sysfs 的 `gids/` 是空的(0 個項目),SF 則有 255 格。沒有 GID 就組不出位址,所以 relay 和其他程式都要用 **SF**(`mlx5_2`/`mlx5_3`)。為什麼上行口沒有 GID 表,未查到文件,這只是觀察。
- **每個 SF 同一時間只能有一個 RDMA 行程**:同一個 SF 上只要有一個行程建了 RC QP,其他行程在這個 SF 上 `ibv_create_cq` 都會 `EINVAL`(`ibv_rc_pingpong` 也一樣)。同一個行程在一個 SF 上建多個 QP、CQ 都正常;別的 SF 和 PF 不受影響。原因未查明。所以 DPU1 的 source 加 3 個 sink 共 4 個行程要 4 個 SF,DPU2 的 relay 一個行程用 2 個 SF。
- **同一子網多個 IP 會有 ARP 問題**:DPU1 的 `10.0.2.1`、`.3`、`.4` 在不同 SF 上,預設第一個 SF 會替全部 IP 回 ARP,DPU2 把三個 target 都解析成 `.1` 的 MAC,RoCE 全送到 sink A 的 SF(ping 仍通,因為 ICMP 由整台機器回應)。`dpu-net.sh up` 會在 DPU1 的 SF 設 `arp_ignore=1`、`arp_announce=2` 並清 DPU2 的鄰居表。判斷方式:`ip neigh show` 看三個 IP 的 MAC 是否各異。
- **新建的 SF 第一次傳輸很慢**:前幾次迭代含 OVS 建流程的時間(5 次 pingpong 約 200 µs/次),跑幾千次後和舊 SF 一樣約 4 µs/次,流程已 offload。量測前先 warm-up。
- **`mlnx-sf --action delete` 要完整 index**:`--sfindex pci/0000:03:00.1/294945`,不是 `--sfnum`。
- 新建 SF 的 representor 預設 MTU 1500(舊的是 9216),路徑 MTU 1024 下不影響。

### DPU 上的 ibdev 與介面(兩台 DPU 相同)

- `mlx5_0`/`mlx5_1` 是兩個 PF(PCI `03:00.0`/`03:00.1`),管理 eswitch(`switchdev` 模式);`p0`/`p1` 是它們的實體 uplink(port 83),`pf0hpf`/`pf1hpf` 是 host PF 的 representor(port 1)。
- `en3f0pf0sf0` 等是 SF 的 **representor**,在 `ovsbr1`/`ovsbr2`,用來把 SF 接進 eswitch,不是給應用程式用的 port(在 `mlx5_0` 裡是 port 18)。
- `enp3s0f0s0`(`mlx5_2`)、`enp3s0f1s0`(`mlx5_3`)是 SF 自己的 netdev,應用程式綁它們;封包經 representor 進 eswitch,再由 OVS 轉到 `p0`/`p1`。額外的 SF 用 `mlnx-sf --action create --device 0000:03:00.1 --sfnum N` 建(PF1,`sfnum 0` 是原有的)。

## 怎麼跑

編譯與參數見 [rdma/RELAY.md](rdma/RELAY.md)。順序:先 3 個 sink,再 relay,最後 source;每個程式在 TCP 交換處等下一個。目前配置的指令:

```sh
# DPU1:3 個 sink,各用一個 SF
./sink --device mlx5_3 --gid-index 1 --port 10011 --count N --msg-size M --pool 64 --verify
./sink --device mlx5_4 --gid-index 1 --port 10012 --count N --msg-size M --pool 64 --verify
./sink --device mlx5_5 --gid-index 1 --port 10013 --count N --msg-size M --pool 64 --verify
# DPU2:relay
./relay --in-device mlx5_2 --in-gid 1 --out-device mlx5_3 --out-gid 1 --msg-size M --pool 64 \
        --target 10.0.2.1:10011 --target 10.0.2.3:10012 --target 10.0.2.4:10013 --count N --stats
# DPU1:source
./source --device mlx5_2 --gid-index 1 --server-ip 10.0.1.2 --count N --size S --window 16
```

- `--msg-size` 要 ≥ `source --size`(量測時用 `max(S + 64, 4096)`)。
- 成功判準:每個 sink 都是 `0 corrupted, 0 out of order`,relay 印 `Forwarded N messages ... to 3 target(s)`。
- source 的 completion 只代表 relay 的 NIC 收到,不代表 sink 收到;吞吐與完成時間要以 sink 和 relay 的輸出為準。
- **不要用預設的 `--pool 16 --window 16`**:兩者一樣大時,1KB 只有 0.09 Gbps(約 1.1 萬則/秒);`--pool 64`(window 16)是 1.93 Gbps,差約 20 倍。

### 掃描:`rdma/bench.sh`

[rdma/bench.sh](rdma/bench.sh) 從能 `ssh DPU1` / `ssh DPU2` 的機器執行,照上面的配置起 sink、relay、source,每次跑印一行(source / relay / 各 sink 的 Gbps、訊息率、verify 結果)。兩台 DPU 的 `~/baseline/rdma` 要已編譯,且 `dpu-net.sh` 已 `up`。

```sh
rdma/bench.sh                          # 預設:64B 到 64KB 六種大小,各 3 次,pool 64、window 16
rdma/bench.sh -r 1 1024:20000          # SIZE[:COUNT],跑 1 次 20000 則 1KB
rdma/bench.sh --no-verify 16384:100000 # sink 不加 --verify
rdma/bench.sh -d baseline_fast 1024:300000   # 用 DPU 上另一份程式副本(家目錄下的資料夾名)
```

開頭會先跑一次 warm-up(讓 OVS 把流程 offload),結束時不論成敗都會收掉 sink / relay / source。

## 量測結果(2026-10-21)

配置如上(source 與 sinks 在 DPU1 ARM,relay 在 DPU2 ARM,單向兩段 100G 鏈路),`--pool 64 --window 16`,每個 sink 都 `0 corrupted, 0 out of order`。「in」是 relay 收到的速率,「out」是 relay 對 3 個 target 送出的總和(= 3 × in)。

**原始程式、含 `--verify`**(每種大小 3 次的平均,各次差距在 ±5% 內):

| 訊息大小 | 訊息率 (百萬則/秒) | relay in (Gbps) | relay out (Gbps) |
|---|---|---|---|
| 64 B | 0.043 | 0.02 | 0.07 |
| 256 B | 0.049 | 0.10 | 0.30 |
| 1 KB | 0.241 | 1.98 | 5.9 |
| 4 KB | 0.078 | 2.5 | 7.6 |
| 16 KB | 0.053 | 6.9 | 20.7 |
| 64 KB | 0.018 | 9.4 | 28.2 |

這組數字**不是 relay 的上限**,主要被測試程式本身和 RNR 卡住:

- `--verify`(sink 逐位元組檢查)與 `payload_fill`(source 每則逐位元組填資料)都是 ARM 上的純量迴圈,大訊息時成為瓶頸。拿掉 `--verify`:1KB 變 9.5 Gbps(1.16 百萬則/秒)、16KB / 64KB 變 14.4 Gbps。
- RNR:source 的 completion 來自 relay 網卡的 ACK,和 relay 有沒有轉完無關,所以 source 會一路送到把 relay 的 pool 吃光,之後靠 RNR 限速。`qp.c` 的 `min_rnr_timer = 12`(IB 規格對照表約 0.64 ms),每次 relay 的 `QP_in` 沒有可用接收緩衝(`out_of_buffer` 計數器增加,64B 跑 10 萬則有 1106 次)source 就等一次。只把它改成 1,訊息率:64B 0.043→0.145、1KB 0.24→0.51、16KB 0.052→0.073 百萬則/秒。

**盡量排除測試程式與 RNR 計時器的影響**(`payload_fill` 只填 8 byte 序號、不加 `--verify`、`min_rnr_timer = 1`;每種大小 2 次,括號為兩次):

| 訊息大小 | 訊息率 (百萬則/秒) | relay in (Gbps) | relay out (Gbps) |
|---|---|---|---|
| 64 B | 0.145 (0.145, 0.143) | 0.07 | 0.22 |
| 256 B | 0.154 (0.160, 0.148) | 0.3 | 0.9–1.0 |
| 1 KB | 0.145 (0.151, 0.139) | 1.1–1.2 | 3.4–3.7 |
| 4 KB | 0.412 (0.407, 0.417) | 13.4–13.7 | 40–41 |
| 16 KB | 0.232 (0.231, 0.232) | 30.3–30.4 | 91.0–91.2 |
| 64 KB | 0.059 (0.059, 0.059) | 30.8–30.9 | 92.4–92.6 |

- **≥ 16 KB 時 relay 把出向埠打滿**:out 約 91–92.5 Gbps,對照 `ib_send_bw`(SF 之間單一 QP、64KB)約 92.4 Gbps。此時 relay 的上限是「出向 100G 埠 ÷ 3 個 target」,in 約 30 Gbps。
- **小訊息(64B–1KB)只有約 0.14–0.16 百萬則/秒,且不隨大小變化;4KB 反而是 0.41**。這個不單調的行為**原因未查明**:有 RNR 計數器在動,但沒有找到完整解釋。不要把小訊息的數字當成 relay 的處理能力。
- 沒有調 `--pool` / `--window` 之外的參數;64KB 時把 pool / window 加到 256 / 128 頻寬不變(約 9.5 Gbps,原始程式含 verify)。
- relay 的 CPU 使用率**無資訊價值**:它 busy-poll,每次都是一個核心 100%。沒有量到 ARM 上實際的處理負載。
- 這些副本只存在 DPU 上(不在 repo):`~/baseline_rnr`(`min_rnr_timer = 1`)、`~/baseline_nofill`(`payload_fill` 只填表頭)、`~/baseline_fast`(兩者都改),另有排查用的 `~/baseline/{cqtest,cqcount,hold,manyqp}`。可刪。要重現「排除測試程式影響」那一組:`rdma/bench.sh -d baseline_fast --no-verify`。
- 每組只跑 1–7 秒,每個大小 2–3 次;沒做長時間穩定度和丟包 / 重傳的量測。

## 工作原則

- 影響共用環境的動作(改 IP、MTU、OVS、建 / 刪 SF、link type、重開機、刷 firmware)先跟使用者說明做什麼、再動手;做完要能還原,並記下改了什麼。
- 不要編造硬體行為或數字。量測結果(訊息大小、Gbps、訊息率、掉包 / 重傳)要貼出實際輸出;baseline 的瓶頸(每則訊息都經 DPU ARM,一進三出)就靠這些數字說明。測試程式本身(`--verify`、`payload_fill`、RNR 計時器)會影響數字,比較不同做法時要用同一組設定。
- 跑完把背景程式(sink / relay / source)收掉。
- 風格:沿用既有檔案的註解密度與命名;操作步驟變動時一併更新 README / RELAY.md。
