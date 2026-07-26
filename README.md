# jarvis-windows-arm64-builder

Windows ARM64ネイティブの実行ファイル3つを、GitHubの公式Windows ARM64ランナーでビルドするための
最小リポジトリです。

- `whisper-cli.exe`
- `clap-vad.exe`
- `jarvis-windows-helper.exe`

配布用JARVIS本体をGitHubへ上げずにビルドできるよう、**ビルドに必要な入力だけ**を切り出してあります。
Whisperモデル、`node_modules`、既存のMac用・Windows x64用バイナリ、yt-dlp、JARVISのUIやアプリ本体は
含まれていません。

## なぜ別リポジトリなのか

配布用JARVISは約610MBあり、`models/whisper/ggml-small.bin`（465MB）のようにGitHubの100MB上限を
超えるファイルを含みます。そのままではpushできません。ビルドに必要なのはC++ソースとビルド定義だけなので、
このフォルダだけをリポジトリにします。

## 含まれるファイル

| ファイル | 役割 |
| --- | --- |
| `.github/workflows/build-windows-arm64.yml` | ビルド本体。`windows-11-vs2026-arm` で実行 |
| `build/windows-arm64/CMakeLists.txt` | `clap-vad` と `jarvis-windows-helper` のビルド定義 |
| `native/clap-vad.cpp` | clap-vadのソース（JARVISと同一ファイル） |
| `platform/windows/native/jarvis-windows-helper.cpp` | helperのソース（JARVISと同一ファイル） |
| `whisper-manifest.json` | 固定するwhisper.cppのコミットなど、ビルドに必要なmanifest情報だけ |
| `scripts/Verify-Arm64Artifacts.ps1` | 成果物の形式・サイズ・SHA-256を確認 |

`native/clap-vad.cpp`、`platform/windows/native/jarvis-windows-helper.cpp`、
`build/windows-arm64/CMakeLists.txt` はJARVIS側と同一内容です。SHA-256を比較すれば一致を確認できます。

## ビルド対象のバージョン

whisper.cppは**固定コミット**で取得します。最新版へ勝手に上げません。

```text
リポジトリ : https://github.com/ggml-org/whisper.cpp
バージョン : v1.8.6
コミット   : 23ee03506a91ac3d3f0071b40e66a430eebdfa1d
```

これは配布用JARVISの`whisper-manifest.json`が記録している値と同一で、
このコミットはwhisper.cppの`release : v1.8.6`そのものです。ワークフローは
`whisper-manifest.json`からコミットを読み取り、checkout後に`git rev-parse HEAD`で一致を再検証します。

## 使い方

### 1. GitHubリポジトリにする

```bash
cd path/to/jarvis-windows-arm64-builder
git init
git add -A
git commit -m "Windows ARM64 native tools builder"
```

GitHub CLIが未導入なら先に導入します。

```bash
brew install gh
gh auth login
```

リポジトリを作成してpushします。

```bash
gh repo create jarvis-windows-arm64-builder --public --source=. --push
```

> **公開／非公開の注意**
> Windows ARM64ランナーは**公開リポジトリなら無料**で使えます。非公開リポジトリで使う場合は
> Team以上のプランが必要です。プランが該当しない場合は`--public`を使ってください。
> このフォルダにはJARVISのアプリ本体や認証情報は含まれていません。

### 2. ビルドを実行する

```bash
gh workflow run build-windows-arm64.yml
gh run watch
```

### 3. 成果物を取得する

```bash
gh run download -n jarvis-windows-arm64-native-tools -D ~/Downloads/arm64-tools
```

### 4. 配布用JARVISへ登録する

JARVISフォルダ側で実行します。

```bash
cd path/to/JARVIS
node scripts/install-windows-arm64-tools.js --plan  ~/Downloads/arm64-tools
node scripts/install-windows-arm64-tools.js --apply ~/Downloads/arm64-tools
```

`--apply`は3ファイルを`tools/win32-arm64/`へ配置し、サイズ・SHA-256・形式を
`whisper-manifest.json`へ登録します。登録後のWindows ARM64では、`--allow-windows-x64`なしで
ネイティブ版が自動選択されます。

## x64版の混入を防ぐ仕組み

x64版をARM64版として配布しないよう、3段構えで止めます。

1. ランナー確認 — `OSArchitecture`が`Arm64`でなければ即座に失敗します。x64ランナーへ切り替えません。
2. ビルド設定 — `CMakeLists.txt`は`-A ARM64`以外で構成されると`FATAL_ERROR`になります。
3. 成果物確認 — `scripts/Verify-Arm64Artifacts.ps1`がPEヘッダーを直接読み、
   Machineが`0xAA64`（ARM64）かつOptionalHeaderが`0x020B`（PE32+）でなければ失敗します。
   x64（`0x8664`）を検出した場合は、その旨を明示して停止します。

ビルド構成を信用せず、**出力ファイルのバイト列を読んで**判定している点が重要です。

## ビルド設定

| 設定 | 理由 |
| --- | --- |
| `-A ARM64` | ランナー上のネイティブターゲット |
| `-T ClangCL` | **必須**。ggmlが `MSVC is not supported for ARM, use clang` で構成を拒否するため。VS同梱のClangCLはMSVC ABIを保つので静的CRTとWindows SDKはそのまま使える |
| `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` | 静的CRT。VC++再頒布パッケージ不要の自己完結バイナリにする |
| `-DBUILD_SHARED_LIBS=OFF` | DLLを持ち回らない |
| `-DGGML_NATIVE=OFF` | ホスト固有の命令セット検出に依存しない |
| `-DGGML_OPENMP=OFF` | `vcomp140.dll`への依存を作らない |
| `-DWHISPER_BUILD_TESTS=OFF` `-DWHISPER_BUILD_SERVER=OFF` | 不要な成果物を作らない |

`jarvis-windows-helper.cpp`は`#pragma comment(lib, ...)`を持たないため、必要なWindowsライブラリは
`CMakeLists.txt`で明示的に指定しています（`gdiplus` `ole32` `oleaut32` `user32` `advapi32`）。

## 起動確認

ワークフローは副作用のない引数だけで起動を確認します。

| ファイル | 確認方法 | 期待 |
| --- | --- | --- |
| `whisper-cli.exe` | `--help` | usage表示 |
| `clap-vad.exe` | 引数なし | 終了コード2＋`usage: clap-vad` |
| `jarvis-windows-helper.exe` | 引数なし | 終了コード2（音声・ウィンドウ・プロセスに触れない） |

`volume get`も実行しますが、ランナーに音声デバイスが無い場合があるため参考情報として扱い、
失敗させません。

## ローカルでの成果物確認

Windows環境があれば、ダウンロードした成果物を直接確認できます。

```powershell
pwsh -File scripts/Verify-Arm64Artifacts.ps1 -Path <ダウンロードしたフォルダ>
```

macOSからは、JARVIS側の`scripts/install-windows-arm64-tools.js --plan`が同じ検査を行います。

## このリポジトリの扱い

ビルド専用の一時リポジトリです。3ファイルを取得してJARVISへ登録したあとは削除して構いません。
whisper.cppのバージョンを上げる場合は、JARVIS側の`whisper-manifest.json`と
このフォルダの`whisper-manifest.json`の`source.commit`を同じ値に更新してください。
