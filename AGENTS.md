# AGENTS.md

このリポジトリは Meteorite40 向けの ZMK firmware fork です。
upstream ZMK にない ZMK Studio RPC capability を追加し、Meteorite editor が firmware の公開仕様だけを使って実機設定を編集できるようにします。

## 作業方針

- upstream ZMK との差分は最小に保ち、Meteorite 固有の変更は ZMK Studio RPC / settings / metadata の公開面に限定します。
- upstream 由来の広範な refactor や unrelated formatting は避けてください。
- `upstream` remote は `zmkfirmware/zmk` を指す追従元として維持し、Meteorite fork 側の feature branch へ差分を積みます。
- 現行の Meteorite Studio 連携 branch は `feat/meteorite-custom-config-rpc` です。`zmk-config-meteorite40/config/west.yml` からこの branch を参照します。
- firmware 側 capability が必要な editor 機能は、editor 独自 protocol ではなく ZMK Studio RPC 互換の subsystem / metadata / settings として実装します。
- protobuf schema は `zmk-studio-messages` が正本です。この repo では schema そのものを再定義しません。
- nanopb 生成物は build 時に生成されるため、手で編集しません。

## Branch / Fork 運用

- `origin` は Meteorite fork (`iwk7273/zmk`)、`upstream` は `zmkfirmware/zmk` として扱います。
- Meteorite custom config RPC / encoder metadata の実装は `feat/meteorite-custom-config-rpc` に置きます。
- upstream 追従時は `upstream` から fetch し、この feature branch へ rebase または merge します。
- branch 名を変更する場合は、`zmk-config-meteorite40/config/west.yml`、`zmk-studio-messages` の revision、`zmk-studio-ts-client`、editor の dependency 記述を同時に更新してください。
- `main` へ取り込む場合は、取り込み後に config repo の west manifest も `main` または新しい正本 branch へ切り替えます。

## Meteorite Studio RPC 拡張

- `meteorite` RPC subsystem は `zmk-studio-messages` の subsystem tag `6` に対応します。
- `core.getDeviceInfo.capabilities` には、対応 firmware のみ `meteorite.config` を返します。editor はこの capability を gate にするため、未対応 firmware を壊さないことが重要です。
- `meteorite.getConfigState` は custom config の `fields/current/saved/defaults/dirty` と、rotary encoder 用の `encoderSlots` metadata を返します。
- `meteorite.setConfig` は RAM 上の current のみを更新します。settings への永続化は `meteorite.saveChanges` または firmware 側の save operation に分離します。
- `meteorite.discardChanges` は current を saved へ戻します。
- `core.resetSettings` から Meteorite settings も reset されるよう、`ZMK_RPC_SUBSYSTEM_SETTINGS_RESET` に hook を登録します。

## Rotary Encoder

- encoder binding は専用 settings を持たず、通常 keymap の key position として編集します。
- `meteorite.getConfigState.encoderSlots` は `zmk,behavior-meteorite-encoder` の `slots` devicetree property を正本にして返します。
- editor 側に encoder slot の位置や順序をハードコードさせないため、slot order や左右・CW/CCW の意味を変える場合は schema / metadata / editor の互換性を同時に確認してください。
- encoder action の保存・破棄・リセットは既存 keymap subsystem に乗せます。別の encoder CRUD RPC は、keymap binding から独立させる明確な理由がある場合だけ検討します。

## 関連リポジトリ

- `../zmk-studio-messages`: Studio protobuf schema の正本。
- `../zmk-studio-ts-client`: schema から生成される TypeScript client。
- `../zmk-feature-meteorite-config`: Meteorite custom config state と behavior 実装。
- `../zmk-config-meteorite40`: Meteorite40 shield / keymap / west manifest。
- `../meteorite-studio`: ブラウザ editor。firmware の capability と metadata に基づいて UI を出します。

複数 repo を変更する場合は、repo ごとに差分・検証・commit を分けてください。

## 検証

- ZMK fork 単体ではなく、`zmk-config-meteorite40` の west manifest と組み合わせて firmware build を確認します。
- local west workspace が未初期化の場合は GitHub Actions の firmware build を検証元にしてかまいません。
- Studio RPC を変えた場合は、messages repo と TS client repo の生成・型検査も合わせて確認します。
