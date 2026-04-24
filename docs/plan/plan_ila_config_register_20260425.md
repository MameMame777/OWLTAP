# Plan: ILA CONFIG レジスタによるパラメータ動的共有

作成日: 2026-04-25

## 目的

現状、`DATA_W` / `DEPTH` / `ADDR_W` は HDL と C++ 両方でハードコード
(`IlaDriver::kDataWidth=32`, `kDepth=1024` 等) されている。HDL 側でパラメータ
変更しても GUI/Driver は追従できず、別ビットストリームごとにホスト側を
再ビルドする必要がある。

新 IR `CONFIG` (5'h02, 32bit 読み取り専用) を追加し、HDL パラメータを
ビットパック値としてホストに公開する。`IlaDriver::probe()` により JTAG 経由で
この値を取得し、runtime に `depth()` / `dataWidth()` を返す。

## CONFIG レジスタ ビット割り付け (32 bit)

| ビット | フィールド | 格納値 |
|---|---|---|
| [31:24] | VERSION   | `8'h01` (IP バージョン) |
| [23:20] | NUM_CH    | チャネル数 (現状 1 固定) |
| [19:16] | RESERVED  | `4'h0` |
| [15:10] | DATA_W_M1 | `DATA_W - 1` (6bit, 実値 1〜64) |
| [ 9: 8] | RESERVED  | `2'h0` |
| [ 7: 0] | ADDR_W    | `ADDR_W` (8bit, `DEPTH = 1<<ADDR_W`) |

デフォルト構成 (DATA_W=32, ADDR_W=10, NUM_CH=1) → `CONFIG_VAL = 0x0110_7C0A`

> 計算: `{8'h01, 4'h1, 4'h0, 6'd31, 2'h0, 8'd10}`
> = 0x0100_0000 | 0x0010_0000 | 0x0000_7C00 | 0x0000_000A
> = 0x0110_7C0A

縮小構成 (DATA_W=16, ADDR_W=8) → `0x0110_3C08`

## IR 追加

`5'h02 = IR_CONFIG`, DR=32 bit R。未使用だった値を割り当てるため既存 IR は無変更。
BSCANE2 は既存 37bit フレームをそのまま流用 (opcode[4:0]=0x02)。

## フェーズ

### Phase 1: 仕様確定 (本ファイル)
- 本 `docs/plan/plan_ila_config_register_20260425.md` をコミット
- `OwlTAP_ILA_仕様書.md` は Phase 7 で実装値確定後に更新

### Phase 2: HDL 実装
- `hdl/ila/rtl/ila_tap.sv`
  - `localparam IR_CONFIG = 5'h02` を IR_IDCODE の隣に追加
  - `localparam logic [31:0] CONFIG_VAL = {8'h01, 4'(NUM_CH), 4'h0, 6'(DATA_W-1), 2'h0, 8'(ADDR_W)};` を module 内に宣言
  - `parameter int NUM_CH = 1` を parameter リストに追加
  - `idcode_shift` に倣い `config_shift` 32bit レジスタ追加
  - CAPTURE_DR: `IR_CONFIG: config_shift <= CONFIG_VAL;`
  - SHIFT_DR: `IR_CONFIG: config_shift <= {tdi, config_shift[31:1]};`
  - TDO mux: `IR_CONFIG: tdo_d = config_shift[0];`
- `hdl/ila/rtl/ila_top.sv`
  - `parameter int NUM_CH = 1` を追加、`ila_tap` インスタンスに伝搬
- `hdl/ila/rtl/ila_bscane2_top.sv`
  - `parameter int NUM_CH = 1` を追加
  - `CONFIG_VAL` ローカル定義 (同じビット式)
  - CAPTURE-DR mux: `5'h02: capture_data = CONFIG_VAL;`
  - UPDATE-DR: 書き込み不可なので default 節のまま (書き込み無視)
- 既存 IR 動作無変更 → **完全後方互換**

### Phase 3: HDL シミュレーション
- `hdl/ila/sim/tb/ila_test_pkg.sv`
  - `localparam IR_CONFIG = 5'h02` 追加
  - `ila_base_seq::ila_read_config(output bit[31:0])` タスク追加
  - 新テスト `ila_config_read_test` 追加: デフォルト期待値 `0x0117_7C0A`
- `hdl/ila/sim/tb/ila_bscane2_test_pkg.sv`
  - 同等の CONFIG 読み出しテスト (2 スキャン方式) 追加
- `hdl/ila/sim/regression_tests.json` にテスト追加

### Phase 4: ソフトウェア実装

#### 4.1 `src/ila/ila_driver.h` / `ila_driver.cpp`
- 新 IR 定数: `kIrConfig = 0x02`
- 新 struct:
  ```cpp
  struct IlaCaps {
      uint8_t  version  = 0;
      uint8_t  num_ch   = 1;
      uint8_t  data_w   = 32;
      uint8_t  addr_w   = 10;
      uint32_t depth    = 1024;
      uint32_t raw      = 0;
      bool     probed   = false;
  };
  ```
- 新関数:
  - `bool probe(IlaCaps& out);` — IR_CONFIG 読み出し、内部 caps_ 更新
  - `const IlaCaps& caps() const { return caps_; }`
  - `int dataWidth() const { return caps_.data_w; }`
  - `int addrWidth() const { return caps_.addr_w; }`
  - `int depth() const { return static_cast<int>(caps_.depth); }`
- 内部置換:
  - `configureTrigger`, `setReadAddr`: `kDepth`→`depth()`, `kAddrWidth`→`addrWidth()`
  - `readSamples`: `kDataWidth`→`dataWidth()`, `kDepth`→`depth()`
- `static constexpr kDepth / kDataWidth / kAddrWidth` は**後方互換のため残す**
  (MockIlaTap, 既存 GUI 初期値でも参照されている)

#### 4.2 `src/gui/ila_panel.h` / `ila_panel.cpp`
- `pre_samples_` の初期値は (固定値のまま `kDepth/4`)
- `setChain` / `setBscaneChain` の末尾で `driver_->probe(caps)` を呼ぶ
  - probe 成功時: `pre_samples_ = min(pre_samples_, depth/4)` に再設定
- `setReadAddr(IlaDriver::kDepth - 1)` → `driver_->depth() - 1` (2 箇所)
- `SliderInt` の上限を `driver_->depth() - 1` に変更 (driver 接続時のみ)
- `wave_x_[i] = i * 8.0` は不変
- `samples_` は `uint32_t` 維持 (DATA_W≤32 前提)
- 表示: caps の情報をパネル上部に `CAPS: DW=32 Depth=1024` のように表示

#### 4.3 バックエンド
- `BscaneIlaTapBackend` / `ChainIlaTapBackend`: 変更不要

### Phase 5: ソフトウェア単体テスト
- `test/support/mock_ila_tap.h`
  - IR_CONFIG opcode に対する CAPTURE_DR 応答を追加 (`config_val_` メンバ)
  - セッター `setConfigValue(uint32_t)` 追加 (デフォルトは default 構成の期待値)
- `test/ila_driver_test.cpp`
  - `ProbeReadsConfigRegister`
  - `DepthReflectsProbedAddrW`
  - `ConfigureTriggerUsesRuntimeDepth`
  - `DefaultCapsWhenProbeNotCalled`

### Phase 6: 実機確認 (Zybo Z7020)
1. 現行ビットストリームで回帰 (動作不変確認)
2. OwlTAP の IlaPanel に CAPS 表示が出ること確認
3. 将来拡張: `DATA_W=16, ADDR_W=8` 構成で再ビルド → GUI の pre_samples スライダが 0〜255 になること、256 サンプル読み出せることを確認

### Phase 7: ドキュメント最終化
- `OwlTAP_ILA_仕様書.md`:
  - §4 IR 表に `5'h02 CONFIG` 追加
  - §4.x (新) CONFIG レジスタ節追加
  - §10 `probe()` と `IlaCaps` 追加
  - §11 パラメータ一覧に `NUM_CH` 追加
- `OwlTAP_ILA_BSCANE2_使用方法.md`: CONFIG は 2 スキャン読み出し

## 検証ステップ
1. `bazelisk build //src:jtag_viewer` エラー 0
2. `bazelisk test //test:ila_driver_test` 全 pass
3. HDL sim (dsim) デフォルト構成で CONFIG=`0x0117_7C0A` 検証 pass
4. HDL sim BSCANE2 版でも同値検証 pass
5. 実機: IlaPanel に CAPS=`DW=32 Depth=1024` 表示
6. 実機: 従来トリガ + 波形取得機能が回帰なし

## 決定事項
- DATA_W は 6bit に `DATA_W-1` を格納 (値域 1〜64 を有効活用)
- ADDR_W は 8bit にそのまま格納
- NUM_CH は placeholder (現状 1 固定、将来拡張用)
- `kDepth`/`kDataWidth`/`kAddrWidth` static constexpr は **残す** (後方互換)
- probe 失敗時はログ警告 + デフォルト値で継続動作

## スコープ外
- NUM_CH > 1 の本体実装
- エッジ/シーケンストリガ
- Intel FPGA 対応
- IDCODE 本番値化
