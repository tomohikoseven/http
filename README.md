「ふつうのLinuxプログラミング」の例題(第16章HTTPサーバを作る)を、  
自分好みにリファクタリングした。  
そのときの生成物を置く。

# 主要ファイル
|ファイル名      |概要                                    |
|----------------|----------------------------------------|
| main.c         |   HTTPサーバ本体                       |
| docroot        |   wwwドキュメントルート                |
| docroot/hello.c|   wwwドキュメント                      |
| SConstruct     |   ビルドに必要なファイル               |
| splint.sh      |   splintで静的チェックしようとした残骸 |

# ビルド方法
 main.c および SConstruct があるカレントディレクトリで以下を実行する。  
 $ scons .

# sconsのインストール方法
 $ sudo apt-get install scons  

# 動作例
> andre@andre-VirtualBox:~/work/http$ ./main docroot  
>  
>   Electric Fence 2.1 Copyright (C) 1987-1998 Bruce Perens.  
> GET hello.c HTTP/1.0  
> (Enter)  
> HTTP/1.0 200 OK  
> Date: Sat, 09 Mar 2013 23:37:31 GMT  
> Server: LittleHTTP/1.0  
> Connection: close  
> Content-Length: 71  
> Content-Type: text/plain  
>  
> \#include\<stdio.h\>  
>  
> int  
> main()  
> {  
>     printf("hello\n");  
>     return 0;  
> }
