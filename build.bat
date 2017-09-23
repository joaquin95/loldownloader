gcc -std=c11 -Wall -pedantic -Iinclude -Llib -DCURL_STATICLIB loldownloader.c inflate.c -lcurl -lz -lws2_32 -o loldl.exe -Os -s
