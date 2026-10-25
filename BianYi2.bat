@echo off
g++ -std=c++11 -O2 -s -mwindows -static -pthread -fexec-charset=GBK KeyboardCursor.cpp -o KeyboardCursor.exe -lcomctl32 -lwinmm -lgdi32 -luser32 -lshell32
pause