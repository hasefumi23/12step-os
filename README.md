# README.md

- http://kozos.jp/books/makeos/
    - 著者によるサポートページ

## 用語集

### 2 nd ステップ シリアル通信

- SCI
    - H8のシリアルコントローラ
- SMR(Serial Mode Register)
    - シリアル入出力のパラメータ設定を行います
    - シリアル通信には、ビット数、パリティ、ストップビット長などの各種パラメータがあります
- SCR(Serial Contoroller Register)
    - シリアル入出力の制御を行います
    - 制御とは、送受信の有効/無効の切り替え、割り込みの有効/無効の切り替えなどです
- BRR(Bit Rate Register)
    - シリアル通信の速度(ボーレート)の設定になります
     code:def.txt
- TDR(トランスミットデータレジスタ)
    - シリアルへの1文字出力を行うのに使う
- SSR(シリアルステータスレジスタ)
    - シリアルへの1文字出力を行うのに使う

### 3 rd ステップ 静的変数の読み書き

- PA(Physical Address) / LA(Load Address) / LMA(Load Memory Address)
    - 物理アドレス
    - 変数の初期値が配置されるアドレス
- LA(Logical Address / Link Address) / VA/VMA(Virtual Address)
    - 論理アドレス / リンクアドレス / 仮想アドレス
    - プログラムが変数にアクセスする際のアドレス
- セクション
    - リンク時に同じ内容の領域をリンカがまとめるためのもの
- セグメント
    - プログラムの実行時にローダが参照してメモリ上に展開するためのもの
