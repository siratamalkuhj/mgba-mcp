# mgba-mcp(MCPサーバー内蔵版mGBA)

このフォークは、GBAエミュレーター mGBA に **MCPサーバー** を組み込んだものです。
Claude Code などのAIエージェントが、エミュレーター上のゲームを操作・観察・デバッグできるようになります。

- 英語版の説明: [MCP.md](MCP.md)
- 仕組みの元ネタ: [duckstation-claude-plugin](https://github.com/sadnescity/duckstation-claude-plugin)(PS1エミュレーター DuckStation 向けの同種プロジェクト)

## できること

- ゲームを遊ばせる:ボタン入力、フレーム送り、スクリーンショットで画面確認
- ゲームの中身を調べる:メモリの読み書き、CPUレジスタ、逆アセンブル
- デバッグする:ブレークポイント(特定の命令で止める)、ウォッチポイント(特定のメモリが書き換わったら止める)
- セーブステートの保存と読み込み
- チート:値の検索で所持金やHPのアドレスを突き止め、その値を固定するチートを登録(GameSharkなどのコードも可)

## ダウンロード(Windows)

最新のWindows版は自動ビルドされ、ここに置かれます:
https://github.com/siratamalkuhj/mgba-mcp/releases/tag/windows-latest

`mgba-mcp-windows.zip` をダウンロードして展開してください。中に2種類入っています。

- `mGBA.exe`:普段使う画面付きのmGBA(おすすめ)
- `mgba-headless.exe`:画面なしで動く版(`start-mcp.bat` から使います)

設定は展開したフォルダの中に保存されます(`portable.ini` があるため)。

## 使い方

### 1. エミュレーターを起動する

**画面付き版(mGBA.exe)の場合**

1. `mGBA.exe` を起動します。
2. メニューの「Tools」→「Settings」→「General」を開き、一番下の **「Enable MCP server」** にチェックを入れて「OK」を押します(ポートは通常2347のままでOK)。
3. いつも通りゲームを開きます。画面に `MCP server: http://127.0.0.1:2347/mcp` と一瞬表示されたら準備完了です。

一度チェックを入れれば、次からはゲームを開くだけで自動的にMCPサーバーが動きます。
画面付き版では、ゲームは普通の速さで動き続けます。Claude が `pause` で止めたり、`run_frames` で1コマずつ進めたりできます。

**画面なし版(mgba-headless.exe)の場合**

```sh
mgba-headless --mcp game.gba          # ポート2347で待ち受け
mgba-headless --mcp=2400 game.gba     # ポートを変える場合(「=」が必要)
```

Windowsでは、`.gba` ファイルを `start-mcp.bat` にドラッグ&ドロップしても起動できます。
黒いウィンドウに `MCP server: Listening on http://127.0.0.1:2347/mcp` と表示されたら起動成功です。ウィンドウは閉じないでください。

### 2. Claude Code から接続する

別のターミナルで次を実行します。

```sh
claude mcp add --transport http mgba http://127.0.0.1:2347/mcp
```

### 3. Claude に頼む

たとえば次のように話しかけます。

- 「mgbaのツールでスクリーンショットを撮って、今の画面を説明して」
- 「Startボタンを押してから、もう一度画面を見せて」
- 「0x02000000 からメモリを64バイト読んで」
- 「HPが入っていそうなアドレスにウォッチポイントを置いて、書き換えている命令を探して」
- 「所持金のアドレスを探して、常に9999になるチートを作って」

## 知っておくと便利なこと

- **画面なし版には画面ウィンドウがありません。** 画面は `screenshot` ツールで確認します。
- **画面なし版では、ゲームは一時停止状態で始まります。** 一時停止中は `run_frames`(フレーム送り)や `press_buttons`(ボタンを押して進める)で1手ずつ確実に進められます。
- **画面なし版は、`resume`(再開)すると速度制限なしで動きます。** 通常の60fpsよりずっと速く進みます。画面付き版は通常の速さです。
- 画面付き版でメニューから一時停止している間は、`step` と `run_frames` は使えません。メニューで再開してから、Claude に `pause` してもらってください。
- 安全のため、サーバーは自分のPC(127.0.0.1)からの接続しか受け付けません。

## ツール一覧

| ツール | 内容 |
| --- | --- |
| `get_status` | 実行中か一時停止中か、フレーム数、ゲーム名、現在の命令位置、押しているボタン、最後に止まった理由 |
| `pause` / `resume` | 一時停止 / 再開 |
| `step` | CPU命令を1つずつ実行 |
| `run_frames` | 指定フレーム数だけ進める(ボタンを押しながら進めることも、最後に画面を撮ることも可) |
| `press_buttons` | ボタンを指定フレーム押して離す |
| `set_buttons` | 押しっぱなしにするボタンを指定する |
| `screenshot` | 現在の画面をPNG画像で取得 |
| `reset` | リセット |
| `read_memory` / `write_memory` | メモリの読み書き |
| `list_memory_blocks` | メモリマップ(どのアドレスに何があるか) |
| `get_registers` / `set_register` | CPUレジスタの読み書き |
| `disassemble` | 機械語を人が読めるアセンブリに変換して表示 |
| `add_breakpoint` | 指定アドレスの命令を実行する直前で止める |
| `add_watchpoint` | 指定メモリの読み書き・変化で止める |
| `remove_breakpoint` / `list_breakpoints` | ブレークポイント・ウォッチポイントの削除と一覧 |
| `save_state` / `load_state` | セーブステート(スロット1〜9、またはファイル) |
| `add_cheat` | チートを登録(GameShark / Action Replay / CodeBreaker / VBA のコード、またはアドレスと値を指定して値を固定) |
| `list_cheats` | 登録済みチートの一覧(画面付き版のチート画面で入れたものも含む) |
| `set_cheat_enabled` / `remove_cheat` | チートのオン・オフ切り替え / 削除 |
| `memory_search_start` | 値の検索を開始(画面の数値で検索、または「不明」で全メモリを記録) |
| `memory_search_refine` | 検索結果の絞り込み(同じ値・増えた・減った・変わった・変わらない・N変わった など) |
| `memory_search_results` | 現在の検索候補と、その値の一覧 |

アドレスは数値でも `"0x02000000"` のような文字列でも指定できます。

### チートの作り方(例:所持金を固定する)

Claude に「所持金のアドレスを探して固定して」と頼むと、だいたい次の流れで進めます。

1. 今の所持金(例:500)で `memory_search_start` を実行
2. ゲーム内でお金を使う(例:480になる)→ `memory_search_refine` で480に絞り込み
3. 候補が数個になるまで繰り返す(お金の増減が自分でできない場面では、Claude があなたに操作をお願いすることがあります)
4. `write_memory` で候補を書き換えて、画面の所持金が変わるか確かめる
5. 当たりのアドレスに `add_cheat` で値を固定するチートを登録

ポイント:
- 255を超える数値は2バイトか4バイトで探します(`width` を2か4に)。
- 画面の数値がそのまま入っていないゲームもあります(10で割った値、BCDなど)。その場合は「不明(unknown)」で始めて、「増えた」「減った」で絞り込みます。
- 登録したチートは画面付き版ではゲームごとに保存され、「Tools」→「Cheats」の画面にも出ます(チート画面を開いたままだと表示が更新されないことがあるので、一度閉じて開き直してください)。

### GBAの主なメモリ領域

| アドレス | 内容 |
| --- | --- |
| `0x02000000` | EWRAM(作業用メモリ、256KB) |
| `0x03000000` | IWRAM(高速な作業用メモリ、32KB) |
| `0x04000000` | I/Oレジスタ(画面設定、キー入力など) |
| `0x05000000` | パレット |
| `0x06000000` | VRAM(画面のデータ) |
| `0x07000000` | OAM(スプライト情報) |
| `0x08000000` | ROM(ゲーム本体) |

## 現状と今後の予定

- 動作確認済み:画面なし版(`mgba-headless`)をWindowsの Claude Code から操作
- 追加済み:画面付き版(Qt)の設定画面に「Enable MCP server」とポート番号
- 予定:Claude Code用プラグイン(接続設定と使い方のスキルをまとめたもの)

## ビルド方法

MCP対応はデバッガ機能が有効なら標準でオンです(`-DENABLE_MCP_SERVER=ON`)。

```sh
cmake -B build -DBUILD_HEADLESS=ON
cmake --build build
```

JSONの処理に [cJSON](https://github.com/DaveGamble/cJSON)(MITライセンス)を同梱しています(`src/third-party/cjson`)。
MCPサーバー本体は `src/debugger/mcp-server.c` です。
